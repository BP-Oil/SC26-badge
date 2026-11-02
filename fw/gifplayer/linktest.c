// SC26 OS "LINK TEST": find the docking-port pins and test a badge-to-badge serial link.
//
// The original badge firmware talks to conference stations over UART0 (GP0 = TX, GP1 = RX,
// 9600 8N1). Three pages, LEFT/RIGHT to switch, B or hold CENTER to leave:
//   PIN FINDER  every spare GPIO is an input with a pull-up; touch a wire from GND to a pad
//               (through a ~1 kOhm resistor, in case the pad is a power pin) and the screen
//               names the GPIO that went low. The badge only reads these pins on this page.
//   TX BLINK    GP0 (TX) switches 0 V <-> 3.3 V once a second, for finding it with a meter.
//   LINK TEST   UART0 on GP0/GP1 sends "$SC26,<id>,<n>*<xor>" 4x a second and shows what comes
//               back: your own id = loopback (TX jumpered to RX), another id = linked badge.
//               UP/DOWN change the baud rate (both badges must match).
//   USB LINK    two badges joined by a USB-C cable. Both run on battery; one has to act as
//               the USB host ("PLAYER 1"), so pressing A here restarts this badge in host mode
//               (linktest_usb_host_run). The other badge stays on this page as PLAYER 2: it is
//               a normal USB device and answers over its USB serial port.
// Link messages (UART and USB): "$SC26,<id>,<seq>,<ack>*<xor>" - ack is the last seq heard
// from the other side, so the sender can time the round trip.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "lcd.h"
#include "buttons.h"
#include "leds.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include "flash_disk.h"
#include "settings.h"
#include "usb_glue.h"
#include "linktest.h"

#define C_BG     RGB565(0, 0, 0)
#define C_TITLE  RGB565(60, 230, 120)
#define C_TEXT   RGB565(235, 235, 235)
#define C_DIM    RGB565(140, 150, 170)
#define C_CHIP   RGB565(36, 46, 64)
#define C_HIT    RGB565(220, 50, 50)
#define C_FOUND  RGB565(60, 230, 120)
#define C_AMBER  RGB565(255, 190, 80)

typedef enum { PG_FINDER, PG_BLINK, PG_LINK, PG_USB, PG_COUNT } page_t;

static const struct { uint8_t gpio; const char *what; } pins[] = {
    { 0, "TX" }, { 1, "RX" }, { 2, "" }, { 3, "" }, { 6, "" },
    { 25, "" }, { 26, "" }, { 27, "" }, { 28, "" }, { 29, "STRP" },
};
#define NPINS ((int)(sizeof pins / sizeof pins[0]))

static const uint32_t bauds[] = { 9600, 115200, 1000000 };
static int baud_i;
static page_t page;
static bool base_low[NPINS], was_low[NPINS], found[NPINS];
static char my_id[5];

static void text_c(int y, const char *s, uint16_t fg, int scale) {
    lcd_draw_text((LCD_WIDTH - lcd_text_width(s, scale)) / 2, y, s, fg, C_BG, scale);
}

static void header(const char *title, const char *sub) {
    lcd_blit_wait();
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    text_c(34, title, C_TITLE, 2);
    if (sub) text_c(76, sub, C_DIM, 1);
    // page dots + hint
    for (int i = 0; i < PG_COUNT; i++) {
        int x = 180 - 7 * PG_COUNT + 2 + i * 14;
        if (i == (int)page) lcd_fill_rect(x - 1, 307, 10, 10, C_TITLE);
        else                lcd_fill_rect(x + 1, 309, 6, 6, RGB565(70, 80, 100));
    }
    text_c(322, "L/R PAGE  B EXIT", C_DIM, 1);
}

// ------------------------------------------------------------------ pin modes
static void pins_release(void) {
    uart_deinit(uart0);
    for (int i = 0; i < NPINS; i++) { gpio_init(pins[i].gpio); gpio_disable_pulls(pins[i].gpio); }
}

static void pins_inputs(void) {
    pins_release();
    for (int i = 0; i < NPINS; i++) gpio_pull_up(pins[i].gpio);
    sleep_ms(5);
    for (int i = 0; i < NPINS; i++) { base_low[i] = !gpio_get(pins[i].gpio); was_low[i] = base_low[i]; }
}

