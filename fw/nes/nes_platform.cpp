// InfoNES platform layer for the SC26 board.
//
// Video : InfoNES renders NES scanlines 4..235 one at a time into a small ring of line buffers
//         (RGB565 big-endian via NesPalette). Each finished line is DMA'd straight to the round
//         panel inside one open pixel-write window (256x232, centred 1:1, fully inside the circle),
//         and copied into the shared framebuffer so the LED ring can follow the picture.
// Input : joystick = D-pad, A = A, B = B, CENTER tap = START, CENTER + B (or CENTER + DOWN) =
//         SELECT,
//         hold CENTER 1.5 s = quit back to the SC26 menu.
// Timing: paced to the NES NTSC rate (60.1 Hz); automatic frame-skip if emulation falls behind.
// Sound : the board has no speaker, so the APU is muted.
// 2 PLAYER: while twoplayer.c runs a linked game, the buttons of both badges are exchanged every
//         frame (tp_nes_exchange) and PLAYER 1 decides the frame-skip for both, so the two
//         emulators stay bit-identical. USB is serviced through tp_poll() instead of tud_task().

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "pico/stdlib.h"
#include "tusb.h"

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_pAPU.h"

extern "C" {
#include "lcd.h"
#include "buttons.h"
#include "ambient.h"
#include "gif_player.h"     // gp_fb: shared arena (NES mapper buffers live there while a game runs)
#include "nes_platform.h"
#include "twoplayer.h"   // 2-player lockstep over the USB-C link
}

extern int APU_Mute;

static constexpr int NES_X = (LCD_WIDTH - NES_DISP_WIDTH) / 2;      // 52
static constexpr int FIRST_LINE = 4, LAST_LINE = 235;                 // InfoNES draws 4..235
static constexpr int NES_Y = (LCD_HEIGHT - (LAST_LINE - FIRST_LINE + 1)) / 2;   // 64
static constexpr uint64_t FRAME_US = 16639;                          // 60.0988 Hz

