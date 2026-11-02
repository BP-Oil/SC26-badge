// SC26 OS "2 PLAYER" - NES games for two badges over a USB-C cable. See twoplayer.h.
//
// Wire format: fixed 16-byte packets  A5 <type> <arg:u32 LE> <data:9> <xor of bytes 1..14>
//   H hello (arg = 1/2 = role, data = badge id)     R ROM offer (arg = CRC32, data = size)
//   N "send it" (P2 hasn't got that ROM)             M name chunk (arg = offset, data = 9 chars)
//   D ROM data (arg = offset, data = 9 bytes)        E end of ROM data (arg = size)
//   K ready (arg = CRC32)   X can't play (P2)        G go: start the emulator now
//   I buttons (arg = frame, d0 = pad, d1 = quit, d2 = frame-skip chosen by PLAYER 1)
//   C RAM checksum (arg = frame, d0..3 = checksum) - catches the emulators drifting apart
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "fatfs/ff.h"
#include "lcd.h"
#include "buttons.h"
#include "leds.h"
#include "ambient.h"
#include "settings.h"
#include "flash_disk.h"
#include "nes_cache.h"
#include "nes_platform.h"
#include "usb_glue.h"
#include "linktest.h"
#include "twoplayer.h"

#define C_BG     RGB565(0, 0, 0)
#define C_TITLE  RGB565(60, 230, 120)
#define C_TEXT   RGB565(235, 235, 235)
#define C_DIM    RGB565(140, 150, 170)
#define C_AMBER  RGB565(255, 190, 80)
#define C_RED    RGB565(255, 90, 60)
#define C_SEL    RGB565(60, 230, 120)

#define DELAY    2                 // input delay in frames (each side sends buttons for frame + 2)
#define RING     64
#define CRC_EVERY 120

static bool host;                  // true on PLAYER 1
static char my_id[5], peer_id[5] = "----";
static uint64_t peer_seen, hello_next;

// ------------------------------------------------------------------ transport (USB serial)
void tp_poll(void) { if (host) tuh_task(); else tud_task(); }

static bool link_up(void) {
    if (host) { int i = linktest_host_cdc(); return i >= 0 && tuh_cdc_mounted((uint8_t)i); }
    return tud_cdc_connected() || tud_mounted();      // DTR or not: a linked badge is all we talk to
}

static uint8_t inbuf[128];
static uint32_t in_n, in_pos;

static int link_getc(void) {
    if (in_pos >= in_n) {
        in_pos = in_n = 0;
        if (host) { int i = linktest_host_cdc(); if (i >= 0 && tuh_cdc_mounted((uint8_t)i)) in_n = tuh_cdc_read((uint8_t)i, inbuf, sizeof inbuf); }
        else if (tud_cdc_available()) in_n = tud_cdc_read(inbuf, sizeof inbuf);
        if (!in_n) return -1;
    }
    return inbuf[in_pos++];
}

static bool bulk_mode;            // sending a ROM: let full 64-byte USB packets go out on their own

static void link_flush(void) {
    if (host) { int i = linktest_host_cdc(); if (i >= 0 && tuh_cdc_mounted((uint8_t)i)) tuh_cdc_write_flush((uint8_t)i); }
    else tud_cdc_write_flush();
}

static bool link_write(const uint8_t *b, uint32_t n) {
    uint64_t until = time_us_64() + 2000000;
    while (n) {
        uint32_t w = 0;
        if (host) {
            int i = linktest_host_cdc();
            if (i < 0 || !tuh_cdc_mounted((uint8_t)i)) return false;
            w = tuh_cdc_write((uint8_t)i, b, n);
            if (!bulk_mode || w < n) tuh_cdc_write_flush((uint8_t)i);
        } else {
            if (!tud_mounted()) return false;
            uint32_t av = tud_cdc_write_available();
            if (av) w = tud_cdc_write(b, n < av ? n : av);
            if (!bulk_mode || w < n) tud_cdc_write_flush();
        }
        b += w; n -= w;
        if (n) { tp_poll(); if (time_us_64() > until) return false; }
    }
    return true;
}