// ------------------------------------------------------------------ PIN FINDER
static void finder_box(int i, bool low) {
    int col = i % 2, row = i / 2;
    int x = 55 + col * 130, y = 100 + row * 38;
    uint16_t bg = low && !base_low[i] ? C_HIT : C_CHIP;
    uint16_t fg = found[i] ? C_FOUND : base_low[i] ? C_AMBER : C_TEXT;
    if (low && !base_low[i]) fg = C_TEXT;
    lcd_fill_rect(x, y, 120, 30, bg);
    char s[16];
    snprintf(s, sizeof s, "GP%-2d %s", pins[i].gpio, pins[i].what);
    lcd_draw_text(x + 6, y + 6, s, fg, bg, 1);
    const char *st = base_low[i] ? "LO*" : low ? "LOW" : found[i] ? "ok" : "";
    lcd_draw_text(x + 114 - lcd_text_width(st, 1), y + 6, st, fg, bg, 1);
}

static void finder_draw(void) {
    header("PIN FINDER", "GND via 1k to each pad");
    for (int i = 0; i < NPINS; i++) finder_box(i, !gpio_get(pins[i].gpio));
}

static uint64_t banner_until;
static void finder_step(void) {
    for (int i = 0; i < NPINS; i++) {
        bool low = !gpio_get(pins[i].gpio);
        if (low == was_low[i]) continue;
        was_low[i] = low;
        if (low && !base_low[i]) {
            found[i] = true;
            char s[28];
            snprintf(s, sizeof s, "GP%d TOUCHED %s", pins[i].gpio, pins[i].what);
            lcd_fill_rect(40, 288, 280, 19, C_BG);
            text_c(288, s, C_AMBER, 1);
            banner_until = time_us_64() + 3000000;
            leds_fill(0, 60, 0); leds_show();
        } else {
            leds_fill(0, 0, 0); leds_show();
        }
        finder_box(i, low);
    }
    if (banner_until && time_us_64() > banner_until) { lcd_fill_rect(40, 288, 280, 19, C_BG); banner_until = 0; }
}

// ------------------------------------------------------------------ TX BLINK
static bool blink_on;
static uint64_t blink_next;
static void blink_state(void) {
    lcd_fill_rect(130, 196, 100, 100, C_BG);
    uint16_t c = blink_on ? C_FOUND : C_CHIP;
    lcd_fill_rect(150, 200, 60, 60, c);
    text_c(268, blink_on ? "HIGH 3.3V" : " LOW 0V  ", blink_on ? C_FOUND : C_DIM, 1);
    leds_fill(0, 0, 0);
    if (blink_on) leds_set(7, 0, 80, 0);                   // nose LED follows TX
    leds_show();
}

static void blink_draw(void) {
    header("TX BLINK", "GP0 (TX) switches 1x/s");
    text_c(104, "Probe the gold fingers", C_TEXT, 1);
    text_c(126, "with a meter, black lead", C_TEXT, 1);
    text_c(148, "on GND. The pad going", C_TEXT, 1);
    text_c(170, "0 V <-> 3.3 V is TX.", C_TEXT, 1);
    blink_state();
}

static void blink_start(void) {
    pins_inputs();
    gpio_set_dir(0, GPIO_OUT);                               // GP0 is the original's UART TX: an output by design
    blink_on = false; blink_next = 0;
}

static void blink_step(void) {
    if (time_us_64() < blink_next) return;
    blink_next = time_us_64() + 500000;
    blink_on = !blink_on;
    gpio_put(0, blink_on);
    blink_state();
}

// ------------------------------------------------------------------ link protocol (UART + USB)
static struct {
    uint32_t tx_n, rx_ok, rx_bad, rx_loop, peer_seq, rtt_ms;
    uint64_t last_partner, last_loop, tx_next, sent_at[32];
    char from[5], line[48];
    int line_n, last_status;
} L;

static uint8_t xor_sum(const char *s, int n) { uint8_t x = 0; while (n--) x ^= (uint8_t)*s++; return x; }

static void proto_reset(void) { memset(&L, 0, sizeof L); strcpy(L.from, "----"); L.last_status = -1; }

