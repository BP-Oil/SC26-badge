// Player settings kept across power cycles in one 4 KB flash sector (SETTINGS_OFFSET in
// flash_layout.h). OS-only UF2 updates and the USB drive never touch this sector.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SETTINGS_NAME_LEN 96

typedef struct {
    uint8_t stay;          // 1 = keep playing the current GIF (no auto-advance)
    uint8_t led_mode;
    uint8_t brightness;    // 10..100
    uint8_t zoom;
    uint8_t ring_cw;
    char    gif[SETTINGS_NAME_LEN];   // last GIF shown
} settings_t;

bool settings_load(settings_t *s);            // false (and defaults) if nothing valid stored
void settings_changed(const settings_t *s);   // queue a save (debounced)
void settings_poll(bool allowed);             // call from the main loop; writes when idle 3 s
void settings_flush(void);                    // write any pending change now
