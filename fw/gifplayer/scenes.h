// LED scenes for the 9-LED rocket ring. The minibadge clock (GP18) is not touched.
//
// Built-in scenes are small functions in scenes.c. Custom scenes are plain text files in
// the LEDS folder of the "SC26 GIFS" drive (format in docs/LED_SCENES.md), so new scenes
// need no rebuild: copy a .TXT over, open the LED SCENES page, pick it.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "board_sc26.h"

#define SCENE_NAME_LEN 24

typedef struct {
    uint8_t rgb[WS2812_COUNT][3];   // physical LED order, brightness and gamma already applied
    uint8_t view[WS2812_COUNT][3];  // same colours as you'd see them, for an on-screen preview
} scene_out_t;

void        scenes_init(void);
void        scenes_scan(void);                    // re-read LEDS/*.TXT (drive must be mounted)
bool        scenes_make_example(void);            // create LEDS/ + examples if LEDS/ is missing
int         scenes_count(void);                   // built-in + custom
int         scenes_builtin_count(void);
const char *scenes_name(int i);
bool        scenes_is_custom(int i);
int         scenes_find(const char *name);        // -1 if not found
bool        scenes_select(int i, char *err, size_t errlen);   // loads a custom file if needed
int         scenes_current(void);

int         scenes_speed_count(void);
int         scenes_speed_pct(int idx);
int         scenes_speed_idx(void);
void        scenes_set_speed_idx(int idx);
uint8_t     scenes_brightness(void);              // 5..100 %
void        scenes_set_brightness(uint8_t pct);

// Record the running scene as a short seamless loop for the DOOM image to play back
// (format in scenes.c). Changes the scene's state: use it only when leaving for DOOM.
size_t      scenes_record_loop(uint8_t *buf, size_t max, uint32_t start_ms);

void        scenes_lock(void);                    // hold off the LED timer while changing state
void        scenes_unlock(void);
bool        scenes_busy(void);

void        scenes_led_pos(int led, float *x, float *y);   // physical LED -> rocket x,y (-1..1)
void        scenes_render(uint32_t now_ms, scene_out_t *out);   // advances scene time
