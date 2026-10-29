// 9x WS2812 RGB LEDs on GP24 (PIO0) + single status LED on GP18.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "board_sc26.h"

void leds_init(void);
void leds_set(int i, uint8_t r, uint8_t g, uint8_t b);
void leds_fill(uint8_t r, uint8_t g, uint8_t b);
void leds_show(void);                       // push the buffer out (~270 us for 9 LEDs)
void leds_set_brightness(uint8_t scale);    // global 0..255 scale applied in leds_show()
void status_led(bool on);
