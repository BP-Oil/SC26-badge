// SC26 hardware probe: confirms the reverse-engineered pin map on real hardware.
//
//  * Display  : init ST77916, draw title, R/G/B/W swatches, gradient (DMA path), live button map
//  * Buttons  : 5-way joystick + A/B light up on screen and print on USB serial
//  * LEDs     : 9 WS2812s chase R->G->B; while a button is held they all show that button's colour
//  * GP18     : toggled at 2 Hz (original blinked it at 1 Hz)
//  * GP8 (TE) : rising edges counted per second -> should read ~50-70 Hz if it is the panel's TE line
//  * GP29     : level shown live
//  * CENTER   : toggles backlight 100 % <-> 20 % (confirms GP7 is the backlight PWM)
//  Nothing here touches the EEPROM or the docking UART.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "lcd.h"
#include "leds.h"
#include "buttons.h"

#define BG      RGB565(8, 12, 24)
#define FG      RGB565(230, 230, 230)
#define DIM     RGB565(60, 70, 90)
#define ON      RGB565(40, 220, 90)

typedef struct { int x, y, w, h; const char *label; } box_t;
// D-pad around (130,205), A/B on the right; all inside a 360 px circle.
static const box_t boxes[BTN_COUNT] = {
    {110, 150, 40, 40, "U"},   // UP
    {110, 240, 40, 40, "D"},   // DOWN
    { 65, 195, 40, 40, "L"},   // LEFT
    {155, 195, 40, 40, "R"},   // RIGHT
    {110, 195, 40, 40, "C"},   // CENTER
    {215, 215, 44, 44, "B"},   // B
    {265, 170, 44, 44, "A"},   // A
};
static const uint8_t btn_rgb[BTN_COUNT][3] = {
    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 160, 0}, {255, 255, 255}, {255, 0, 255}, {0, 255, 255},
};

static volatile uint32_t te_edges;
static void gpio_cb(uint gpio, uint32_t events) { if (gpio == PIN_LCD_TE) te_edges++; }

static void draw_box(int i, bool pressed) {
    const box_t *b = &boxes[i];
    uint16_t fill = pressed ? ON : DIM;
    lcd_fill_rect(b->x, b->y, b->w, b->h, fill);
    lcd_draw_text(b->x + (b->w - 10) / 2, b->y + (b->h - 19) / 2, b->label, pressed ? BG : FG, fill, 1);
}

static void draw_centered(int y, const char *s, uint16_t fg, int scale) {
    lcd_draw_text((LCD_WIDTH - lcd_text_width(s, scale)) / 2, y, s, fg, BG, scale);
}

int main(void) {
    stdio_init_all();
    buttons_init();
    leds_init();
    lcd_init();

    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, BG);
    draw_centered(38, "SC26 PROBE", FG, 2);

    // Colour swatches: should read RED, GREEN, BLUE, WHITE left to right.
    const uint16_t sw[4] = { RGB565(255, 0, 0), RGB565(0, 255, 0), RGB565(0, 0, 255), RGB565(255, 255, 255) };
    for (int i = 0; i < 4; i++) lcd_fill_rect(90 + i * 47, 86, 38, 38, sw[i]);

    // Gradient strip through the DMA path.
    static uint16_t grad[180 * 10];
    for (int y = 0; y < 10; y++)
        for (int x = 0; x < 180; x++) {
            int v = x * 255 / 179;
            grad[y * 180 + x] = RGB565_BE(v, 255 - v, (v * 2) & 255);
        }
    lcd_blit(90, 130, 180, 10, grad);

    for (int i = 0; i < BTN_COUNT; i++) draw_box(i, false);

    gpio_set_irq_enabled_with_callback(PIN_LCD_TE, GPIO_IRQ_EDGE_RISE, true, gpio_cb);

    sleep_ms(1500);   // give USB serial a moment to enumerate
    printf("\nSC26 probe: LCD ST77916 QSPI(SCK=10 D0-3=11-14 CS=15 RST=9 TE=8 BL=7) "
           "WS2812x%d=GP%d STATUS=GP%d\n", WS2812_COUNT, PIN_WS2812, PIN_STATUS_LED);

    uint32_t prev = 0, tick = 0, chase = 0;
    uint8_t bl = 100;
    absolute_time_t next_sec = make_timeout_time_ms(1000), next_chase = make_timeout_time_ms(150);
    absolute_time_t next_blink = make_timeout_time_ms(250);
    bool blink = false;
    int last_strap = -1;
    char line[40];

    while (true) {
        uint32_t now = buttons_read();
        uint32_t changed = now ^ prev;
        for (int i = 0; i < BTN_COUNT; i++) {
            if (!(changed & (1u << i))) continue;
            bool down = now & (1u << i);
            draw_box(i, down);
            printf("BTN %-6s (GP%d) %s\n", button_name(i), button_pin(i), down ? "pressed" : "released");
            if (down && i == 4) { bl = bl == 100 ? 20 : 100; lcd_backlight(bl); printf("backlight %d%%\n", bl); }
        }
        prev = now;

        if (now) {
            int i = __builtin_ctz(now);
            leds_fill(btn_rgb[i][0], btn_rgb[i][1], btn_rgb[i][2]);
            leds_show();
        } else if (time_reached(next_chase)) {
            next_chase = make_timeout_time_ms(150);
            leds_fill(0, 0, 0);
            static const uint8_t c[3][3] = { {255, 0, 0}, {0, 255, 0}, {0, 0, 255} };
            const uint8_t *col = c[(chase / WS2812_COUNT) % 3];
            leds_set(chase % WS2812_COUNT, col[0], col[1], col[2]);
            leds_show();
            chase++;
        }

        if (time_reached(next_blink)) {
            next_blink = make_timeout_time_ms(250);
            blink = !blink;
            status_led(blink);
        }

        int strap = gpio_get(PIN_OFFLINE_STRAP);
        if (strap != last_strap) {
            last_strap = strap;
            snprintf(line, sizeof line, "GP29 %s ", strap ? "HIGH" : "LOW ");
            draw_centered(298, line, FG, 1);
            printf("GP29 = %s\n", strap ? "HIGH" : "LOW");
        }

        if (time_reached(next_sec)) {
            next_sec = make_timeout_time_ms(1000);
            uint32_t te = te_edges; te_edges = 0;
            snprintf(line, sizeof line, "TE %3lu HZ  T %lus", (unsigned long)te, (unsigned long)++tick);
            draw_centered(274, line, te > 20 ? ON : FG, 1);
            printf("TE(GP8) %lu Hz, buttons=0x%02lx\n", (unsigned long)te, (unsigned long)now);
        }
        sleep_ms(5);
    }
}
