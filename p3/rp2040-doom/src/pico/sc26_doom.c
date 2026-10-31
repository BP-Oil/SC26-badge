// SC26 badge port glue for rp2040-doom. See sc26_doom.h.
//
// Copyright(C) 2026 SC26 badge port. GPL-2.0-or-later, like the rest of this tree.

#if SC26_BOARD
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/xip_cache.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/syscfg.h"
#include "hardware/structs/nvic.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "pico/platform.h"
#include "sc26_doom.h"
#include "board_sc26.h"
#include "st77916_init.h"
#include "qspi_lcd.pio.h"
#include "ws2812.pio.h"

#include "doomkeys.h"
#include "d_event.h"
#include "doom/doomstat.h"
#include "doom/d_items.h"
#include "doom/d_player.h"

// ------------------------------------------------------------------ SC26 OS settings (read-only)
// Same layout as SC26 OS settings.c; read through the untranslated XIP alias because this
// image runs with the flash window shifted to its own slot.
#define SC26_SETTINGS_PHYS 0x2FF000u
typedef struct { uint8_t stay, led_mode, brightness, zoom, ring_cw; char gif[96]; } os_settings_t;
typedef struct { uint32_t magic, version; os_settings_t s; uint32_t crc; } os_stored_t;
static uint8_t os_brightness = 100, os_led_mode = 1, os_ring_cw = 0;   // rocket: 8,0,1,2 on the left

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}

// LED-scenes record written by SC26 OS at offset 512 of the same sector (fw/gifplayer/settings.c).
// When DOOM is started in SCENES mode, SC26 OS records the scene as a short seamless loop right
// after it: u16 interval_ms, u16 n, n x 27 bytes (physical LED order, ready to send), then a
// crc of the loop. Playing it back needs no floating point, which this build doesn't have.
#define SC26_SCENES_MAGIC 0x454E4353u            // "SCNE"
typedef struct { char scene[24]; uint8_t speed, bright; uint16_t blob_len; } os_scene_rec_t;
typedef struct { uint32_t magic, version; os_scene_rec_t r; uint32_t crc; } os_stored_scene_t;
static const uint8_t *scene_loop;               // frames, read straight from flash
static uint32_t scene_iv, scene_n;

static void read_os_scene(const uint8_t *sector) {
    const os_stored_scene_t *sc = (const os_stored_scene_t *)(sector + 512);
    if (sc->magic != SC26_SCENES_MAGIC || sc->version != 1 ||
        sc->crc != crc32((const uint8_t *)&sc->r, sizeof sc->r)) return;
    uint32_t len = sc->r.blob_len;
    if (len < 4 || 512 + sizeof *sc + len + 4 > 4096) return;
    const uint8_t *blob = sector + 512 + sizeof *sc;
    uint32_t bc;
    memcpy(&bc, blob + len, 4);
    if (bc != crc32(blob, len)) return;
    uint32_t iv = blob[0] | blob[1] << 8, n = blob[2] | blob[3] << 8;
    if (iv < 10 || n < 2 || 4 + n * WS2812_COUNT * 3 > len) return;
    scene_iv = iv; scene_n = n; scene_loop = blob + 4;
}

static void read_os_settings(void) {
    const os_stored_t *st = (const os_stored_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + SC26_SETTINGS_PHYS);
    if (st->magic == 0x53433236u && st->version == 1 && st->crc == crc32((const uint8_t *)&st->s, sizeof st->s)) {
        os_brightness = (st->s.brightness >= 10 && st->s.brightness <= 100) ? st->s.brightness : 100;
        os_led_mode = st->s.led_mode;
        os_ring_cw = st->s.ring_cw;
        read_os_scene((const uint8_t *)st);   // the LED scene loop, if any
    }
}


// ------------------------------------------------------------------ flash slot / address translation
static uint32_t slot_base;

uint32_t sc26_flash_slot_base(void) { return slot_base; }

bool sc26_flash_offset_ok(uint32_t logical_offs) {
    return logical_offs >= SC26_WHD_OFFSET && logical_offs < SC26_DOOM_SIZE;
}