// ------------------------------------------------------------------ packets
typedef struct __attribute__((packed)) { uint8_t sync, type; uint32_t arg; uint8_t d[9]; uint8_t x; } pkt_t;
_Static_assert(sizeof(pkt_t) == 16, "packet size");

static bool pkt_send(uint8_t type, uint32_t arg, const void *d, int n) {
    pkt_t p; memset(&p, 0, sizeof p);
    p.sync = 0xA5; p.type = type; p.arg = arg;
    if (d) memcpy(p.d, d, n > 9 ? 9 : (size_t)n);
    const uint8_t *b = (const uint8_t *)&p;
    for (int i = 1; i < 15; i++) p.x ^= b[i];
    return link_write(b, sizeof p);
}

static uint8_t rx[16];
static int rx_n;

// --- NES lockstep state (filled by the packet pump)
typedef struct { uint32_t f; uint8_t pad, quit, skip; } inp_t;
static inp_t loc[RING], rem[RING];
static bool nes_on, desync;
static uint32_t nf;
static uint32_t crc_mine[4], crc_peer[4], crc_f_mine[4] = {~0u, ~0u, ~0u, ~0u}, crc_f_peer[4] = {~0u, ~0u, ~0u, ~0u};

static void crc_check(int k) {
    if (crc_f_mine[k] == crc_f_peer[k] && crc_f_mine[k] != ~0u && crc_mine[k] != crc_peer[k]) desync = true;
}

// Reads whatever has arrived. Hello / buttons / checksums are handled here; anything else is
// returned to the caller (true + *out).
static bool pump(pkt_t *out) {
    int c;
    while ((c = link_getc()) >= 0) {
        if (rx_n == 0 && c != 0xA5) continue;
        rx[rx_n++] = (uint8_t)c;
        if (rx_n < 16) continue;
        uint8_t x = 0;
        for (int i = 1; i < 15; i++) x ^= rx[i];
        if (x != rx[15]) {                                     // resync on the next 0xA5
            int k = 1;
            while (k < 16 && rx[k] != 0xA5) k++;
            memmove(rx, rx + k, (size_t)(16 - k)); rx_n = 16 - k;
            continue;
        }
        pkt_t p; memcpy(&p, rx, sizeof p); rx_n = 0;
        switch (p.type) {
        case 'H':
            peer_seen = time_us_64();
            memcpy(peer_id, p.d, 4); peer_id[4] = 0;
            break;
        case 'I':
            if (nes_on) { inp_t *r = &rem[p.arg % RING]; r->f = p.arg; r->pad = p.d[0]; r->quit = p.d[1]; r->skip = p.d[2]; }
            break;
        case 'C':
            if (nes_on) {
                int k = (int)((p.arg / CRC_EVERY) % 4);
                crc_f_peer[k] = p.arg; memcpy(&crc_peer[k], p.d, 4);
                crc_check(k);
            }
            break;
        default:
            *out = p;
            return true;
        }
    }
    return false;
}

static void hello(void) {
    if (time_us_64() < hello_next || !link_up()) return;
    hello_next = time_us_64() + 300000;
    pkt_send('H', host ? 1 : 2, my_id, 4);
}

static bool peer_here(void) { return peer_seen && time_us_64() - peer_seen < 1500000; }

// Wait for one of the given packet types (others are dropped). false on timeout / B pressed.
static bool wait_for(const char *types, pkt_t *out, uint32_t timeout_ms) {
    uint64_t until = time_us_64() + (uint64_t)timeout_ms * 1000;
    while (time_us_64() < until) {
        tp_poll(); hello();
        if (buttons_read() & BTN_B) return false;
        pkt_t p;
        while (pump(&p)) if (strchr(types, p.type)) { *out = p; return true; }
    }
    return false;
}

// ------------------------------------------------------------------ NES lockstep
bool tp_nes_active(void) { return nes_on; }

