// LED engine.
//
// ORIGINAL mode reproduces the stock "Wayfarer" firmware exactly (decoded from its
// routine at 0x10010670): a 5-step sweep over the ring using the masks below, one
// step every 170 ms, colour (16,0,16); plus the status LED on GP18 blinking at 1 Hz.
// The masks show the ring order: LED7 is at the top, the sweep runs down both
// sides (6/8 -> 5/0 -> 4/1) and ends at the bottom pair (3/2).
//
// AMBIENT mode: the badge is the SAINTCON 2026 rocket. LED7 is the nose above the screen,
// and the other eight run down the two sides of the body in pairs: 6/8 beside the screen,
// 5/0 and 4/1 down the body, 3/2 at the fins (measured from a photo of the badge). Each
// LED takes the picture colour at a matching point on the picture's edge: nose = top
// centre, then each pair a little further down its side, fins = lower corners. So the
// picture "pours" down the rocket. For each LED we cast a ray from the screen centre at
// its sampling angle (table below), find where it leaves the picture
// rectangle, and average a 3x3 patch of samples near that edge (3 radii x 3 angles).
// The average is saturation-boosted, gamma-corrected and exponentially smoothed so the
// ring "breathes" with the image instead of flickering.

#include <math.h>
#include "pico/stdlib.h"
#include "leds.h"
#include "ambient.h"
#include "board_sc26.h"
#include "scenes.h"

static const uint16_t orig_masks[5] = { 0x080, 0x140, 0x021, 0x012, 0x00C };
#define ORIG_STEP_MS   170
#define TOP_LED        7

static led_mode_t mode = LEDMODE_ORIGINAL;
static uint8_t    amb_brightness = 45;          // percent
static bool       clockwise = false;     // rocket: 8,0,1,2 on the left (photo 2026-10-28)
static float      target[WS2812_COUNT][3], current[WS2812_COUNT][3];
static uint32_t   orig_step;
static absolute_time_t next_step, next_blink, next_push;
static bool       blink;
static scene_out_t last_scene;
static bool       have_scene;

bool ambient_last_scene(scene_out_t *out) {
    scenes_lock();                                // don't copy while the timer is writing it
    bool ok = have_scene;
    if (ok) *out = last_scene;
    scenes_unlock();
    return ok;
}

// SCENES mode runs from a 50 Hz timer interrupt rather than the main loop, so the ring keeps
// animating smoothly while the GIF decoder holds the main loop for a whole frame. The timer
// only touches the ring LEDs (never GP18), skips a tick while the main loop holds
// scenes_lock(), and stays quiet until ambient_scenes_live(true) (after the boot splash,
// which drives the LEDs itself).
static repeating_timer_t scene_timer;
static volatile bool     scenes_live;

static bool scene_tick(repeating_timer_t *rt) {
    (void)rt;
    if (!scenes_live || mode != LEDMODE_SCENE || scenes_busy()) return true;
    scene_out_t o;
    scenes_render(to_ms_since_boot(get_absolute_time()), &o);
    last_scene = o; have_scene = true;
    leds_set_brightness(255);                     // scenes apply brightness themselves
    for (int i = 0; i < WS2812_COUNT; i++) leds_set(i, o.rgb[i][0], o.rgb[i][1], o.rgb[i][2]);
    leds_show();
    return true;
}

void ambient_scenes_live(bool on) {
    static bool started;
    scenes_live = on;
    if (on && !started) started = add_repeating_timer_ms(-20, scene_tick, NULL, &scene_timer);
}

static const char *names[LEDMODE_COUNT] = { "ORIGINAL", "AMBIENT", "OFF", "SCENES" };
const char *ambient_mode_name(led_mode_t m) { return m < LEDMODE_COUNT ? names[m] : "?"; }
led_mode_t ambient_mode(void) { return mode; }
led_mode_t ambient_next_mode(led_mode_t m) {
    switch (m) {
    case LEDMODE_ORIGINAL: return LEDMODE_AMBIENT;
    case LEDMODE_AMBIENT:  return LEDMODE_SCENE;
    case LEDMODE_SCENE:    return LEDMODE_OFF;
    default:               return LEDMODE_ORIGINAL;
    }
}
bool ambient_clockwise(void) { return clockwise; }
void ambient_toggle_direction(void) { clockwise = !clockwise; }
void ambient_set_clockwise(bool cw) { clockwise = cw; }
void ambient_set_brightness(uint8_t pct) { amb_brightness = pct > 100 ? 100 : pct; }

void ambient_init(void) {
    leds_init();
    next_step = next_blink = next_push = get_absolute_time();
}

void ambient_set_mode(led_mode_t m) {
    scenes_lock();                                // the scene timer must not write mid-change
    mode = m;
    leds_fill(0, 0, 0);
    leds_set_brightness(255);
    leds_show();
    scenes_unlock();
    status_led(false);
    for (int i = 0; i < WS2812_COUNT; i++)
        for (int c = 0; c < 3; c++) current[i][c] = target[i][c] = 0;
}

// angle in radians (screen coords: 0 = right, +pi/2 = down) of ring LED i
// sampling angle per LED in degrees (screen coords: 0 = right, 90 = down) for LEDs 8,0,1,2 on
// the right-hand side; the SAINTCON rocket actually has them on the LEFT, which is the
// default (clockwise = false mirrors this table). Hold B to flip.
static const int16_t rocket_deg[WS2812_COUNT] = {
    0,      // LED0  right side, level with the screen centre
    26,     // LED1  right side, lower body
    53,     // LED2  right fin
    127,    // LED3  left fin
    154,    // LED4  left side, lower body
    180,    // LED5  left side, level with the screen centre
    208,    // LED6  left, beside the screen
    -90,    // LED7  nose
    -28,    // LED8  right, beside the screen
};
static float led_angle(int i) {
    float d = rocket_deg[i];
    if (!clockwise) d = 180.0f - d;               // mirror left/right
    return d * (float)M_PI / 180.0f;
}

