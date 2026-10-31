// Player settings kept across power cycles in one 4 KB flash sector (SETTINGS_OFFSET in
// flash_layout.h). OS-only UF2 updates and the USB drive never touch this sector.
// Layout in settings.c; the DOOM image reads it too (p3/rp2040-doom/src/pico/sc26_doom.c).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SETTINGS_NAME_LEN 96
#define SETTINGS_BLOB_MAX 3513     // room for a file scene's frames: 4 + 121 x 29 bytes

typedef struct {
    uint8_t stay;          // 1 = keep playing the current GIF (no auto-advance)
    uint8_t led_mode;
    uint8_t brightness;    // 10..100
    uint8_t zoom;
    uint8_t ring_cw;
    char    gif[SETTINGS_NAME_LEN];   // last GIF shown
    // ---- LED scenes (stored in their own record, see settings.c)
    char    scene[24];     // scene name (built-in name or LEDS/ file name)
    uint8_t scene_speed;   // index into the scenes.c speed table
    uint8_t scene_bright;  // 5..100 %
} settings_t;

bool settings_load(settings_t *s);            // false (and defaults) if nothing valid stored
// queue a save (debounced); blob = the selected file scene's frames, or NULL
void settings_changed(const settings_t *s, const uint8_t *blob, size_t blob_len);
void settings_poll(bool allowed);             // call from the main loop; writes when idle 3 s
void settings_flush(void);                    // write any pending change now