static void nes_begin(void) {
    memset(loc, 0xFF, sizeof loc); memset(rem, 0xFF, sizeof rem);
    for (uint32_t f = 0; f < DELAY; f++) {
        loc[f] = (inp_t){ f, 0, 0, 0 };
        rem[f] = (inp_t){ f, 0, 0, 0 };
    }
    for (int k = 0; k < 4; k++) crc_f_mine[k] = crc_f_peer[k] = ~0u;
    nf = 0; desync = false; nes_on = true;
}

static void wait_msg(bool on) {
    const int y = 300;
    lcd_fill_rect(60, y, 240, 20, C_BG);
    if (on) {
        const char *s = host ? "WAITING FOR PLAYER 2" : "WAITING FOR PLAYER 1";
        lcd_draw_text((LCD_WIDTH - lcd_text_width(s, 1)) / 2, y, s, C_AMBER, C_BG, 1);
    }
}

bool tp_nes_exchange(uint8_t pad, bool quit, uint8_t want_skip, uint32_t ram_crc,
                     uint8_t *pad1, uint8_t *pad2, bool *quit_any, uint8_t *skip, bool *ds) {
    uint32_t f = nf++;
    uint32_t t = f + DELAY;
    loc[t % RING] = (inp_t){ t, pad, (uint8_t)quit, host ? want_skip : 0 };
    uint8_t d[3] = { pad, (uint8_t)quit, host ? want_skip : 0 };
    if (!pkt_send('I', t, d, 3)) return false;
    if (f % CRC_EVERY == 0) {
        int k = (int)((f / CRC_EVERY) % 4);
        crc_f_mine[k] = f; crc_mine[k] = ram_crc;
        pkt_send('C', f, &ram_crc, 4);
        crc_check(k);
    }
    uint64_t t0 = time_us_64();
    bool shown = false;
    while (rem[f % RING].f != f) {
        tp_poll();
        pkt_t p; pump(&p);
        uint64_t w = time_us_64() - t0;
        if (w > 300000 && !shown) { wait_msg(true); shown = true; }
        if (w > 8000000) { wait_msg(false); return false; }     // other badge gone
    }
    if (shown) wait_msg(false);
    const inp_t *me = &loc[f % RING], *them = &rem[f % RING];
    *pad1 = host ? me->pad : them->pad;
    *pad2 = host ? them->pad : me->pad;
    *quit_any = me->quit || them->quit;
    *skip = host ? me->skip : them->skip;                       // PLAYER 1 decides, both apply
    *ds = desync;
    return true;
}

static void play(const uint8_t *rom) {
    settings_t cfg;
    bool have = settings_load(&cfg);
    led_mode_t prev = ambient_mode();
    if (!have || cfg.led_mode != LEDMODE_OFF) ambient_set_mode(LEDMODE_AMBIENT);
    nes_begin();
    nes_run(rom);
    nes_on = false;
    ambient_set_mode(prev);
    leds_fill(0, 0, 0); leds_show();
    while (buttons_read()) { tp_poll(); sleep_ms(5); }
}

// ------------------------------------------------------------------ screens
static void text_c(int y, const char *s, uint16_t fg, int scale) {
    lcd_draw_text((LCD_WIDTH - lcd_text_width(s, scale)) / 2, y, s, fg, C_BG, scale);
}

static void clear_line(int y) { lcd_fill_rect(30, y - 2, 300, 22, C_BG); }

static void status_line(int y, const char *s, uint16_t c) { clear_line(y); text_c(y, s, c, 1); }

static void make_id(void) {
    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);
    snprintf(my_id, sizeof my_id, "%02X%02X", uid.id[6], uid.id[7]);
}

static void drain(void) { pkt_t p; for (int i = 0; i < 50; i++) { tp_poll(); while (pump(&p)) {} sleep_ms(2); } }

// ------------------------------------------------------------------ PLAYER 2 (normal OS)
static char rx_name[64];
static uint32_t rx_expect, rx_size_total;

static void p2_draw(void) {
    lcd_blit_wait();
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    text_c(34, "2 PLAYER", C_TITLE, 2);
    text_c(80, "PLAYER 2", C_TEXT, 1);
    text_c(260, "A: be PLAYER 1  B: back", C_DIM, 1);
}