static int proto_make(char *s, size_t cap) {
    L.tx_n++;
    L.sent_at[L.tx_n % 32] = time_us_64();
    int n = snprintf(s, cap, "$SC26,%s,%lu,%lu", my_id, (unsigned long)L.tx_n, (unsigned long)L.peer_seq);
    n += snprintf(s + n, cap - (size_t)n, "*%02X\n", xor_sum(s + 1, n - 1));
    return n;
}

static void proto_line(void) {
    L.line[L.line_n] = 0;
    char *star = strrchr(L.line, '*');
    unsigned sum = 0;
    bool ok = L.line_n > 10 && star && strncmp(L.line, "$SC26,", 6) == 0 && L.line[10] == ','
              && sscanf(star + 1, "%2x", &sum) == 1 && xor_sum(L.line + 1, (int)(star - L.line - 1)) == sum;
    if (!ok) { L.rx_bad++; return; }
    char id[5]; memcpy(id, L.line + 6, 4); id[4] = 0;
    unsigned long seq = 0, ack = 0;
    sscanf(L.line + 11, "%lu,%lu", &seq, &ack);
    uint64_t now = time_us_64();
    if (strcmp(id, my_id) == 0) {                            // our own message came back: loopback
        L.rx_loop++; L.last_loop = now;
        leds_fill(0, 0, 0); leds_set(7, 0, 0, 90);
    } else {
        L.rx_ok++; strcpy(L.from, id); L.last_partner = now; L.peer_seq = (uint32_t)seq;
        if (ack && ack <= L.tx_n && L.tx_n - ack < 32) L.rtt_ms = (uint32_t)((now - L.sent_at[ack % 32]) / 1000);
        leds_fill(0, 25, 0); leds_set(7, 0, 90, 0);
    }
    leds_show();
}

static void proto_rx(char c) {
    if (c == '$') L.line_n = 0;
    if (c == '\n' || c == '\r') { if (L.line_n && L.line[0] == '$') proto_line(); L.line_n = 0; return; }
    if (L.line_n < (int)sizeof L.line - 1) L.line[L.line_n++] = c;
}

static void link_values(const char *extra) {
    char s[40];
    lcd_fill_rect(60, 102, 250, 110, C_BG);
    snprintf(s, sizeof s, "ME    %s", my_id);                         lcd_draw_text(70, 104, s, C_TEXT, C_BG, 1);
    lcd_draw_text(70, 126, extra, C_TEXT, C_BG, 1);
    snprintf(s, sizeof s, "SENT  %lu", (unsigned long)L.tx_n);         lcd_draw_text(70, 148, s, C_TEXT, C_BG, 1);
    snprintf(s, sizeof s, "GOT   %lu ok %lu bad", (unsigned long)(L.rx_ok + L.rx_loop), (unsigned long)L.rx_bad);
    lcd_draw_text(70, 170, s, C_TEXT, C_BG, 1);
    if (L.rx_ok) snprintf(s, sizeof s, "FROM  %s  %lu ms", L.from, (unsigned long)L.rtt_ms);
    else         snprintf(s, sizeof s, "FROM  %s", L.rx_loop ? "myself" : "----");
    lcd_draw_text(70, 192, s, C_TEXT, C_BG, 1);
}

static void link_status(bool force, const char *hint) {
    uint64_t now = time_us_64();
    int st = (L.last_partner && now - L.last_partner < 1500000) ? 2 : (L.last_loop && now - L.last_loop < 1500000) ? 1 : 0;
    if (st == L.last_status && !force) return;
    L.last_status = st;
    static const char *txt[] = { "  WAITING  ", "LOOPBACK OK", "  LINKED!  " };
    static const uint16_t col[] = { C_DIM, C_AMBER, C_FOUND };
    lcd_fill_rect(40, 224, 280, 62, C_BG);
    text_c(226, txt[st], col[st], 2);
    text_c(268, st ? "" : hint, C_DIM, 1);
    if (!st) { leds_fill(0, 0, 0); leds_show(); }
}

// ------------------------------------------------------------------ LINK TEST (UART on the dock pins)
static void link_extra(char *s, size_t n) { snprintf(s, n, "BAUD  %lu", (unsigned long)bauds[baud_i]); }