void sc26_early_init(void) {
    nvic_hw->icer[0] = 0xFFFFFFFFu; nvic_hw->icer[1] = 0xFFFFFFFFu;
    nvic_hw->icpr[0] = 0xFFFFFFFFu; nvic_hw->icpr[1] = 0xFFFFFFFFu;
    // SC26 OS starts us with rom_chain_image(): the boot ROM maps our slot to 0x10000000 through
    // QMI ATRANS0. Make sure the whole 6.5 MB slot is visible (the WAD continues past 4 MB),
    // whatever the boot ROM did with the second window.
    uint32_t a0 = qmi_hw->atrans[0];
    uint32_t base0 = (a0 & QMI_ATRANS0_BASE_BITS) >> QMI_ATRANS0_BASE_LSB;     // 4 KB units
    slot_base = base0 << 12;
    uint32_t want0 = (base0 << QMI_ATRANS0_BASE_LSB) | (0x400u << QMI_ATRANS0_SIZE_LSB);
    uint32_t want1 = ((base0 + 0x400u) << QMI_ATRANS1_BASE_LSB) |
                     (((SC26_DOOM_SIZE - 0x400000u) >> 12) << QMI_ATRANS1_SIZE_LSB);
    if (qmi_hw->atrans[0] != want0) qmi_hw->atrans[0] = want0;
    if (qmi_hw->atrans[1] != want1) {
        qmi_hw->atrans[1] = want1;
        xip_cache_invalidate_all();     // drop anything cached under the old 4..8 MB mapping
    }
    read_os_settings();
    watchdog_hw->scratch[1] = watchdog_hw->scratch[2] = watchdog_hw->scratch[3] = 0;
    sc26_stage(0, 1);
    sc26_stage(1, 0);
    watchdog_enable(15000, true);   // a hang reboots into SC26 OS, which logs where it got stuck
}

// ------------------------------------------------------------------ panel
static PIO lcd_pio = pio1;
static uint lcd_sm;
int sc26_lcd_dma = -1;

static inline void __not_in_flash_func(cs_low)(void)  { gpio_put(PIN_LCD_CS, 0); }
static inline void __not_in_flash_func(cs_high)(void) { gpio_put(PIN_LCD_CS, 1); }
static inline void __not_in_flash_func(put_raw)(uint8_t b) { pio_sm_put_blocking(lcd_pio, lcd_sm, (uint32_t)b << 24); }

static void __not_in_flash_func(put_single)(uint8_t b) {
    for (int i = 7; i > 0; i -= 2)
        put_raw((uint8_t)((((b >> i) & 1) << 4) | ((b >> (i - 1)) & 1)));
}

static void __not_in_flash_func(wait_idle)(void) {
    uint32_t stall = 1u << (PIO_FDEBUG_TXSTALL_LSB + lcd_sm);
    while (!pio_sm_is_tx_fifo_empty(lcd_pio, lcd_sm)) tight_loop_contents();
    lcd_pio->fdebug = stall;
    while (!(lcd_pio->fdebug & stall)) tight_loop_contents();
}

static void __not_in_flash_func(send_cmd)(uint8_t cmd, const uint8_t *data, int len) {
    cs_low();
    put_single(0x02); put_single(0x00); put_single(cmd); put_single(0x00);
    for (int i = 0; i < len; i++) put_single(data[i]);
    wait_idle();
    cs_high();
}