static void p2_status(void) {
    static int last = -1;
    int st = peer_here() ? 2 : link_up() ? 1 : 0;
    if (st == last) return;
    last = st;
    clear_line(140); clear_line(166);
    if (st == 2) {
        char s[32]; snprintf(s, sizeof s, "LINKED TO %s", peer_id);
        text_c(140, s, C_TITLE, 1);
        text_c(166, "PLAYER 1 picks the game", C_DIM, 1);
    } else if (st == 1) {
        text_c(140, "CABLE OK", C_AMBER, 1);
        text_c(166, "open 2 PLAYER on the other", C_DIM, 1);
    } else {
        text_c(140, "USB-C cable to the badge", C_TEXT, 1);
        text_c(166, "that will be PLAYER 1", C_TEXT, 1);
    }
    if (st != 2) leds_fill(0, 0, 0); else leds_fill(0, 20, 0);
    leds_show();
}

static bool find_rom(uint32_t crc, uint32_t size, char *path, size_t cap) {
    DIR dir; FILINFO fi;
    bool found = false;
    if (f_opendir(&dir, "GAMES") != FR_OK) return false;
    while (!found && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        size_t n = strlen(fi.fname);
        if ((fi.fattrib & AM_DIR) || fi.fname[0] == '.' || n < 5 || strcasecmp(fi.fname + n - 4, ".nes")) continue;
        if ((uint32_t)fi.fsize != size) continue;
        snprintf(path, cap, "GAMES/%s", fi.fname);
        uint32_t sz = 0;
        found = nescache_file_crc(path, &sz) == crc && sz == size;
    }
    f_closedir(&dir);
    return found;
}

static void p2_offer(const pkt_t *p) {
    uint32_t crc = p->arg, size;
    memcpy(&size, p->d, 4);
    char path[96], err[40] = "";
    lcd_fill_rect(30, 120, 300, 70, C_BG);
    status_line(140, "PLAYER 1 PICKED A GAME", C_TEXT);
    const uint8_t *rom = NULL;
    if (find_rom(crc, size, path, sizeof path)) {
        status_line(166, "loading ...", C_DIM);
        rom = nescache_load(path, err, sizeof err, NULL);
        if (!rom) { status_line(166, err, C_RED); pkt_send('X', crc, NULL, 0); sleep_ms(1500); p2_draw(); return; }
    } else {
        // not on this badge: PLAYER 1 sends it
        if (!nescache_rx_begin(size, crc)) { status_line(166, "GAME TOO BIG", C_RED); pkt_send('X', crc, NULL, 0); sleep_ms(1500); p2_draw(); return; }
        memset(rx_name, 0, sizeof rx_name);
        rx_expect = 0; rx_size_total = size;
        pkt_send('N', crc, NULL, 0);
        status_line(166, "receiving 0%", C_AMBER);
        uint32_t shown = 0;
        pkt_t q;
        while (wait_for("MDE", &q, 8000)) {
            if (q.type == 'M') { if (q.arg < sizeof rx_name - 9) memcpy(rx_name + q.arg, q.d, 9); rx_name[sizeof rx_name - 1] = 0; }
            else if (q.type == 'D') {
                if (q.arg != rx_expect) break;
                uint32_t n = size - q.arg < 9 ? size - q.arg : 9;
                if (!nescache_rx_data(q.d, n)) break;
                rx_expect += n;
                uint32_t pc = nescache_rx_progress();
                if (pc >= shown + 5) { shown = pc; char s[24]; snprintf(s, sizeof s, "receiving %lu%%", (unsigned long)pc); status_line(166, s, C_AMBER); }
            } else if (q.type == 'E') {
                rom = nescache_rx_end(rx_name, err, sizeof err);
                break;
            }
        }
        if (!rom) { status_line(166, err[0] ? err : "TRANSFER FAILED", C_RED); pkt_send('X', crc, NULL, 0); sleep_ms(2000); p2_draw(); return; }
        // keep a copy in GAMES
        char clean[64]; int k = 0;
        for (const char *s = rx_name; *s && k < 58; s++) clean[k++] = (*s == '/' || *s == '\\' || *s == ':') ? '_' : *s;
        clean[k] = 0;
        if (k < 5 || strcasecmp(clean + k - 4, ".nes")) strcat(clean, ".nes");
        snprintf(path, sizeof path, "GAMES/%s", clean);
        status_line(166, "saving to GAMES ...", C_DIM);
        FILINFO fi;
        if (f_stat(path, &fi) != FR_OK) nescache_save_to(path);
        flashdisk_flush();
    }
    pkt_send('K', crc, NULL, 0);
    status_line(166, "ready - starting!", C_TITLE);
    pkt_t g;
    if (!wait_for("G", &g, 15000)) { p2_draw(); return; }
    play(rom);
    drain();
    p2_draw();
}

