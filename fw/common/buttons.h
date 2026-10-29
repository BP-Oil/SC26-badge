// 5-way joystick + A/B, active low with pull-ups.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "board_sc26.h"

enum {
    BTN_UP = 1 << 0, BTN_DOWN = 1 << 1, BTN_LEFT = 1 << 2, BTN_RIGHT = 1 << 3,
    BTN_CENTER = 1 << 4, BTN_B = 1 << 5, BTN_A = 1 << 6,
};
#define BTN_COUNT 7

void     buttons_init(void);
uint32_t buttons_read(void);                 // raw state, bit set = pressed
const char *button_name(int bit_index);
int      button_pin(int bit_index);
