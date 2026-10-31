// LED engine for the 9-LED ring + GP18 status LED.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "scenes.h"

typedef enum {
    LEDMODE_ORIGINAL = 0,   // the stock firmware's purple sweep + 1 Hz status blink
    LEDMODE_AMBIENT,        // each ring LED follows the nearest edge of the picture
    LEDMODE_OFF,
    LEDMODE_SCENE,          // a scene from scenes.c (built-in or a LEDS/*.TXT file)
    LEDMODE_COUNT           // (SCENE was added after OFF so saved settings keep their meaning)
} led_mode_t;

void ambient_init(void);
void ambient_set_mode(led_mode_t m);
led_mode_t ambient_mode(void);
led_mode_t ambient_next_mode(led_mode_t m);     // B-button order: original, ambient, scenes, off
const char *ambient_mode_name(led_mode_t m);
void ambient_set_brightness(uint8_t pct);        // ambient-mode brightness, 0..100
void ambient_toggle_direction(void);             // mirror ring order if the LEDs run the other way
bool ambient_clockwise(void);
void ambient_set_clockwise(bool cw);
// Sample the framebuffer (big-endian RGB565, 360x360) inside the given picture rect.
void ambient_sample(const uint16_t *fb, int rx, int ry, int rw, int rh);
// Same, from a small thumbnail (w x h, big-endian RGB565) of a picture shown centred at `scale`x.
void ambient_sample_small(const uint16_t *img, int w, int h, int scale);
void ambient_tick(void);                          // call often; drives animation and smoothing
bool ambient_last_scene(scene_out_t *out);       // last frame sent in SCENES mode (false if none)
void ambient_scenes_live(bool on);               // let the 50 Hz scene timer drive the ring