void twoplayer_p2_run(void) {
    host = false;
    make_id();
    usb_console(false);
    peer_seen = 0; rx_n = 0; in_n = in_pos = 0;
    tud_cdc_read_flush();
    p2_draw();
    uint32_t prev = buttons_read();
    while (true) {
        tp_poll(); hello();
        pkt_t p;
        while (pump(&p)) if (p.type == 'R') p2_offer(&p);
        p2_status();
        uint32_t b = buttons_read(), down = b & ~prev;
        prev = b;
        if (down & BTN_B) break;
        if (down & BTN_A) linktest_become_player1();
        sleep_ms(2);
    }
    leds_fill(0, 0, 0); leds_show();
    usb_console(true);
    while (buttons_read()) { tud_task(); sleep_ms(5); }
}

// ------------------------------------------------------------------ PLAYER 1 (host-mode restart)
#define MAX_GAMES 40
static char games[MAX_GAMES][64];
static int ngames, sel, top;
#define ROWS 6

static void list_games(void) {
    ngames = 0;
    DIR dir; FILINFO fi;
    if (f_opendir(&dir, "GAMES") != FR_OK) return;
    while (ngames < MAX_GAMES && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        size_t n = strlen(fi.fname);
        if ((fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) || fi.fname[0] == '.' || n < 5 || strcasecmp(fi.fname + n - 4, ".nes")) continue;
        snprintf(games[ngames++], sizeof games[0], "%s", fi.fname);
    }
    f_closedir(&dir);
    // simple sort
    for (int i = 1; i < ngames; i++)
        for (int j = i; j > 0 && strcasecmp(games[j - 1], games[j]) > 0; j--) {
            char t[64]; memcpy(t, games[j], 64); memcpy(games[j], games[j - 1], 64); memcpy(games[j - 1], t, 64);
        }
}

static int nitems(void) { return ngames; }