static void link_draw(void) {
    header("LINK TEST", "UP/DOWN: baud rate");
    char e[24]; link_extra(e, sizeof e);
    link_values(e);
    link_status(true, "TX->RX  RX->TX  GND");
}

static void link_start(void) {
    pins_release();
    uart_init(uart0, bauds[baud_i]);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    gpio_pull_up(1);                                         // idle high when nothing is plugged in
    proto_reset();
}

static void link_step(void) {
    bool changed = false;
    while (uart_is_readable(uart0)) { proto_rx((char)uart_getc(uart0)); changed = true; }
    if (time_us_64() >= L.tx_next) {
        L.tx_next = time_us_64() + 250000;
        char s[48]; int n = proto_make(s, sizeof s);
        uart_write_blocking(uart0, (const uint8_t *)s, (size_t)n);
        changed = true;
    }
    if (changed) { char e[24]; link_extra(e, sizeof e); link_values(e); }
    link_status(false, "TX->RX  RX->TX  GND");
}

// ------------------------------------------------------------------ USB LINK, PLAYER 2 (USB device)
static void usb_draw(void) {
    header("USB LINK", "A: make this PLAYER 1");
    link_values("ROLE  PLAYER 2");
    link_status(true, "USB-C cable to PLAYER 1");
}

static void usb_start(void) {
    pins_release();
    usb_console(false);                                      // keep printf out of the link
    tud_cdc_read_flush();
    proto_reset();
}

static void usb_step(void) {
    bool changed = false;
    char buf[64];
    while (tud_cdc_available()) {
        uint32_t n = tud_cdc_read(buf, sizeof buf);
        for (uint32_t i = 0; i < n; i++) proto_rx(buf[i]);
        changed = true;
    }
    if (time_us_64() >= L.tx_next) {
        L.tx_next = time_us_64() + 100000;
        if (tud_cdc_connected()) {
            char s[48]; int n = proto_make(s, sizeof s);
            if (tud_cdc_write_available() >= (uint32_t)n) { tud_cdc_write(s, (uint32_t)n); tud_cdc_write_flush(); }
            changed = true;
        }
    }
    if (changed) link_values("ROLE  PLAYER 2");
    link_status(false, "USB-C cable to PLAYER 1");
}

void linktest_become_player1(void) {
    header("USB LINK", NULL);
    text_c(150, "RESTARTING AS", C_TEXT, 1);
    text_c(176, "PLAYER 1", C_TITLE, 2);
    settings_flush();
    flashdisk_flush();
    sleep_ms(400);
    watchdog_hw->scratch[0] = LINKTEST_HOST_MAGIC;           // main() sees this and starts in host mode
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
}

// ------------------------------------------------------------------ main loop
static void page_enter(void) {
    leds_fill(0, 0, 0); leds_show();
    switch (page) {
    case PG_FINDER: pins_inputs(); finder_draw(); break;
    case PG_BLINK:  blink_start(); blink_draw(); break;
    case PG_LINK:   link_start(); link_draw(); break;
    case PG_USB:    usb_start(); usb_draw(); break;
    default: break;
    }
}

static void make_id(void) {
    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);
    snprintf(my_id, sizeof my_id, "%02X%02X", uid.id[6], uid.id[7]);
}

void linktest_run(void (*svc)(void)) {
    make_id();
    memset(found, 0, sizeof found);
    page = PG_FINDER;
    page_enter();
    uint32_t prev = buttons_read();
    uint64_t c_down = 0;
    while (true) {
        if (svc) svc();
        uint32_t b = buttons_read(), down = b & ~prev;
        prev = b;
        if (b & BTN_CENTER) { if (!c_down) c_down = time_us_64(); else if (time_us_64() - c_down > 1000000) break; }
        else c_down = 0;
        if (down & BTN_B) break;
        if (down & (BTN_LEFT | BTN_RIGHT)) {
            if (page == PG_USB) usb_console(true);
            page = (page_t)((page + ((down & BTN_RIGHT) ? 1 : PG_COUNT - 1)) % PG_COUNT);
            page_enter();
            continue;
        }
        if (page == PG_LINK && (down & (BTN_UP | BTN_DOWN))) {
            baud_i = (baud_i + ((down & BTN_UP) ? 1 : 2)) % 3;
            link_start(); link_draw();
        }
        switch (page) {
        case PG_FINDER: finder_step(); break;
        case PG_BLINK:  blink_step(); break;
        case PG_LINK:   link_step(); break;
        case PG_USB:
            if (down & BTN_A) linktest_become_player1();
            usb_step();
            break;
        default: break;
        }
        sleep_ms(2);
    }
    usb_console(true);
    pins_release();
    leds_fill(0, 0, 0); leds_show();
    while (buttons_read()) { if (svc) svc(); sleep_ms(5); }     // don't hand the exit press to the menu
}