void __not_in_flash_func(sc26_lcd_begin_pixels)(int x, int y, int w, int h) {
    int x1 = x + w - 1, y1 = y + h - 1;
    uint8_t ca[4] = { (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t ra[4] = { (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    send_cmd(0x2A, ca, 4);
    send_cmd(0x2B, ra, 4);
    cs_low();
    put_single(0x32); put_single(0x00); put_single(0x2C); put_single(0x00);
}

void __not_in_flash_func(sc26_lcd_start_dma)(const void *src, uint32_t bytes) {
    dma_channel_set_read_addr(sc26_lcd_dma, src, false);
    dma_channel_set_trans_count(sc26_lcd_dma, bytes, true);
}

void __not_in_flash_func(sc26_lcd_recover)(void) {
    hw_clear_bits(&dma_hw->ch[sc26_lcd_dma].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_hw->abort = 1u << sc26_lcd_dma;
    while (dma_hw->abort & (1u << sc26_lcd_dma)) tight_loop_contents();
    hw_set_bits(&dma_hw->ch[sc26_lcd_dma].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_hw->ints0 = 1u << sc26_lcd_dma;
    pio_sm_clear_fifos(lcd_pio, lcd_sm);
    cs_high();
}

void __not_in_flash_func(sc26_lcd_end_pixels)(void) {
    wait_idle();
    cs_high();
}

void sc26_backlight(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    pwm_set_gpio_level(PIN_LCD_BL, (uint16_t)(pct * 10));
}

static void fill_screen(uint16_t c) {
    sc26_lcd_begin_pixels(0, 0, LCD_WIDTH, LCD_HEIGHT);
    for (int n = LCD_WIDTH * LCD_HEIGHT; n > 0; n--) { put_raw(c >> 8); put_raw(c & 0xFF); }
    sc26_lcd_end_pixels();
}

void sc26_lcd_init(void) {
    uint32_t sys = clock_get_hz(clk_sys);
    gpio_set_function(PIN_LCD_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_LCD_BL);
    pwm_config pc = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&pc, (sys + 5000000u) / 10000000u);   // ~10 MHz / 1000 = 10 kHz
    pwm_config_set_wrap(&pc, 999);
    pwm_init(slice, &pc, true);
    sc26_backlight(0);

    gpio_init(PIN_LCD_CS);  gpio_set_dir(PIN_LCD_CS, GPIO_OUT);  gpio_put(PIN_LCD_CS, 1);
    gpio_init(PIN_LCD_RST); gpio_set_dir(PIN_LCD_RST, GPIO_OUT); gpio_put(PIN_LCD_RST, 1);
    gpio_init(PIN_LCD_TE);  gpio_set_dir(PIN_LCD_TE, GPIO_IN);   gpio_pull_up(PIN_LCD_TE);

    uint offset = pio_add_program(lcd_pio, &qspi_lcd_program);
    lcd_sm = pio_claim_unused_sm(lcd_pio, true);
    // SCK = sys / (2 * div); integer divider (no jitter), SCK <= 75 MHz like the original firmware
    uint div = (sys + 149999999u) / 150000000u;
    if (div < 1) div = 1;
    // same setup as qspi_lcd_program_init(), but with an integer divider: this build has no
    // float library (pico_float "none"), and the float helpers would panic
    {
        pio_sm_config c = qspi_lcd_program_get_default_config(offset);
        sm_config_set_out_pins(&c, PIN_LCD_D0, 4);
        sm_config_set_sideset_pins(&c, PIN_LCD_SCK);
        sm_config_set_out_shift(&c, false, true, 8);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        sm_config_set_clkdiv_int_frac8(&c, div, 0);
        for (uint i = 0; i < 4; i++) pio_gpio_init(lcd_pio, PIN_LCD_D0 + i);
        pio_gpio_init(lcd_pio, PIN_LCD_SCK);
        pio_sm_set_pins_with_mask(lcd_pio, lcd_sm, 0, (0xFu << PIN_LCD_D0) | (1u << PIN_LCD_SCK));
        pio_sm_set_consecutive_pindirs(lcd_pio, lcd_sm, PIN_LCD_D0, 4, true);
        pio_sm_set_consecutive_pindirs(lcd_pio, lcd_sm, PIN_LCD_SCK, 1, true);
        pio_sm_init(lcd_pio, lcd_sm, offset, &c);
        pio_sm_set_enabled(lcd_pio, lcd_sm, true);
    }

    dma_channel_claim(11);       // used by the status-bar XIP streaming in i_video.c
    sc26_lcd_dma = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(sc26_lcd_dma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(lcd_pio, lcd_sm, true));
    dma_channel_configure(sc26_lcd_dma, &dc, &lcd_pio->txf[lcd_sm], NULL, 0, false);

    gpio_put(PIN_LCD_RST, 1); sleep_ms(10);
    gpio_put(PIN_LCD_RST, 0); sleep_ms(10);
    gpio_put(PIN_LCD_RST, 1); sleep_ms(120);
    for (size_t i = 0; i < ST77916_INIT_SEQ_LEN; i++) {
        const st77916_init_cmd_t *c = &st77916_init_seq[i];
        send_cmd(c->cmd, c->data, c->len);
        if (c->delay_ms) sleep_ms(c->delay_ms);
    }
    fill_screen(0x0000);
    sc26_backlight(os_brightness);
}

// ------------------------------------------------------------------ LEDs
static PIO led_pio = pio0;
static uint led_sm;
static int16_t led_cur[WS2812_COUNT][3], led_tgt[WS2812_COUNT][3];

static void leds_init(void) {
    uint offset = pio_add_program(led_pio, &ws2812_program);
    led_sm = pio_claim_unused_sm(led_pio, true);
    {   // ws2812_program_init() with an integer 8.8 divider (no float library in this build)
        pio_gpio_init(led_pio, PIN_WS2812);
        pio_sm_set_consecutive_pindirs(led_pio, led_sm, PIN_WS2812, 1, true);
        pio_sm_config c = ws2812_program_get_default_config(offset);
        sm_config_set_sideset_pins(&c, PIN_WS2812);
        sm_config_set_out_shift(&c, false, true, 24);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        uint32_t bit_hz = 800000u * (ws2812_T1 + ws2812_T2 + ws2812_T3);
        uint32_t div256 = (uint32_t)(((uint64_t)clock_get_hz(clk_sys) * 256u + bit_hz / 2) / bit_hz);
        sm_config_set_clkdiv_int_frac8(&c, div256 >> 8, (uint8_t)(div256 & 0xFF));
        pio_sm_init(led_pio, led_sm, offset, &c);
        pio_sm_set_enabled(led_pio, led_sm, true);
    }
    gpio_init(PIN_STATUS_LED); gpio_set_dir(PIN_STATUS_LED, GPIO_OUT); gpio_put(PIN_STATUS_LED, 0);
}

static void leds_push(void) {
    for (int i = 0; i < WS2812_COUNT; i++) {
        uint32_t r = (uint32_t)led_cur[i][0], g = (uint32_t)led_cur[i][1], b = (uint32_t)led_cur[i][2];
        pio_sm_put_blocking(led_pio, led_sm, ((g << 16) | (r << 8) | b) << 8);
    }
}

// position k = 0..8 clockwise from the top of the screen -> physical LED index
static int ring_index(int k) { return os_ring_cw ? (7 + k) % WS2812_COUNT : (7 - k + WS2812_COUNT) % WS2812_COUNT; }

void sc26_set_led_target(int k, uint8_t r, uint8_t g, uint8_t b) {
    int i = ring_index(k);
    led_tgt[i][0] = r; led_tgt[i][1] = g; led_tgt[i][2] = b;
}

// The ring follows the LED mode chosen in SC26 OS: 0 original sweep, 1 ambient (follows the
// screen), 2 off, 3 scene (the recorded loop; ambient if there is none). Called once per frame
// by the video code.
void sc26_leds_update(void) {
    if (os_led_mode == 2) return;                    // LEDs off in SC26 OS -> off here too
    uint32_t ms = time_us_32() / 1000;
    if (os_led_mode == 3 && scene_loop) {            // the scene recorded by SC26 OS
        uint32_t pos = ms % (scene_iv * scene_n);
        uint32_t i = pos / scene_iv, f = (pos % scene_iv) * 256 / scene_iv;   // blend 0..255
        const uint8_t *a = scene_loop + i * (WS2812_COUNT * 3);
        const uint8_t *b = scene_loop + ((i + 1) % scene_n) * (WS2812_COUNT * 3);
        for (int k = 0; k < WS2812_COUNT * 3; k++)
            led_cur[k / 3][k % 3] = (int16_t)((a[k] * (256 - f) + b[k] * f) >> 8);
        leds_push();
        return;
    }
    if (os_led_mode == 0) {                          // the stock badge's purple sweep, 170 ms steps
        static const uint16_t masks[5] = { 0x080, 0x140, 0x021, 0x012, 0x00C };
        uint16_t m = masks[(ms / 170) % 5];
        for (int i = 0; i < WS2812_COUNT; i++) {
            int16_t v = (m >> i) & 1 ? 16 : 0;
            led_cur[i][0] = v; led_cur[i][1] = 0; led_cur[i][2] = v;
        }
        leds_push();
        return;
    }
    for (int i = 0; i < WS2812_COUNT; i++)
        for (int c = 0; c < 3; c++) {
            led_cur[i][c] += (int16_t)((led_tgt[i][c] - led_cur[i][c]) * 3 / 8);
            if (led_cur[i][c] < 0) led_cur[i][c] = 0;
            if (led_cur[i][c] > 255) led_cur[i][c] = 255;
        }
    leds_push();
}

// ------------------------------------------------------------------ diagnostics
static void diag_set_magic(uint32_t magic) {
    uint32_t v = watchdog_hw->scratch[0];
    watchdog_hw->scratch[0] = (magic << 24) | (v & 0x00FFFF00u) | (get_core_num() & 0xFFu);
}

void sc26_stage(int core, int stage) {
    uint32_t v = watchdog_hw->scratch[0];
    if ((v >> 24) != SC26_DIAG_RUN) v = SC26_DIAG_RUN << 24;
    if (core) v = (v & ~0x00FF0000u) | ((uint32_t)(stage & 0xFF) << 16);
    else      v = (v & ~0x0000FF00u) | ((uint32_t)(stage & 0xFF) << 8);
    watchdog_hw->scratch[0] = v;
    watchdog_update();
}

static void __attribute__((noreturn)) reboot_to_os(void) {
    watchdog_reboot(0, 0, 10);     // normal boot = SC26 OS in the first flash slot
    while (true) tight_loop_contents();
}

// ------------------------------------------------------------------ exit / crash -> back to SC26 OS
void sc26_exit_to_os(void) {
    diag_set_magic(SC26_DIAG_QUIT);
    sc26_backlight(0);
    memset(led_cur, 0, sizeof led_cur);
    leds_push();
    reboot_to_os();
}

void __attribute__((noinline)) sc26_error(const char *fmt, ...) {
    watchdog_hw->scratch[1] = (uint32_t)fmt;
    watchdog_hw->scratch[2] = (uint32_t)__builtin_return_address(0);
    diag_set_magic(SC26_DIAG_ERROR);
    sc26_backlight(0);
    reboot_to_os();
}

// SDK panic() (wrapped with --wrap=panic)
void __attribute__((noinline, noreturn)) __wrap_panic(const char *fmt, ...) {
    // record the first two code return addresses on the stack: the 2nd is usually whoever
    // called hard_assertion_failure() / panic()
    uint32_t *sp, a[2] = { 0, 0 };
    __asm volatile("mov %0, sp" : "=r"(sp));
    for (int i = 0, n = 0; i < 64 && n < 2; i++) {
        uint32_t v = sp[i];
        if ((v & 1u) && v >= 0x10000000u && v < 0x10080000u) a[n++] = v;
    }
    watchdog_hw->scratch[1] = (uint32_t)fmt;
    watchdog_hw->scratch[2] = a[0] ? a[0] : (uint32_t)__builtin_return_address(0);
    watchdog_hw->scratch[3] = a[1];
    diag_set_magic(SC26_DIAG_PANIC);
    reboot_to_os();
}

void __attribute__((noreturn)) _exit(int status) {
    watchdog_hw->scratch[1] = 0xE0000000u | (uint32_t)(status & 0xFF);
    watchdog_hw->scratch[2] = (uint32_t)__builtin_return_address(0);
    diag_set_magic(SC26_DIAG_ERROR);
    reboot_to_os();
}

void __attribute__((used)) sc26_fault_c(uint32_t *frame) {
    watchdog_hw->scratch[1] = frame[6];                         // stacked PC
    watchdog_hw->scratch[2] = frame[5];                         // stacked LR
    watchdog_hw->scratch[3] = *(volatile uint32_t *)0xE000ED28u; // CFSR
    diag_set_magic(SC26_DIAG_FAULT);
    reboot_to_os();
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b sc26_fault_c  \n");
}

// ------------------------------------------------------------------ buttons
enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_CENTER, B_B, B_A, B_N };
static const uint8_t btn_pin[B_N] = { PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_LEFT, PIN_BTN_RIGHT,
                                      PIN_BTN_CENTER, PIN_BTN_B, PIN_BTN_A };
static int held_key[B_N];
static uint32_t btn_prev;
static uint64_t center_down_at;
static bool center_combo;
bool sc26_always_run = true;

#define NPULSE 4
static struct { int key; uint64_t up_at; } pulses[NPULSE];

extern boolean menuactive;
extern int messageToPrint;
extern boolean messageNeedsInput;

static void post(evtype_t t, int key) {
    event_t e;
    memset(&e, 0, sizeof e);
    e.type = t;
    e.data1 = key;
    if (t == ev_keydown && key > 0 && key < 128) e.data2 = e.data3 = key;
    D_PostEvent(&e);
}

static void pulse(int key) {        // key down now, up ~3 tics later (so G_BuildTiccmd sees it)
    for (int i = 0; i < NPULSE; i++) if (!pulses[i].key) {
        post(ev_keydown, key);
        pulses[i].key = key;
        pulses[i].up_at = time_us_64() + 100000;
        return;
    }
}

static int next_weapon_key(void) {
    static const weapontype_t order[] = { wp_fist, wp_chainsaw, wp_pistol, wp_shotgun, wp_supershotgun,
                                          wp_chaingun, wp_missile, wp_plasma, wp_bfg };
    static const char keys[] = { '1', '1', '2', '3', '3', '4', '5', '6', '7' };
    const int n = sizeof order / sizeof order[0];
    player_t *p = &players[consoleplayer];
    weapontype_t cur = p->pendingweapon != wp_nochange ? p->pendingweapon : p->readyweapon;
    int idx = 0;
    for (int i = 0; i < n; i++) if (order[i] == cur) idx = i;
    for (int k = 1; k < n; k++) {
        weapontype_t w = order[(idx + k) % n];
        if (w == wp_supershotgun && gamemode != commercial) continue;
        if (!p->weaponowned[w]) continue;
        ammotype_t a = weaponinfo[w].ammo;
        if (a != am_noammo && p->ammo[a] <= 0) continue;
        return keys[(idx + k) % n];
    }
    return 0;
}

void sc26_input_init(void) {
    for (int i = 0; i < B_N; i++) { gpio_init(btn_pin[i]); gpio_set_dir(btn_pin[i], GPIO_IN); gpio_pull_up(btn_pin[i]); }
    leds_init();
    memset(led_cur, 0, sizeof led_cur);
    leds_push();
}

static uint32_t read_buttons(void) {
    uint32_t in = gpio_get_all(), b = 0;
    for (int i = 0; i < B_N; i++) if (!(in & (1u << btn_pin[i]))) b |= 1u << i;
    return b;
}

// What a button press means right now. Returns a key to hold while the button is down,
// 0 when the press was handled here (pulsed keys / toggles).
static int map_press(int b, bool center_held) {
    const bool in_menu = menuactive || messageToPrint;
    const bool prompt = messageToPrint && messageNeedsInput;
    if (center_held && !in_menu) {
        switch (b) {
            case B_LEFT:  return ',';                    // strafe left
            case B_RIGHT: return '.';                    // strafe right
            case B_UP:    { int k = next_weapon_key(); if (k) pulse(k); return 0; }
            case B_DOWN:  pulse(KEY_TAB); return 0;      // automap
            case B_B:     sc26_always_run = !sc26_always_run; return 0;
            default: break;
        }
    }
    switch (b) {
        case B_UP:    return KEY_UPARROW;
        case B_DOWN:  return KEY_DOWNARROW;
        case B_LEFT:  return KEY_LEFTARROW;
        case B_RIGHT: return KEY_RIGHTARROW;
        case B_A:     return in_menu ? (prompt ? 'y' : KEY_ENTER) : KEY_RCTRL;      // fire
        case B_B:     return in_menu ? (prompt ? 'n' : KEY_BACKSPACE) : ' ';         // use / open
        default:      return 0;
    }
}

// ------------------------------------------------------------------ core 1 NMI (freeze reports)
static volatile bool c1_nmi_done;
static volatile uint32_t c1_nmi_pc, c1_nmi_lr, c1_nmi_ipsr, c1_nmi_primask, c1_nvic[4];
extern volatile uint32_t sc26_c1_last_alarm_us;
extern uint32_t *sc26_c1_stack_lo, *sc26_c1_stack_hi;

void __attribute__((used)) sc26_nmi_c(uint32_t *frame) {
    uint32_t pm;
    __asm volatile("mrs %0, primask" : "=r"(pm));
    c1_nmi_pc = frame[6];
    c1_nmi_lr = frame[5];
    c1_nmi_ipsr = frame[7] & 0x1FFu;        // exception core 1 was in (0 = normal code)
    c1_nmi_primask = pm;
    c1_nvic[0] = nvic_hw->iser[0]; c1_nvic[1] = nvic_hw->iser[1];     // core 1's own NVIC
    c1_nvic[2] = nvic_hw->ispr[0]; c1_nvic[3] = nvic_hw->ispr[1];
    c1_nmi_done = true;
    while (true) __wfe();                   // stay put; core 0 reboots the chip
}

void __attribute__((naked)) isr_nmi(void) {
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b sc26_nmi_c    \n");
}

// ------------------------------------------------------------------ core 0 hang probe
// A 2 Hz timer interrupt on core 0. If the game loop hasn't polled the buttons for 4 s, record
// where core 0 is stuck (and what core 1 / the panel pump were doing) and reboot to SC26 OS.
extern volatile uint32_t sc26_frames, sc26_recoveries, sc26_core1_pc;
uint32_t sc26_pump_state(void);
void sc26_pump_regs(uint32_t *out);
static volatile uint32_t heartbeat;
static uint probe_alarm;
static uint32_t probe_next, probe_last_hb, probe_stuck;

void __attribute__((used)) sc26_probe_c(uint32_t *frame) {
    timer_hw->intr = 1u << probe_alarm;
    probe_next += 500000;
    if ((int32_t)(probe_next - timer_hw->timerawl) < 1000) probe_next = timer_hw->timerawl + 500000;
    timer_hw->alarm[probe_alarm] = probe_next;
    if (heartbeat != probe_last_hb) { probe_last_hb = heartbeat; probe_stuck = 0; return; }
    if (++probe_stuck < 8) return;
    bool halted = (syscfg_hw->proc_config & SYSCFG_PROC_CONFIG_PROC1_HALTED_BITS) != 0;
    sio_hw->doorbell_out_set = 1;                     // -> NMI on core 1 (sc26_nmi_c)
    for (int i = 0; i < 3000 && !c1_nmi_done; i++) busy_wait_us(1);
    uint32_t free4 = 0;                               // core 1 stack never touched, in words
    for (uint32_t *s = sc26_c1_stack_lo; s < sc26_c1_stack_hi && *s == 0x5C26C0DEu; s++) free4++;
    if (free4 > 0xFFF) free4 = 0xFFF;
    uint32_t ms = (timer_hw->timerawl - sc26_c1_last_alarm_us) / 1000;
    if (ms > 0xFFF) ms = 0xFFF;
    watchdog_hw->scratch[1] = frame[6];
    watchdog_hw->scratch[2] = frame[5];
    watchdog_hw->scratch[3] = c1_nmi_done ? c1_nmi_pc : sc26_core1_pc;
    watchdog_hw->scratch[5] = c1_nmi_done ? c1_nmi_lr : 0;
    watchdog_hw->scratch[6] = ((uint32_t)c1_nmi_done << 31) | ((uint32_t)halted << 30) | ((c1_nmi_primask & 1u) << 29) |
                              ((sc26_pump_state() >> 31) << 28) | (free4 << 16) | (c1_nmi_ipsr & 0x1FFu);
    watchdog_hw->scratch[7] = (ms << 20) | (sc26_frames & 0xFFFFFu);
    // extra register dump in a RAM mailbox SC26 OS doesn't use (it survives the watchdog reboot)
    {
        volatile uint32_t *mb = (volatile uint32_t *)SC26_DIAG_MAILBOX;
        uint32_t w[24], n = 0;
        for (int i = 0; i < 4; i++) w[n++] = c1_nvic[i];
        w[n++] = timer_hw->inte; w[n++] = timer_hw->intr; w[n++] = timer_hw->armed;
        uint32_t pr[9];
        sc26_pump_regs(pr);
        for (int i = 0; i < 9; i++) w[n++] = pr[i];
        w[n++] = pio1->fdebug; w[n++] = pio1->fstat;
        w[n++] = timer_hw->timerawl; w[n++] = sc26_c1_last_alarm_us;
        w[n++] = nvic_hw->iser[0]; w[n++] = nvic_hw->ispr[0];      // core 0's own NVIC
        mb[1] = n;
        for (uint32_t i = 0; i < n; i++) mb[2 + i] = w[i];
        mb[2 + n] = crc32((const uint8_t *)w, n * 4);
        mb[0] = 0x53433244u;                                         // "SC2D"
    }
    diag_set_magic(SC26_DIAG_HANG);
    reboot_to_os();
}

static void __attribute__((naked)) probe_isr(void) {
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b sc26_probe_c  \n");
}

static void probe_start(void) {
    extern uint32_t __vectors[];
    probe_alarm = (uint)hardware_alarm_claim_unused(true);
    uint irq = TIMER_ALARM_IRQ_NUM(timer_hw, probe_alarm);
    ((volatile uint32_t *)scb_hw->vtor)[16 + irq] = (uint32_t)probe_isr;
    __dsb(); __isb();
    irq_set_priority(irq, 0x00);           // above everything else on core 0
    hw_set_bits(&timer_hw->inte, 1u << probe_alarm);
    irq_set_enabled(irq, true);
    probe_next = timer_hw->timerawl + 500000;
    timer_hw->alarm[probe_alarm] = probe_next;
}

void sc26_input_poll(void) {
    static bool first = true;
    if (first) { first = false; sc26_stage(0, 7); probe_start(); }
    heartbeat++;
    watchdog_update();
    uint64_t now = time_us_64();
    for (int i = 0; i < NPULSE; i++)
        if (pulses[i].key && now >= pulses[i].up_at) { post(ev_keyup, pulses[i].key); pulses[i].key = 0; }

    uint32_t b = read_buttons();
    uint32_t pressed = b & ~btn_prev, released = btn_prev & ~b;
    const uint32_t C = 1u << B_CENTER;

    // CENTER: tap = menu (Esc); modifier for strafe / weapon / map / run; hold alone 2 s = quit to SC26 OS
    if (pressed & C) { center_down_at = now; center_combo = false; }
    if ((b & C) && (b & ~C)) center_combo = true;
    if ((b & C) && !center_combo && now - center_down_at > 2000000) sc26_exit_to_os();
    if ((released & C) && !center_combo && now - center_down_at < 600000) pulse(KEY_ESCAPE);

    for (int i = 0; i < B_N; i++) {
        if (i == B_CENTER) continue;
        uint32_t m = 1u << i;
        if (pressed & m) {
            int k = map_press(i, (b & C) != 0);
            if (k) { post(ev_keydown, k); held_key[i] = k; }
        }
        if ((released & m) && held_key[i]) { post(ev_keyup, held_key[i]); held_key[i] = 0; }
    }
    btn_prev = b;
}
#endif