// NES palette (same RGB555 source values as pico-infones), stored as big-endian RGB565.
static constexpr uint16_t C(uint16_t x) {
    uint16_t r = (x >> 10) & 31, g = (x >> 5) & 31, b = x & 31;
    uint16_t v = (uint16_t)((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
    return (uint16_t)((v >> 8) | (v << 8));
}
const WORD __not_in_flash_func(NesPalette)[64] = {
    C(0x39ce), C(0x1071), C(0x0015), C(0x2013), C(0x440e), C(0x5402), C(0x5000), C(0x3c20),
    C(0x20a0), C(0x0100), C(0x0140), C(0x00e2), C(0x0ceb), C(0x0000), C(0x0000), C(0x0000),
    C(0x5ef7), C(0x01dd), C(0x10fd), C(0x401e), C(0x5c17), C(0x700b), C(0x6ca0), C(0x6521),
    C(0x45c0), C(0x0240), C(0x02a0), C(0x0247), C(0x0211), C(0x0000), C(0x0000), C(0x0000),
    C(0x7fff), C(0x1eff), C(0x2e5f), C(0x223f), C(0x79ff), C(0x7dd6), C(0x7dcc), C(0x7e67),
    C(0x7ae7), C(0x4342), C(0x2769), C(0x2ff3), C(0x03bb), C(0x0000), C(0x0000), C(0x0000),
    C(0x7fff), C(0x579f), C(0x635f), C(0x6b3f), C(0x7f1f), C(0x7f1b), C(0x7ef6), C(0x7f75),
    C(0x7f94), C(0x73f4), C(0x57d7), C(0x5bf9), C(0x4ffe), C(0x0000), C(0x0000), C(0x0000)};

static WORD __attribute__((aligned(4))) line_buf[4][NES_DISP_WIDTH + 16];
static constexpr int THUMB_W = NES_DISP_WIDTH / 8, THUMB_H = (LAST_LINE - FIRST_LINE + 1) / 8;   // 32 x 29
static uint16_t thumb[THUMB_H][THUMB_W];          // every 8th pixel of every 8th line, for the LED ring
static const uint8_t *g_rom;
static int  g_menu_calls;
static bool g_stream_open;
static int  g_next_line;
static uint64_t g_next_frame_us;
static int  g_frames, g_late_frames;
static uint64_t g_fps_t0;
static float g_fps;
static uint64_t g_hint_until;
static uint8_t  g_want_skip;       // 2 PLAYER: frame-skip PLAYER 1 asks for (applied by both)
static uint32_t g_link_frame;
static bool     g_desync_shown;

static void usb_poll(void) { if (tp_nes_active()) tp_poll(); else tud_task(); }


// ---------------------------------------------------------------- ROM
static bool check_magic(const BYTE *id) { return id[0] == 'N' && id[1] == 'E' && id[2] == 'S' && id[3] == 0x1A; }

static bool parse_rom(const uint8_t *nes) {
    memcpy(&NesHeader, nes, sizeof NesHeader);
    if (!check_magic(NesHeader.byID)) return false;
    nes += sizeof NesHeader;
    memset(SRAM, 0, SRAM_SIZE);
    if (NesHeader.byInfo1 & 4) { memcpy(&SRAM[0x1000], nes, 512); nes += 512; }   // trainer
    ROM = (BYTE *)nes;
    nes += NesHeader.byRomSize * 0x4000;
    VROM = NesHeader.byVRomSize ? (BYTE *)nes : nullptr;
    return true;
}

int InfoNES_ReadRom(const char *) { return -1; }        // ROMs are memory-mapped, never "read"
void InfoNES_ReleaseRom() { ROM = nullptr; VROM = nullptr; }

int InfoNES_Menu() {
    // Called before each emulation run: run exactly once, then quit back to the caller.
    if (g_menu_calls++) return -1;
    if (!parse_rom(g_rom) || InfoNES_Reset() < 0) return -1;
    APU_Mute = 1;
    return 0;
}

// ---------------------------------------------------------------- video
void __not_in_flash_func(InfoNES_PreDrawLine)(int line) {
    InfoNES_SetLineBuffer(line_buf[line & 3], NES_DISP_WIDTH + 16);
}

void __not_in_flash_func(InfoNES_PostDrawLine)(int line) {
    if (!g_stream_open || line != g_next_line) {
        if (g_stream_open) lcd_stream_end();
        lcd_stream_begin(NES_X, NES_Y + (line - FIRST_LINE), NES_DISP_WIDTH, LAST_LINE - line + 1);
        g_stream_open = true;
    }
    const WORD *src = line_buf[line & 3];
    lcd_stream_push(src, NES_DISP_WIDTH * 2);
    int rel = line - FIRST_LINE;
    if ((rel & 7) == 4 && (rel >> 3) < THUMB_H)
        for (int i = 0; i < THUMB_W; i++) thumb[rel >> 3][i] = src[i * 8 + 4];
    g_next_line = line + 1;
    if (line >= LAST_LINE) { lcd_stream_end(); g_stream_open = false; }
}

static void draw_hint(void) {
    const char *s = "HOLD CENTER TO EXIT";
    lcd_draw_text((LCD_WIDTH - lcd_text_width(s, 1)) / 2, NES_Y + 236, s, RGB565(150, 160, 180), 0, 1);
}

void InfoNES_LoadFrame() {
    if (g_stream_open) { lcd_stream_end(); g_stream_open = false; }
    int skip = FrameSkip + 1;

    // LEDs: follow the picture
    ambient_sample_small(&thumb[0][0], THUMB_W, THUMB_H, 8);
    ambient_tick();
    usb_poll();

    if (g_hint_until && time_us_64() > g_hint_until) {
        g_hint_until = 0;
        lcd_fill_rect(40, NES_Y + 234, LCD_WIDTH - 80, 24, 0);
    }

    // Pace to 60.1 Hz. If rendered frames keep arriving late, skip rendering every other
    // (then every third) frame; drop back once we have had ~2 s of headroom.
    uint64_t now = time_us_64();
    g_next_frame_us += FRAME_US * skip;
    bool late = now > g_next_frame_us;
    if (!late) {
        while (time_us_64() < g_next_frame_us) tight_loop_contents();
    } else if (now > g_next_frame_us + 4 * FRAME_US) {
        g_next_frame_us = now;                                    // far behind: resync
    }
    // In a 2-player game the skip is only *requested* here (g_want_skip); InfoNES_PadState
    // applies PLAYER 1's choice on the same frame on both badges.
    int cur = tp_nes_active() ? g_want_skip : FrameSkip;
    if (late) { if (++g_late_frames > 10 && cur < 2) { cur++; g_late_frames = 0; } }
    else      { if (--g_late_frames < -120 && cur > 0) { cur--; g_late_frames = 0; } }
    if (tp_nes_active()) g_want_skip = (uint8_t)cur; else FrameSkip = (WORD)cur;

    if (++g_frames >= 60) {
        uint64_t t = time_us_64();
        g_fps = g_frames * skip * 1e6f / (float)(t - g_fps_t0);
        g_fps_t0 = t; g_frames = 0;
        printf("nes: %.1f fps (skip %d)\n", (double)g_fps, FrameSkip);
    }
}

// ---------------------------------------------------------------- input
void InfoNES_PadState(DWORD *pad1, DWORD *pad2, DWORD *system) {
    enum { A = 1, B = 2, SELECT = 4, START = 8, UP = 16, DOWN = 32, LEFT = 64, RIGHT = 128 };
    static uint64_t center_t0;
    static bool center_was, chord;
    static int start_pulse, select_pulse;
    uint32_t b = buttons_read();
    uint64_t now = time_us_64();
    DWORD v = 0;
    bool center = b & BTN_CENTER;

    if (center && !center_was) { center_t0 = now; chord = false; }
    // SELECT: hold CENTER and press B (easy with two thumbs) or DOWN
    if (center && (b & (BTN_DOWN | BTN_B)) && !chord) { chord = true; select_pulse = 6; }
    *system = 0;
    if (center && now - center_t0 > 1500000) *system = PAD_SYS_QUIT;
    if (!center && center_was && !chord && now - center_t0 < 1500000) start_pulse = 6;
    center_was = center;

    if (b & BTN_UP)    v |= UP;
    if ((b & BTN_DOWN) && !center) v |= DOWN;
    if (b & BTN_LEFT)  v |= LEFT;
    if (b & BTN_RIGHT) v |= RIGHT;
    if (b & BTN_A)     v |= A;
    if ((b & BTN_B) && !center) v |= B;           // B with CENTER held is SELECT, not B
    if (start_pulse)  { v |= START;  start_pulse--; }
    if (select_pulse) { v |= SELECT; select_pulse--; }
    if (tp_nes_active()) {
        // checksum of NES RAM every 120 frames lets both badges spot a desync
        uint32_t crc = 0;
        if (g_link_frame % 120 == 0) {
            const uint32_t *w = (const uint32_t *)RAM;
            crc = 2166136261u;
            for (int i = 0; i < RAM_SIZE / 4; i++) crc = (crc ^ w[i]) * 16777619u;
        }
        g_link_frame++;
        uint8_t p1, p2, skip; bool quit_any, ds;
        if (!tp_nes_exchange((uint8_t)v, *system & PAD_SYS_QUIT, g_want_skip, crc, &p1, &p2, &quit_any, &skip, &ds)) {
            *pad1 = *pad2 = 0;
            *system = PAD_SYS_QUIT;                      // the other badge went away
            return;
        }
        *pad1 = p1; *pad2 = p2;
        *system = quit_any ? PAD_SYS_QUIT : 0;
        FrameSkip = skip;
        if (ds && !g_desync_shown) {
            g_desync_shown = true;
            const char *m = "DESYNC - restart game";
            lcd_draw_text((LCD_WIDTH - lcd_text_width(m, 1)) / 2, NES_Y + 236, m, RGB565(255, 90, 60), 0, 1);
        }
        return;
    }
    *pad1 = v;
    *pad2 = 0;
}

// ---------------------------------------------------------------- misc callbacks
void InfoNES_DebugPrint(const char *msg) { printf("%s", msg); }
void InfoNES_MessageBox(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("[nes] "); vprintf(fmt, ap); printf("\n"); va_end(ap);
}
void InfoNES_SoundInit() {}
int  InfoNES_SoundOpen(int, int) { return 0; }
void InfoNES_SoundClose() {}
int  InfoNES_GetSoundBufferSize() { return 0; }
void InfoNES_SoundOutput(int, BYTE *, BYTE *, BYTE *, BYTE *, BYTE *) {}

// ---------------------------------------------------------------- entry point
extern "C" int nes_run(const uint8_t *rom) {
    g_rom = rom;
    g_menu_calls = 0;
    g_stream_open = false;
    g_next_line = -1;
    g_frames = 0; g_late_frames = 0;
    g_fps_t0 = g_next_frame_us = time_us_64();
    memset(gp_fb, 0, SC26_ARENA_BYTES);                    // mapper RAM starts zeroed, as on hardware
    memset(thumb, 0, sizeof thumb);
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
    draw_hint();
    g_hint_until = time_us_64() + 3000000;
    g_want_skip = 0; g_link_frame = 0; g_desync_shown = false;
    if (tp_nes_active()) { FrameSkip = 0; g_hint_until = time_us_64() + 3000000; }
    while (buttons_read() & BTN_CENTER) usb_poll();        // don't inherit the press that started us
    InfoNES_Main();
    if (g_stream_open) { lcd_stream_end(); g_stream_open = false; }
    return 0;
}
