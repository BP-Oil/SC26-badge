#include <stdbool.h>
#include "leds.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

static PIO  led_pio = pio0;
static uint led_sm;
static uint8_t rgb[WS2812_COUNT][3];
static uint8_t bright = 64;

void leds_init(void) {
    uint offset = pio_add_program(led_pio, &ws2812_program);
    led_sm = pio_claim_unused_sm(led_pio, true);
    ws2812_program_init(led_pio, led_sm, offset, PIN_WS2812, 800000.f);
    gpio_init(PIN_STATUS_LED);
    gpio_set_dir(PIN_STATUS_LED, GPIO_OUT);
    leds_fill(0, 0, 0);
    leds_show();
}

void leds_set(int i, uint8_t r, uint8_t g, uint8_t b) {
    if (i < 0 || i >= WS2812_COUNT) return;
    rgb[i][0] = r; rgb[i][1] = g; rgb[i][2] = b;
}

void leds_fill(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < WS2812_COUNT; i++) leds_set(i, r, g, b);
}

void leds_set_brightness(uint8_t s) { bright = s; }

void leds_show(void) {
    for (int i = 0; i < WS2812_COUNT; i++) {
        uint32_t r = rgb[i][0] * bright / 255, g = rgb[i][1] * bright / 255, b = rgb[i][2] * bright / 255;
        pio_sm_put_blocking(led_pio, led_sm, ((g << 16) | (r << 8) | b) << 8);   // GRB, left-aligned
    }
}

void status_led(bool on) { gpio_put(PIN_STATUS_LED, on); }