// ------------------------------------------------------------------ USB LINK, PLAYER 1 (USB host)
// Runs instead of the normal OS after become_player1() restarts the badge. The USB port is a
// host here (no USB drive, no computer), so the other badge enumerates as a USB serial device.
static volatile int cdc_idx = -1;
static volatile uint8_t dev_addr;
static uint16_t dev_vid, dev_pid;

void tuh_mount_cb(uint8_t daddr) { dev_addr = daddr; tuh_vid_pid_get(daddr, &dev_vid, &dev_pid); }
void tuh_umount_cb(uint8_t daddr) { if (daddr == dev_addr) dev_addr = 0; }
void tuh_cdc_mount_cb(uint8_t idx) { cdc_idx = idx; }
void tuh_cdc_umount_cb(uint8_t idx) { if (idx == cdc_idx) cdc_idx = -1; }
int linktest_host_cdc(void) { return cdc_idx; }

static void host_extra(char *s, size_t n) {
    if (cdc_idx >= 0)   snprintf(s, n, "BADGE %04X:%04X", dev_vid, dev_pid);
    else if (dev_addr)  snprintf(s, n, "USB   %04X:%04X ?", dev_vid, dev_pid);
    else                snprintf(s, n, "USB   no device");
}

void linktest_usb_host_run(void) {
    make_id();
    proto_reset();
    page = PG_USB;
    tusb_rhport_init_t host = { .role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_FULL };
    tusb_rhport_init(0, &host);
    header("USB LINK", "PLAYER 1  (host)");
    lcd_fill_rect(40, 300, 280, 40, C_BG);                   // no pages here: just the way out
    text_c(316, "B: EXIT", C_DIM, 1);
    char e[28]; host_extra(e, sizeof e);
    link_values(e);
    link_status(true, "USB-C cable to PLAYER 2");
    uint32_t prev = buttons_read();
    uint64_t c_down = 0, redraw = 0;
    int shown_idx = -2; uint8_t shown_addr = 0xFF;
    while (true) {
        tuh_task();
        uint32_t b = buttons_read(), down = b & ~prev;
        prev = b;
        if (b & BTN_CENTER) { if (!c_down) c_down = time_us_64(); else if (time_us_64() - c_down > 1000000) break; }
        else c_down = 0;
        if (down & BTN_B) break;
        bool changed = false;
        int idx = cdc_idx;
        if (idx >= 0 && tuh_cdc_mounted((uint8_t)idx)) {
            char buf[64];
            uint32_t n;
            while ((n = tuh_cdc_read((uint8_t)idx, buf, sizeof buf)) > 0) {
                for (uint32_t i = 0; i < n; i++) proto_rx(buf[i]);
                changed = true;
            }
            if (time_us_64() >= L.tx_next) {
                L.tx_next = time_us_64() + 100000;
                char s[48]; int m = proto_make(s, sizeof s);
                tuh_cdc_write((uint8_t)idx, s, (uint32_t)m);
                tuh_cdc_write_flush((uint8_t)idx);
                changed = true;
            }
        }
        if (idx != shown_idx || dev_addr != shown_addr) { shown_idx = idx; shown_addr = dev_addr; changed = true; }
        if (changed && time_us_64() >= redraw) {
            redraw = time_us_64() + 50000;
            host_extra(e, sizeof e);
            link_values(e);
        }
        link_status(false, "USB-C cable to PLAYER 2");
    }
    leds_fill(0, 0, 0); leds_show();
    while (buttons_read()) { tuh_task(); sleep_ms(5); }        // don't hand the exit press on
}
