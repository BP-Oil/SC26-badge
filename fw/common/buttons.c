#include "buttons.h"
#include "pico/stdlib.h"

// Same bit order as the original firmware's button bitmap.
static const uint8_t pins[BTN_COUNT] = {
    PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_LEFT, PIN_BTN_RIGHT, PIN_BTN_CENTER, PIN_BTN_B, PIN_BTN_A
};
static const char *names[BTN_COUNT] = { "UP", "DOWN", "LEFT", "RIGHT", "CENTER", "B", "A" };

void buttons_init(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
    gpio_init(PIN_OFFLINE_STRAP);
    gpio_set_dir(PIN_OFFLINE_STRAP, GPIO_IN);
    gpio_pull_up(PIN_OFFLINE_STRAP);
}

uint32_t buttons_read(void) {
    uint32_t in = gpio_get_all(), out = 0;
    for (int i = 0; i < BTN_COUNT; i++)
        if (!(in & (1u << pins[i]))) out |= 1u << i;
    return out;
}

const char *button_name(int i) { return (i >= 0 && i < BTN_COUNT) ? names[i] : "?"; }
int button_pin(int i) { return (i >= 0 && i < BTN_COUNT) ? pins[i] : -1; }