static void p1_draw(void) {
    lcd_blit_wait();
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    text_c(30, "2 PLAYER", C_TITLE, 2);
    text_c(72, "PLAYER 1  pick a game", C_TEXT, 1);
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    for (int r = 0; r < ROWS && top + r < nitems(); r++) {
        int i = top + r, y = 128 + r * 25;
        char label[34];
        snprintf(label, sizeof label, "%.*s", (int)(strlen(games[i]) - 4 > 26 ? 26 : strlen(games[i]) - 4), games[i]);
        for (char *c = label; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        bool s = i == sel;
        if (s) lcd_fill_rect(50, y - 4, LCD_WIDTH - 100, 23, C_SEL);
        lcd_draw_text((LCD_WIDTH - lcd_text_width(label, 1)) / 2, y, label, s ? C_BG : C_TEXT, s ? C_SEL : C_BG, 1);
    }
    if (!ngames) text_c(128, "no .nes files in GAMES", C_DIM, 1);
    text_c(300, "A: PLAY   B: EXIT", C_DIM, 1);
}

static void p1_status(bool force) {
    static int last = -1;
    int st = peer_here() ? 2 : link_up() ? 1 : 0;
    if (st == last && !force) return;
    last = st;
    char s[32];
    if (st == 2) snprintf(s, sizeof s, "LINKED TO %s", peer_id);
    else if (st == 1) snprintf(s, sizeof s, "open 2 PLAYER on badge 2");
    else snprintf(s, sizeof s, "connect PLAYER 2 (USB-C)");
    status_line(100, s, st == 2 ? C_TITLE : C_AMBER);
    leds_fill(0, st == 2 ? 20 : 0, 0); leds_show();
}

static void p1_progress(int pct) {
    char s[24]; snprintf(s, sizeof s, "copying %d%%", pct > 100 ? 100 : pct);
    status_line(200, s, C_DIM);
    tuh_task();
}

static void p1_message(const char *a, const char *b, uint16_t c) {
    lcd_fill_rect(30, 120, 300, 170, C_BG);
    text_c(160, a, c, 1);
    if (b) text_c(186, b, C_DIM, 1);
}

static void p1_start(const char *file) {
    char path[96], err[40] = "";
    snprintf(path, sizeof path, "GAMES/%s", file);
    p1_message("LOADING", file, C_TEXT);
    const uint8_t *rom = nescache_load(path, err, sizeof err, p1_progress);
    uint32_t size = 0, crc = 0;
    if (!rom || !nescache_current(&size, &crc)) { p1_message("CAN'T LOAD", err, C_RED); sleep_ms(2000); return; }
    drain();
    pkt_send('R', crc, &size, 4);
    p1_message("ASKING PLAYER 2 ...", NULL, C_TEXT);
    pkt_t p;
    if (!wait_for("KNX", &p, 15000)) { p1_message("NO ANSWER", "from PLAYER 2", C_RED); sleep_ms(2000); return; }
    if (p.type == 'N') {
        // send the game: name, then the data in 9-byte pieces
        for (uint32_t o = 0; o < strlen(file) + 1 && o < 63; o += 9) pkt_send('M', o, file + o, 9);
        char s[32];
        uint32_t shown = 101;
        bulk_mode = true;
        for (uint32_t o = 0; o < size; o += 9) {
            uint32_t n = size - o < 9 ? size - o : 9;
            if (!pkt_send('D', o, rom + o, (int)n)) break;
            uint32_t pc = o * 100 / size;
            if (pc / 5 != shown / 5) { shown = pc; snprintf(s, sizeof s, "sending to PLAYER 2 %lu%%", (unsigned long)pc); p1_message(s, file, C_AMBER); }
            if ((o & 0x3FF) < 9) tuh_task();
        }
        bulk_mode = false;
        link_flush();
        pkt_send('E', size, NULL, 0);
        p1_message("PLAYER 2 is saving it ...", NULL, C_TEXT);
        if (!wait_for("KX", &p, 60000)) { p1_message("NO ANSWER", "from PLAYER 2", C_RED); sleep_ms(2000); return; }
    }
    if (p.type != 'K') { p1_message("PLAYER 2 CAN'T PLAY IT", NULL, C_RED); sleep_ms(2000); return; }
    pkt_send('G', crc, NULL, 0);
    play(rom);
    drain();
}

void twoplayer_p1_run(void) {
    host = true;
    make_id();
    peer_seen = 0; rx_n = 0; in_n = in_pos = 0;
    list_games();
    sel = top = 0;
    p1_draw(); p1_status(true);
    uint32_t prev = buttons_read();
    while (true) {
        tp_poll(); hello();
        pkt_t p;
        while (pump(&p)) {}
        p1_status(false);
        uint32_t b = buttons_read(), down = b & ~prev;
        prev = b;
        if (down & BTN_B) break;
        if (ngames && (down & BTN_UP))   { sel = (sel + nitems() - 1) % nitems(); p1_draw(); p1_status(true); }
        if (ngames && (down & BTN_DOWN)) { sel = (sel + 1) % nitems(); p1_draw(); p1_status(true); }
        if (down & (BTN_A | BTN_RIGHT)) {
            if (!ngames) {}
            else if (!peer_here()) { status_line(100, "PLAYER 2 NOT READY", C_RED); sleep_ms(1200); }
            else p1_start(games[sel]);
            prev = buttons_read();
            p1_draw(); p1_status(true);
        }
        sleep_ms(2);
    }
    leds_fill(0, 0, 0); leds_show();
}