static inline void px_rgb(uint16_t be, float *r, float *g, float *b) {
    uint16_t p = (uint16_t)((be >> 8) | (be << 8));
    *r = (float)((p >> 11) & 31) * (255.0f / 31);
    *g = (float)((p >> 5) & 63) * (255.0f / 63);
    *b = (float)(p & 31) * (255.0f / 31);
}

typedef uint16_t (*px_fn)(const void *ctx, int x, int y);       // screen coords -> BE RGB565

static void sample_common(px_fn get, const void *ctx, int rx, int ry, int rw, int rh) {
    if (mode != LEDMODE_AMBIENT || rw <= 0 || rh <= 0) return;
    const float cx = LCD_WIDTH / 2.0f, cy = LCD_HEIGHT / 2.0f;
    // picture rect clipped to the screen, relative to centre
    float x0 = fmaxf((float)rx, 0) - cx, x1 = fminf((float)(rx + rw - 1), LCD_WIDTH - 1) - cx;
    float y0 = fmaxf((float)ry, 0) - cy, y1 = fminf((float)(ry + rh - 1), LCD_HEIGHT - 1) - cy;
    for (int i = 0; i < WS2812_COUNT; i++) {
        float a = led_angle(i), sr = 0, sg = 0, sb = 0;
        int n = 0;
        for (int da = -1; da <= 1; da++) {
            float ang = a + da * 0.21f;                    // +/- 12 degrees
            float ux = cosf(ang), uy = sinf(ang);
            // distance to the picture edge along this ray, capped to the panel radius
            float t = 170.0f;
            if (ux > 1e-3f) t = fminf(t, x1 / ux); else if (ux < -1e-3f) t = fminf(t, x0 / ux);
            if (uy > 1e-3f) t = fminf(t, y1 / uy); else if (uy < -1e-3f) t = fminf(t, y0 / uy);
            if (t < 4) continue;
            for (int k = 0; k < 3; k++) {
                float rr = t * (0.72f + 0.12f * k);
                int x = (int)(cx + ux * rr), y = (int)(cy + uy * rr);
                if (x < rx || y < ry || x >= rx + rw || y >= ry + rh) continue;
                float r, g, b;
                px_rgb(get(ctx, x, y), &r, &g, &b);
                sr += r; sg += g; sb += b; n++;
            }
        }
        if (!n) { target[i][0] = target[i][1] = target[i][2] = 0; continue; }
        float r = sr / n, g = sg / n, b = sb / n;
        // saturation boost around the luma
        float l = 0.299f * r + 0.587f * g + 0.114f * b;
        r = l + (r - l) * 1.5f; g = l + (g - l) * 1.5f; b = l + (b - l) * 1.5f;
        float c[3] = { r, g, b };
        for (int j = 0; j < 3; j++) {
            float v = fminf(fmaxf(c[j], 0), 255) / 255.0f;
            target[i][j] = powf(v, 2.2f) * 255.0f;    // perceptual -> LED linear
        }
    }
}

static uint16_t fb_get(const void *ctx, int x, int y) { return ((const uint16_t *)ctx)[y * LCD_WIDTH + x]; }

void ambient_sample(const uint16_t *fb, int rx, int ry, int rw, int rh) {
    sample_common(fb_get, fb, rx, ry, rw, rh);
}

typedef struct { const uint16_t *img; int w, h, scale, rx, ry; } small_ctx_t;
static uint16_t small_get(const void *ctx, int x, int y) {
    const small_ctx_t *s = ctx;
    int ix = (x - s->rx) / s->scale, iy = (y - s->ry) / s->scale;
    if (ix >= s->w) ix = s->w - 1;
    if (iy >= s->h) iy = s->h - 1;
    return s->img[iy * s->w + ix];
}

void ambient_sample_small(const uint16_t *img, int w, int h, int scale) {
    small_ctx_t s = { img, w, h, scale, (LCD_WIDTH - w * scale) / 2, (LCD_HEIGHT - h * scale) / 2 };
    sample_common(small_get, &s, s.rx, s.ry, w * scale, h * scale);
}

void ambient_tick(void) {
    if (mode == LEDMODE_OFF) return;

    if (time_reached(next_blink)) {                    // status LED: 1 Hz like the original
        next_blink = delayed_by_ms(next_blink, 500);
        blink = !blink;
        status_led(blink);
    }

    if (mode == LEDMODE_SCENE) return;                 // drawn by scene_tick() (timer)

    if (mode == LEDMODE_ORIGINAL) {
        if (!time_reached(next_step)) return;
        next_step = delayed_by_ms(next_step, ORIG_STEP_MS);
        uint16_t m = orig_masks[orig_step++ % 5];
        leds_set_brightness(255);
        for (int i = 0; i < WS2812_COUNT; i++)
            leds_set(i, (m >> i) & 1 ? 16 : 0, 0, (m >> i) & 1 ? 16 : 0);
        leds_show();
        return;
    }

    // AMBIENT: smooth towards the latest sample at ~50 Hz
    if (!time_reached(next_push)) return;
    next_push = make_timeout_time_ms(20);
    leds_set_brightness((uint8_t)(amb_brightness * 255 / 100));
    for (int i = 0; i < WS2812_COUNT; i++) {
        for (int j = 0; j < 3; j++) current[i][j] += (target[i][j] - current[i][j]) * 0.35f;
        leds_set(i, (uint8_t)current[i][0], (uint8_t)current[i][1], (uint8_t)current[i][2]);
    }
    leds_show();
}
