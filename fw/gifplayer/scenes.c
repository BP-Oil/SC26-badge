// LED scene engine. Scenes drive the 9 ring LEDs only; the minibadge clock (GP18) is left
// to the rest of SC26 OS (the stock 1 Hz blink).
//
// Geometry. The rocket's 9 LEDs are handled in "ring order" k = 0..8, going clockwise as
// you look at the badge: k0 is the nose, k1..k4 run down the right-hand side to the right
// fin, k5..k8 run from the left fin back up the left-hand side. Each k has a position
// (x right, y down, roughly -1..1 around the screen centre) and a row (0 = nose ..
// 4 = fins), so scenes can be written as "sweep down the rocket", "light the fins", etc.
// The physical LED index for each k comes from the same measurement ambient.c uses
// (LED7 = nose; 8,0,1,2 down the left side; 6,5,4,3 down the right) and follows the
// "swap LED sides" setting.
//
// Scenes render floating-point colour (0..255, as you would pick it on a screen) in ring
// order. scenes_render() applies gamma and brightness in float, then maps to physical
// order, so low-brightness fades stay smooth.

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "fatfs/ff.h"
#include "scenes.h"
#include "ambient.h"

#define NLED        WS2812_COUNT
#define TAU         6.2831853f
#define MAX_CUSTOM  24
#define MAX_FRAMES  256
#define SCENE_DIR   "LEDS"

typedef void (*scene_fn)(float t, float dt, float c[NLED][3]);

// ------------------------------------------------------------------ geometry
static const float px[NLED] = { 0.00f, 0.90f, 1.00f, 0.90f, 0.60f, -0.60f, -0.90f, -1.00f, -0.90f };
static const float py[NLED] = { -1.00f, -0.45f, 0.00f, 0.45f, 0.85f, 0.85f, 0.45f, 0.00f, -0.45f };
static const uint8_t row[NLED] = { 0, 1, 2, 3, 4, 4, 3, 2, 1 };
// ring order -> physical LED, for the two ways the sides can be wired up
static const uint8_t ring_left[NLED]  = { 7, 6, 5, 4, 3, 2, 1, 0, 8 };   // default: 8,0,1,2 on the left
static const uint8_t ring_right[NLED] = { 7, 8, 0, 1, 2, 3, 4, 5, 6 };   // "LED SIDES SWAPPED"

// ------------------------------------------------------------------ helpers
static uint32_t rng = 0x2545F491u;
static float frand(void) {                       // 0..1
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(rng >> 8) * (1.0f / 16777216.0f);
}
static float fract(float x) { return x - floorf(x); }
static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float hashf(int i) {                      // fixed pseudo-random 0..1 per integer
    uint32_t h = (uint32_t)i * 2654435761u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return (float)(h & 0xFFFFFF) * (1.0f / 16777216.0f);
}
static float noise1(int seed, float x) {         // smooth value noise, 0..1
    int i = (int)floorf(x); float f = x - (float)i;
    f = f * f * (3 - 2 * f);
    float a = hashf(i * 131 + seed * 7919), b = hashf((i + 1) * 131 + seed * 7919);
    return a + (b - a) * f;
}
static void set(float c[3], float r, float g, float b) { c[0] = r; c[1] = g; c[2] = b; }
static void hsv(float c[3], float h, float s, float v) {
    h = fract(h) * 6; int i = (int)h; float f = h - (float)i;
    float p = v * (1 - s), q = v * (1 - s * f), u = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i) {
    case 0: r = v; g = u; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = u; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = u; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    set(c, r * 255, g * 255, b * 255);
}
static void heat(float c[3], float h) {          // black -> red -> orange -> yellow -> white
    h = clamp01(h) * 3;
    if (h < 1)      set(c, 255 * h, 0, 0);
    else if (h < 2) set(c, 255, 200 * (h - 1), 0);
    else            set(c, 255, 200 + 55 * (h - 2), 255 * (h - 2) * 0.8f);
}

// ------------------------------------------------------------------ built-in scenes
// t = scene time in seconds (already scaled by the speed setting), dt = step in seconds.

static void sc_rainbow(float t, float dt, float c[NLED][3]) {
    (void)dt;
    for (int k = 0; k < NLED; k++) hsv(c[k], (float)k / NLED + t * 0.2f, 1, 1);
}

static void sc_comet(float t, float dt, float c[NLED][3]) {
    (void)dt;
    float head = fract(t * 0.9f) * NLED;
    float hue = 0.52f + 0.12f * sinf(t * 0.35f);
    for (int k = 0; k < NLED; k++) {
        float d = head - (float)k;
        if (d < 0) d += NLED;                    // distance behind the head
        float v = expf(-d * 0.6f);
        hsv(c[k], hue + d * 0.02f, d < 0.6f ? 0.25f : 1.0f, v);
    }
}

static void sc_thrusters(float t, float dt, float c[NLED][3]) {
    (void)dt;
    for (int k = 1; k < NLED; k++) {
        float base = clamp01((py[k] + 0.3f) / 1.15f);           // strongest at the fins
        float n = noise1(k, t * 14.0f) * 0.6f + noise1(k + 40, t * 5.0f) * 0.4f;
        heat(c[k], base * (0.55f + 0.6f * n));
    }
    if (fract(t * 0.9f) < 0.07f)                // nav beacon on the nose
        set(c[0], 255, 255, 255);
    else
        set(c[0], 30, 0, 0);
}

static void sc_fire(float t, float dt, float c[NLED][3]) {
    (void)dt;
    for (int k = 0; k < NLED; k++) {
        float n = noise1(k * 3, t * 9.0f) * 0.65f + noise1(k * 3 + 1, t * 23.0f) * 0.35f;
        float cool = 0.55f + 0.45f * (py[k] + 1.0f) / 1.85f;   // flames cooler near the nose
        heat(c[k], (0.3f + 0.7f * n) * cool);
    }
}

static void sc_plasma(float t, float dt, float c[NLED][3]) {
    (void)dt;
    for (int k = 0; k < NLED; k++) {
        float v = sinf(px[k] * 2.0f + t) + sinf(py[k] * 2.3f - t * 1.3f) + sinf((px[k] + py[k]) * 1.7f + t * 0.7f);
        hsv(c[k], v / 6.0f + t * 0.05f, 1, 1);
    }
}

static void sc_breathe(float t, float dt, float c[NLED][3]) {
    (void)dt;
    // the breath rolls from the nose down to the fins, and the colour drifts round the ring
    for (int k = 0; k < NLED; k++) {
        float ph = (1 - cosf(TAU * (t * 0.25f - row[k] * 0.07f))) * 0.5f;
        float v = 0.04f + 0.96f * powf(ph, 1.6f);
        hsv(c[k], t * 0.02f + (float)k / NLED * 0.12f, 0.85f, v);
    }
}

static void sc_scanner(float t, float dt, float c[NLED][3]) {
    (void)dt;
    float p = fract(t * 0.45f);
    p = p < 0.5f ? p * 2 : 2 - p * 2;            // ping-pong 0..1
    float y = -1.0f + p * 1.85f;                 // nose .. fins
    for (int k = 0; k < NLED; k++) {
        float d = py[k] - y;
        float v = expf(-d * d / 0.05f);
        set(c[k], 255 * (0.04f + 0.96f * v), 10 * v, 0);
    }
}

static void sc_hyperspace(float t, float dt, float c[NLED][3]) {
    (void)dt;
    for (int k = 0; k < NLED; k++) {
        float ph = fract(t * 1.4f - row[k] * 0.2f + (k > 4 ? 0.5f : 0.0f) + hashf(k) * 0.15f);
        float v = powf(ph, 7.0f);
        set(c[k], 10 + 245 * v, 14 + 230 * v, 60 + 195 * v);
    }
}

static float star[NLED];
static void sc_starfield(float t, float dt, float c[NLED][3]) {
    (void)t;
    for (int k = 0; k < NLED; k++) {
        star[k] *= expf(-dt * 2.5f);
        if (frand() < dt * 0.7f) star[k] = 1.0f;
        float v = star[k], tint = hashf(k + (int)(t * 0.5f) * 17);
        float r = tint < 0.3f ? 255 : 200, b = tint > 0.7f ? 255 : 200;
        set(c[k], 6 + r * v, 3 + 210 * v, 18 + b * v);
    }
}

static void sc_red_alert(float t, float dt, float c[NLED][3]) {
    (void)dt;
    // a rotating red beacon sweeping round the hull over a slow alarm pulse
    float head = fract(t * 0.8f) * NLED;
    float pulse = 0.08f + 0.22f * powf(sinf((float)M_PI * fract(t * 0.7f)), 2);
    for (int k = 0; k < NLED; k++) {
        float d1 = fabsf(head - k), d2 = fabsf(fmodf(head + NLED / 2.0f, NLED) - k);
        d1 = fminf(d1, NLED - d1); d2 = fminf(d2, NLED - d2);
        float beam = expf(-d1 * d1 * 0.9f) + 0.6f * expf(-d2 * d2 * 0.9f);
        set(c[k], 255 * fminf(1.0f, pulse + beam), 18 * beam, 0);
    }
}

static void sc_heartbeat(float t, float dt, float c[NLED][3]) {
    (void)dt;
    // each beat starts across the middle of the rocket and ripples out to the nose and fins
    for (int k = 0; k < NLED; k++) {
        float lag = fabsf((float)row[k] - 2.0f) * 0.07f;
        float ph = fract((t - lag) / 0.85f);
        float l = (ph - 0.02f) / 0.05f, d = (ph - 0.24f) / 0.06f;
        float e = expf(-l * l) + 0.7f * expf(-d * d);
        float fade = 1.0f - 0.18f * fabsf((float)row[k] - 2.0f);
        set(c[k], 255 * (0.03f + 0.97f * e * fade), 20 * e, 40 * e);
    }
}

static void sc_aurora(float t, float dt, float c[NLED][3]) {
    (void)dt;
    for (int k = 0; k < NLED; k++) {
        float n = 0.5f + 0.5f * sinf(py[k] * 3.0f + t * 0.7f + px[k] * 1.3f);
        float u = 0.5f + 0.5f * sinf(t * 0.23f + px[k] * 1.5f + py[k] * 0.8f);
        float hue = u < 0.5f ? 0.33f + (0.47f - 0.33f) * u * 2 : 0.47f + (0.78f - 0.47f) * (u - 0.5f) * 2;
        hsv(c[k], hue, 0.9f, 0.18f + 0.82f * n * n);
    }
}

// The stock badge's sweep (nose -> fins, 5 steps), cycling the five crew colours.
static void sc_crew(float t, float dt, float c[NLED][3]) {
    (void)dt;
    static const uint8_t crew[5][3] = { {0,0,255}, {255,96,0}, {255,255,255}, {0,255,0}, {255,96,96} };
    int step = (int)(t / 0.17f);
    int lit = step % 5, col = (step / 5) % 5;
    for (int k = 0; k < NLED; k++) {
        int behind = (lit - row[k] + 5) % 5;
        float v = behind == 0 ? 1.0f : behind == 1 ? 0.18f : 0.0f;
        set(c[k], crew[col][0] * v, crew[col][1] * v, crew[col][2] * v);
    }
}

static void sc_ion_storm(float t, float dt, float c[NLED][3]) {
    static float flash_t = -1, flash_len; static int side;
    if (flash_t < 0 && frand() < dt * 0.5f) { flash_t = 0; flash_len = 0.18f + frand() * 0.25f; side = (int)(frand() * 3); }
    float f = 0;
    if (flash_t >= 0) {
        flash_t += dt;
        f = (fract(flash_t * 14.0f) < 0.5f || flash_t < 0.04f) ? 1.0f : 0.25f;   // stuttering bolt
        if (flash_t > flash_len) flash_t = -1;
    }
    for (int k = 0; k < NLED; k++) {
        float glow = 0.12f + 0.1f * noise1(k + 70, t * 1.5f);
        bool hit = side == 2 || (side == 0 ? k <= 4 : (k == 0 || k > 4));
        float v = hit ? f : f * 0.2f;
        set(c[k], 60 * glow + 200 * v, 20 * glow + 210 * v, 160 * glow + 255 * v);
    }
}

// ---- space horror -------------------------------------------------------------------

// Motion tracker: a green ping sweeps nose -> fins. Each ping the contact blip is a row
// closer to the nose... and when it reaches you, everything goes red.
static void sc_motion_tracker(float t, float dt, float c[NLED][3]) {
    (void)t;
    static float ph, alarm; static int dist = 4, side;
    for (int k = 0; k < NLED; k++) set(c[k], 0, 6, 0);
    if (alarm > 0) {                             // contact: red strobe, then start over
        alarm -= dt;
        float v = fract(alarm * 5.0f) < 0.5f ? 1.0f : 0.15f;
        for (int k = 0; k < NLED; k++) set(c[k], 255 * v, 0, 0);
        if (alarm <= 0) { dist = 4; ph = 0; }
        return;
    }
    float period = 0.55f + 0.3f * dist;          // pings speed up as it closes in
    ph += dt / period;
    if (ph >= 1) {
        ph = 0;
        side = frand() < 0.5f;
        if (--dist < 0) { alarm = 1.4f; return; }
    }
    float w = ph * 6.0f - 0.5f;                  // sweep position in rows
    for (int k = 0; k < NLED; k++) {
        float d = (float)row[k] - w;
        float sweep = d < 0 ? expf(d * 1.5f) * 0.55f : expf(-d * d * 8) * 0.55f;
        bool blip = row[k] == dist && (dist == 0 || (side ? k <= 4 : k > 4));
        float b = blip && w > (float)dist ? expf(-(w - dist) * 1.2f) : 0;
        set(c[k], 230 * b, 30 + 225 * fmaxf(sweep, b), 20 * sweep + 160 * b);
    }
}

// Hull breach: amber warning beacons spinning, a red klaxon pulse, lights dropping out.
static void sc_hull_breach(float t, float dt, float c[NLED][3]) {
    (void)dt;
    float head = fract(t * 1.1f) * NLED, klax = powf(fmaxf(0, sinf(t * 5.0f)), 3);
    for (int k = 0; k < NLED; k++) {
        float d1 = fabsf(head - k), d2 = fabsf(fmodf(head + NLED / 2.0f, NLED) - k);
        d1 = fminf(d1, NLED - d1); d2 = fminf(d2, NLED - d2);
        float a = expf(-d1 * d1 * 1.3f) + expf(-d2 * d2 * 1.3f);
        bool drop = noise1(k + 300, t * 4.0f) > 0.78f && fract(t * 23.0f + k * 0.3f) < 0.5f;
        float m = drop ? 0.05f : 1.0f;
        set(c[k], m * (255 * a + 160 * klax), m * 120 * a, 0);
    }
}

// Derelict: the last cold panel lights on a dead ship, buzzing, sputtering, a few gone dark.
static void sc_derelict(float t, float dt, float c[NLED][3]) {
    (void)dt;
    for (int k = 0; k < NLED; k++) {
        bool dead = hashf(k + 50) < 0.3f;
        float n = noise1(k + 60, t * 2.5f), v;
        if (dead) v = noise1(k + 80, t * 1.2f) > 0.82f && fract(t * 31.0f) < 0.4f ? 0.75f : 0.0f;
        else if (n < 0.33f) v = fract(t * 37.0f + k * 0.17f) < 0.45f ? 0.0f : 0.75f;   // sputter
        else v = 0.55f + 0.06f * noise1(k, t * 40.0f);                                 // hum
        set(c[k], 150 * v, 185 * v, 230 * v);
    }
}

// Possessed: a slow, deep red breathing... then without warning the ring convulses.
static void sc_possessed(float t, float dt, float c[NLED][3]) {
    static float fit, wait = 3.0f;
    wait -= dt;
    if (wait <= 0 && fit <= 0) { fit = 0.5f + frand() * 0.7f; wait = 2.5f + frand() * 4.0f; }
    float breath = 0.25f + 0.75f * powf(0.5f + 0.5f * sinf(t * 1.1f), 2);
    float shadow = fract(t * 0.18f) * NLED;          // something dark crawling round the hull
    for (int k = 0; k < NLED; k++) {
        float d = fabsf(shadow - k); d = fminf(d, NLED - d);
        float m = 1.0f - 0.9f * expf(-d * d * 0.6f);
        set(c[k], 120 * breath * m + 40 * (1 - m), 0, 6 * breath * m + 20 * (1 - m));
    }
    if (fit > 0) {
        fit -= dt;
        for (int k = 0; k < NLED; k++) {
            float r = frand();
            if (r < 0.35f)      set(c[k], 170, 0, 255);
            else if (r < 0.55f) set(c[k], 60, 255, 30);
            else if (r < 0.62f) set(c[k], 255, 255, 255);
            else                set(c[k], 0, 0, 0);
        }
    }
}

// Pumpkin nebula: slow swirling gas in Halloween orange and purple.
static void sc_pumpkin_nebula(float t, float dt, float c[NLED][3]) {
    (void)dt;
    for (int k = 0; k < NLED; k++) {
        float v = sinf(px[k] * 1.7f + t * 0.6f) + sinf(py[k] * 2.1f - t * 0.45f) + sinf((px[k] - py[k]) * 1.3f + t * 0.3f);
        float u = clamp01(v / 6.0f + 0.5f);
        float r, g, b;                           // purple -> near black -> orange -> amber
        if (u < 0.35f)      { float a = u / 0.35f;          r = 110 - 95 * a; g = 0;          b = 170 - 155 * a; }
        else if (u < 0.6f)  { float a = (u - 0.35f) / 0.25f; r = 15 + 240 * a; g = 85 * a;     b = 15 - 15 * a; }
        else                { float a = (u - 0.6f) / 0.4f;   r = 255;          g = 85 + 85 * a; b = 30 * a; }
        set(c[k], r, g, b);
    }
}

// Ghost ship: pale wisps drifting up the hull from the fins and fading at the nose.
static void sc_ghost_ship(float t, float dt, float c[NLED][3]) {
    (void)dt;
    for (int k = 0; k < NLED; k++) {
        float v = 0;
        for (int w = 0; w < 2; w++) {
            float cyc = t * 0.32f + w * 0.5f + (k > 4 ? 0.23f : 0.0f);
            float y = 1.0f - fract(cyc) * 2.3f;              // fins -> past the nose
            float d = py[k] - y;
            v += expf(-d * d / 0.07f) * (0.5f + 0.5f * hashf((int)cyc * 7 + w + (k > 4) * 3));
        }
        v = fminf(v, 1.0f);
        set(c[k], 4 + 150 * v, 14 + 240 * v, 12 + 195 * v);
    }
}

// Blood moon: a deep red glow with an eclipse shadow creeping across it.
static void sc_blood_moon(float t, float dt, float c[NLED][3]) {
    (void)dt;
    float s = -2.2f + fract(t / 9.0f) * 4.4f;               // shadow crosses left -> right
    float glow = 0.75f + 0.25f * sinf(t * 0.8f);
    for (int k = 0; k < NLED; k++) {
        float d = px[k] - s;
        float shade = 1.0f - 0.9f * expf(-d * d / 0.35f);
        float v = glow * shade * (k == 0 ? 1.0f : 0.85f);
        set(c[k], 200 * v, 12 * v, 4 * v);
    }
}

// Infection: something green spreads up from the engines, LED by LED... purge... again.
static void sc_infection(float t, float dt, float c[NLED][3]) {
    (void)dt;
    const float cycle = 6.0f;
    int n = (int)(t / cycle);
    float u = t - (float)n * cycle;
    for (int k = 0; k < NLED; k++) {
        float when = (4 - row[k]) * 0.8f + hashf(k + n * 13) * 0.7f;
        if (u < when) { set(c[k], 0, 12, 40); continue; }       // healthy ship light
        float age = u - when;
        float pulse = 0.55f + 0.45f * noise1(k + n * 5, t * 3.0f);
        float bloom = fminf(age * 3.0f, 1.0f);
        set(c[k], 60 * pulse * bloom, 255 * pulse * bloom, 25 * pulse * bloom + 30 * (1 - bloom));
    }
    if (u > 5.0f) {                                            // purge flash
        float f = expf(-(u - 5.0f) * 5.0f);
        for (int k = 0; k < NLED; k++) set(c[k], 255 * f, 255 * f, 255 * f);
    }
}

// Shadow eyes: darkness, then a pair of eyes opens somewhere on the hull, blinks, and is gone.
static void sc_shadow_eyes(float t, float dt, float c[NLED][3]) {
    (void)dt;
    for (int k = 0; k < NLED; k++) set(c[k], 2, 0, 4);
    static const float per[2] = { 2.9f, 4.1f }, off[2] = { 0.0f, 1.7f };
    for (int e = 0; e < 2; e++) {
        float tt = t + off[e];
        int n = (int)(tt / per[e]);
        float u = tt - (float)n * per[e];
        int r = 1 + (int)(hashf(n * 3 + e * 101) * 4.0f);       // row 1..4
        bool yellow = hashf(n * 5 + e * 77) < 0.4f;
        float v = 0;
        if (u > 0.3f && u < 2.3f) {
            v = fminf((u - 0.3f) / 0.25f, 1.0f) * fminf((2.3f - u) / 0.25f, 1.0f);
            if (u > 1.25f && u < 1.38f) v = 0;                  // blink
        }
        int a = r, b = NLED - r;                                // same row, both sides
        float cr = 255, cg = yellow ? 200 : 0, cb = 0;
        for (int i = 0; i < 2; i++) {
            int k = i ? b : a;
            c[k][0] = fmaxf(c[k][0], cr * v); c[k][1] = fmaxf(c[k][1], cg * v); c[k][2] = fmaxf(c[k][2], cb * v);
        }
    }
}

// ---- more space ---------------------------------------------------------------------

static void sc_pulsar(float t, float dt, float c[NLED][3]) {
    (void)dt;
    float head = fract(t * 2.2f) * NLED;
    for (int k = 0; k < NLED; k++) {
        float d = fabsf(head - k); d = fminf(d, NLED - d);
        float v = expf(-d * d * 3.0f);
        set(c[k], 25 + 200 * v, 10 + 230 * v, 60 + 195 * v);
    }
}

static void sc_solar_flare(float t, float dt, float c[NLED][3]) {
    static float pos = -1, life;                  // an eruption running round the ring
    static int dir = 1;
    if (pos < 0 && frand() < dt * 0.6f) { pos = frand() * NLED; life = 1.0f; dir = frand() < 0.5f ? 1 : -1; }
    if (pos >= 0) { pos = fmodf(pos + dir * dt * 5.0f + NLED, NLED); life -= dt * 0.8f; if (life <= 0) pos = -1; }
    for (int k = 0; k < NLED; k++) {
        float base = 0.42f + 0.22f * noise1(k + 200, t * 2.0f);
        float f = 0;
        if (pos >= 0) { float d = fabsf(pos - k); d = fminf(d, NLED - d); f = life * expf(-d * d * 1.2f); }
        heat(c[k], fminf(base + 0.65f * f, 1.0f));
    }
}

static void sc_orbit(float t, float dt, float c[NLED][3]) {
    (void)dt;
    float a = fract(t * 0.6f) * NLED, b = fract(-t * 0.45f) * NLED;
    for (int k = 0; k < NLED; k++) {
        float da = fabsf(a - k), db = fabsf(b - k);
        da = fminf(da, NLED - da); db = fminf(db, NLED - db);
        float va = expf(-da * da * 1.5f), vb = expf(-db * db * 1.5f);
        set(c[k], 255 * vb, 200 * va, 255 * va + 200 * vb);
    }
}

// Docking lights: steady red (left) / green (right) nav lights, double white strobe on the
// nose, red anti-collision beacon on the fins.
static void sc_docking(float t, float dt, float c[NLED][3]) {
    (void)dt;
    float u = fract(t / 1.3f);
    bool strobe = u < 0.05f || (u > 0.12f && u < 0.17f);
    float beacon = powf(fmaxf(0, sinf(t * 4.0f)), 4);
    for (int k = 1; k < NLED; k++) {
        if (k <= 4) set(c[k], 0, 110, 20); else set(c[k], 110, 0, 0);
    }
    set(c[4], 255 * beacon, 0, 0); set(c[5], 255 * beacon, 0, 0);
    if (strobe) set(c[0], 255, 255, 255); else set(c[0], 8, 8, 8);
}

// ---- slow Halloween ambience -----------------------------------------------------------

static void candle_rgb(float c[3], float f) {    // warm flame colour for brightness f (0..1)
    f = clamp01(f);
    set(c, 255 * f, (55 + 95 * f) * f, 14 * f * f);
}

// Candlelight: every LED its own candle - a slow sway, a quick shimmer, the odd gutter.
static void sc_candlelight(float t, float dt, float c[NLED][3]) {
    (void)dt;
    for (int k = 0; k < NLED; k++) {
        float f = 0.55f + 0.25f * noise1(k + 400, t * 1.3f) + 0.12f * noise1(k + 430, t * 4.5f)
                + 0.05f * noise1(k + 460, t * 11.0f);
        float gutter = clamp01((noise1(k + 490, t * 0.35f) - 0.1f) / 0.12f);   // rare dips
        candle_rgb(c[k], f * (0.35f + 0.65f * gutter));
    }
}

// Jack-o'-lantern: candlelit face - green stem on the nose, bright eyes, glowing cheeks and a
// jagged mouth down at the fins, all lit by one flickering candle inside.
static void sc_jack_o_lantern(float t, float dt, float c[NLED][3]) {
    (void)dt;
    float inside = 0.62f + 0.22f * noise1(500, t * 1.6f) + 0.1f * noise1(501, t * 6.0f);
    float squint = 1.0f - 0.55f * clamp01((noise1(502, t * 0.25f) - 0.72f) / 0.08f);
    set(c[0], 30, 70 + 10 * noise1(503, t), 8);                     // stem
    for (int k = 1; k < NLED; k++) {
        float part = row[k] == 1 ? 1.0f * squint                    // eyes
                   : row[k] == 2 ? 0.35f                            // cheeks
                   : row[k] == 4 ? 0.95f : 0.7f;                    // mouth: teeth, corners
        float own = 0.9f + 0.1f * noise1(k + 510, t * 8.0f);
        candle_rgb(c[k], inside * part * own);
    }
}

// Lurking eyes: in a dim violet dark, pairs of eyes slowly open, glance about, blink
// lazily, and sink back into the dark.
static void sc_lurking_eyes(float t, float dt, float c[NLED][3]) {
    (void)dt;
    float dark = 0.6f + 0.4f * sinf(t * 0.4f);
    for (int k = 0; k < NLED; k++) set(c[k], 10 * dark, 0, 18 * dark);
    static const float per[2] = { 6.3f, 8.9f }, off[2] = { 0.0f, 3.4f };
    static const uint8_t col[3][3] = { {255, 140, 0}, {90, 255, 40}, {190, 60, 255} };
    for (int e = 0; e < 2; e++) {
        float tt = t + off[e];
        int n = (int)(tt / per[e]);
        float u = tt - (float)n * per[e];
        int r = 1 + (int)(hashf(n * 7 + e * 31) * 4.0f);
        const uint8_t *cc = col[(int)(hashf(n * 11 + e * 53) * 3.0f) % 3];
        float v = fminf(clamp01((u - 0.5f) / 1.2f), clamp01((per[e] - 1.0f - u) / 1.2f));
        float bt = u - per[e] * 0.5f;                               // one lazy blink mid-way
        if (bt > 0 && bt < 0.6f) v *= fabsf(bt - 0.3f) / 0.3f;
        float glance = 0.5f + 0.5f * sinf(u * 1.3f + e);             // looking left / right
        for (int i = 0; i < 2; i++) {
            int k = i ? NLED - r : r;
            float g = 0.7f + 0.3f * (i ? glance : 1 - glance);
            for (int j = 0; j < 3; j++) c[k][j] = fmaxf(c[k][j], cc[j] * v * g);
        }
    }
}

// Cauldron: a purple glow at the base, green bubbles rising up the sides and popping.
static void sc_cauldron(float t, float dt, float c[NLED][3]) {
    static float by[4] = { 2, 2, 2, 2 }; static int bside[4];
    for (int i = 0; i < 4; i++) {
        if (by[i] > 1.5f && frand() < dt * 0.3f) { by[i] = 1.0f; bside[i] = frand() < 0.5f; }
        else if (by[i] <= 1.5f) by[i] -= dt * 0.38f;
        if (by[i] < -1.15f) by[i] = 2;
    }
    for (int k = 0; k < NLED; k++) {
        float base = clamp01((py[k] + 0.2f) / 1.05f) * (0.55f + 0.25f * noise1(k + 520, t * 0.8f));
        float r = 90 * base, g = 0, b = 140 * base;
        for (int i = 0; i < 4; i++) {
            if (by[i] > 1.5f) continue;
            bool side = k == 0 || (bside[i] ? k <= 4 : k > 4);
            if (!side) continue;
            float d = py[k] - by[i];
            float v = expf(-d * d / 0.03f);
            float pop = by[i] < -0.95f ? 1.6f : 1.0f;              // brighter as it pops
            r += 110 * v * pop; g += 255 * v * pop; b += 30 * v * pop;
        }
        set(c[k], fminf(r, 255), fminf(g, 255), fminf(b, 255));
    }
}

// Graveyard mist: cold violet fog rolling slowly, with a pale green spirit drifting through.
static void sc_graveyard_mist(float t, float dt, float c[NLED][3]) {
    (void)dt;
    float wisp = fmodf(NLED * 3.0f * noise1(900, t * 0.06f) + t * 0.15f, NLED);
    for (int k = 0; k < NLED; k++) {
        float m = 0.25f + 0.55f * noise1(k + 540, t * 0.25f + px[k] * 0.4f);
        float d = fabsf(wisp - k); d = fminf(d, NLED - d);
        float w = expf(-d * d * 0.8f) * (0.6f + 0.4f * sinf(t * 0.5f));
        set(c[k], 55 * m + 70 * w, 35 * m + 255 * w, 100 * m + 150 * w);
    }
}

// Embers: a dying fire - deep red coals breathing slowly, now and then one flares up.
static void sc_embers(float t, float dt, float c[NLED][3]) {
    static float flare[NLED];
    for (int k = 0; k < NLED; k++) {
        flare[k] *= expf(-dt * 0.9f);
        if (frand() < dt * 0.12f) flare[k] = 0.6f + 0.4f * frand();
        float h = 0.22f + 0.2f * noise1(k + 600, t * 0.5f) + 0.06f * noise1(k + 620, t * 2.5f);
        heat(c[k], fminf(h + 0.45f * flare[k], 1.0f));
    }
}

// Will-o'-the-wisp: one soft green-white light wandering the hull, fading in and out.
static void sc_will_o_wisp(float t, float dt, float c[NLED][3]) {
    (void)dt;
    float pos = fmodf(4.5f + 4.0f * sinf(t * 0.19f) + 2.0f * sinf(t * 0.47f + 1.0f) + NLED * 2, NLED);
    float glow = 0.35f + 0.65f * powf(0.5f + 0.5f * sinf(t * 0.33f), 1.5f);
    for (int k = 0; k < NLED; k++) {
        float d = fabsf(pos - k); d = fminf(d, NLED - d);
        float v = expf(-d * d * 0.55f) * glow;
        float bg = 0.5f + 0.5f * noise1(k + 640, t * 0.3f);
        set(c[k], 2 + 150 * v, 6 * bg + 255 * v, 10 * bg + 190 * v);
    }
}

// Candy corn: yellow, orange and white bands rolling slowly down the rocket.
static void sc_candy_corn(float t, float dt, float c[NLED][3]) {
    (void)dt;
    static const float band[3][3] = { {255, 200, 0}, {255, 85, 0}, {255, 235, 210} };
    for (int k = 0; k < NLED; k++) {
        float u = fract(row[k] * 0.2f - t * 0.07f) * 3.0f;
        int i = (int)u; float a = u - (float)i;
        a = a * a * (3 - 2 * a);
        const float *p = band[i % 3], *q = band[(i + 1) % 3];
        set(c[k], p[0] + (q[0] - p[0]) * a, p[1] + (q[1] - p[1]) * a, p[2] + (q[2] - p[2]) * a);
    }
}

// Spider drop: a spider lowers itself from the nose on a silk thread, sways, climbs back up.
static void sc_spider_drop(float t, float dt, float c[NLED][3]) {
    (void)dt;
    const float cycle = 13.0f;
    int n = (int)(t / cycle);
    float u = t - (float)n * cycle, y;
    bool right = hashf(n * 17 + 3) < 0.5f;
    if (u < 4.5f)       { float a = u / 4.5f; y = -1.0f + 1.85f * a * a * (3 - 2 * a); }   // down
    else if (u < 7.0f)  y = 0.85f + 0.06f * sinf((u - 4.5f) * 5.0f);                         // sway
    else if (u < 10.5f) { float a = (u - 7.0f) / 3.5f; y = 0.85f - 1.85f * a * a * (3 - 2 * a); } // up
    else                y = -1.2f;                                                            // hidden
    for (int k = 0; k < NLED; k++) {
        float shimmer = 0.6f + 0.4f * noise1(k + 660, t * 0.4f);
        set(c[k], 18 * shimmer, 0, 30 * shimmer);
        bool side = k == 0 || (right ? k <= 4 : k > 4);
        if (!side) continue;
        if (py[k] < y - 0.1f && y > -1.0f) { c[k][0] += 45; c[k][1] += 45; c[k][2] += 55; }  // silk
        float d = py[k] - y;
        float v = expf(-d * d / 0.04f);
        c[k][0] = fminf(255, c[k][0] + 255 * v); c[k][1] = fminf(255, c[k][1] + 40 * v); c[k][2] = fminf(255, c[k][2] + 40 * v);
    }
}

typedef struct { const char *name; scene_fn fn; } builtin_t;
static const builtin_t builtins[] = {
    { "RAINBOW",     sc_rainbow },
    { "COMET",       sc_comet },
    { "THRUSTERS",   sc_thrusters },
    { "FIRE",        sc_fire },
    { "PLASMA",      sc_plasma },
    { "BREATHE",     sc_breathe },
    { "SCANNER",     sc_scanner },
    { "HYPERSPACE",  sc_hyperspace },
    { "STARFIELD",   sc_starfield },
    { "RED ALERT",   sc_red_alert },
    { "HEARTBEAT",   sc_heartbeat },
    { "AURORA",      sc_aurora },
    { "CREW SWEEP",  sc_crew },
    { "ION STORM",   sc_ion_storm },
    // space horror
    { "MOTION TRACKER", sc_motion_tracker },
    { "HULL BREACH",    sc_hull_breach },
    { "DERELICT",       sc_derelict },
    { "POSSESSED",      sc_possessed },
    { "PUMPKIN NEBULA", sc_pumpkin_nebula },
    { "GHOST SHIP",     sc_ghost_ship },
    { "BLOOD MOON",     sc_blood_moon },
    { "INFECTION",      sc_infection },
    { "SHADOW EYES",    sc_shadow_eyes },
    // more space
    { "PULSAR",         sc_pulsar },
    { "SOLAR FLARE",    sc_solar_flare },
    { "ORBIT",          sc_orbit },
    { "DOCKING LIGHTS", sc_docking },
    // slow Halloween ambience
    { "CANDLELIGHT",    sc_candlelight },
    { "JACK-O-LANTERN", sc_jack_o_lantern },
    { "LURKING EYES",   sc_lurking_eyes },
    { "CAULDRON",       sc_cauldron },
    { "GRAVEYARD MIST", sc_graveyard_mist },
    { "EMBERS",         sc_embers },
    { "WILL-O-WISP",    sc_will_o_wisp },
    { "CANDY CORN",     sc_candy_corn },
    { "SPIDER DROP",    sc_spider_drop },
};
#define NBUILTIN ((int)(sizeof builtins / sizeof builtins[0]))

// ------------------------------------------------------------------ custom (file) scenes
typedef struct { uint8_t c[NLED][3]; uint16_t ms; } frame_t;
static char    custom_names[MAX_CUSTOM][SCENE_NAME_LEN];
static char    custom_files[MAX_CUSTOM][64];
static int     ncustom;
static frame_t frames[MAX_FRAMES];
static int     nframes;
static uint32_t total_ms;
static uint16_t fade_ms;

static int hexv(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = tolower(ch);
    return (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : -1;
}

// "FF8800", "#ff8800", "F80", "-" (off). Returns false if it isn't a colour.
static bool parse_colour(const char *s, uint8_t out[3]) {
    if (s[0] == '-' && !s[1]) { out[0] = out[1] = out[2] = 0; return true; }
    if (*s == '#') s++;
    size_t n = strlen(s);
    if (n != 6 && n != 3) return false;
    for (size_t i = 0; i < n; i++) if (hexv(s[i]) < 0) return false;
    for (int j = 0; j < 3; j++)
        out[j] = n == 6 ? (uint8_t)(hexv(s[j * 2]) * 16 + hexv(s[j * 2 + 1])) : (uint8_t)(hexv(s[j]) * 17);
    return true;
}

static bool streq_ci(const char *a, const char *b) {
    while (*a && *b) if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return false;
    return *a == *b;
}

// One line of a scene file. Returns false and fills err on a bad line.
static uint16_t cur_step_ms;
static bool parse_line(char *line, int lineno, char *err, size_t errlen) {
    // '#' starts a comment, unless it's a "#RRGGBB" / "#RGB" colour token
    for (char *p = line; *p; p++) {
        if (*p != '#') continue;
        bool at_token = p == line || isspace((unsigned char)p[-1]) || p[-1] == ',';
        int n = 0;
        while (hexv((unsigned char)p[1 + n]) >= 0) n++;
        char after = p[1 + n];
        bool colour = at_token && (n == 3 || n == 6) && (!after || isspace((unsigned char)after) || after == ',');
        if (!colour) { *p = 0; break; }
    }
    char *tok[16]; int nt = 0;
    for (char *p = strtok(line, " \t\r\n,"); p && nt < 16; p = strtok(NULL, " \t\r\n,")) tok[nt++] = p;
    if (!nt) return true;

    if (streq_ci(tok[0], "step") || streq_ci(tok[0], "fade")) {
        if (nt < 2) { snprintf(err, errlen, "LINE %d: NEEDS MS", lineno); return false; }
        int v = atoi(tok[1]);
        if (v < 0 || v > 60000) { snprintf(err, errlen, "LINE %d: BAD MS", lineno); return false; }
        if (tolower((unsigned char)tok[0][0]) == 's') cur_step_ms = (uint16_t)(v < 10 ? 10 : v);
        else fade_ms = (uint16_t)v;
        return true;
    }

    uint8_t cols[NLED][3]; int nc = 0;
    frame_t f = { .ms = cur_step_ms };
    for (int i = 0; i < nt; i++) {
        if (tok[i][0] == '@') {
            int v = atoi(tok[i] + 1);
            if (v < 10 || v > 60000) { snprintf(err, errlen, "LINE %d: BAD @MS", lineno); return false; }
            f.ms = (uint16_t)v;
        } else if (streq_ci(tok[i], "C")) {
            // old clock marker: ignored, the minibadge clock is left to the OS
        } else if (nc < NLED && parse_colour(tok[i], cols[nc])) {
            nc++;
        } else {
            snprintf(err, errlen, "LINE %d: ?? %.6s", lineno, tok[i]);
            return false;
        }
    }
    if (nc != 1 && nc != 5 && nc != NLED) {
        snprintf(err, errlen, "LINE %d: %d COLOURS", lineno, nc);
        return false;
    }
    if (nframes >= MAX_FRAMES) { snprintf(err, errlen, "MAX %d FRAMES", MAX_FRAMES); return false; }
    for (int k = 0; k < NLED; k++) {
        int src = nc == 1 ? 0
                : nc == NLED ? k
                : (k <= 4 ? k : NLED - k);       // 5 colours: nose + right side, mirrored to the left
        memcpy(f.c[k], cols[src], 3);
    }
    frames[nframes++] = f;
    total_ms += f.ms;
    return true;
}

static bool load_custom(int ci, char *err, size_t errlen) {
    nframes = 0; total_ms = 0; fade_ms = 0; cur_step_ms = 200;
    FIL fp;
    if (f_open(&fp, custom_files[ci], FA_READ) != FR_OK) { snprintf(err, errlen, "CAN'T OPEN FILE"); return false; }
    static char buf[512];
    char line[200]; int ln = 0, lineno = 1;
    bool ok = true, overlong = false;
    UINT br;
    while (ok && f_read(&fp, buf, sizeof buf, &br) == FR_OK && br > 0) {
        for (UINT i = 0; i < br && ok; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                line[ln] = 0;
                if (overlong) { snprintf(err, errlen, "LINE %d: TOO LONG", lineno); ok = false; }
                else ok = parse_line(line, lineno, err, errlen);
                ln = 0; overlong = false; lineno++;
            } else if (ln < (int)sizeof line - 1) line[ln++] = ch;
            else overlong = true;
        }
    }
    if (ok && ln) { line[ln] = 0; ok = parse_line(line, lineno, err, errlen); }
    f_close(&fp);
    if (ok && !nframes) { snprintf(err, errlen, "NO FRAMES IN FILE"); ok = false; }
    if (!ok) nframes = 0;
    return ok;
}

static int cur_scene;

// The LED timer interrupt (ambient.c) renders scenes; while the main loop changes scene
// state it holds this, and the interrupt just skips that tick. Both run on core 0.
static volatile int busy;
void scenes_lock(void)   { busy++; __asm volatile("" ::: "memory"); }
void scenes_unlock(void) { __asm volatile("" ::: "memory"); busy--; }
bool scenes_busy(void)   { return busy != 0; }
static void scan_dir(void);
static void scenes_scan_locked(void);
void scenes_scan(void) { scenes_lock(); scenes_scan_locked(); scenes_unlock(); }
static void scenes_scan_locked(void) {
    char was[SCENE_NAME_LEN] = "";
    bool was_custom = cur_scene >= NBUILTIN;
    if (was_custom) snprintf(was, sizeof was, "%s", custom_names[cur_scene - NBUILTIN]);
    scan_dir();
    if (!was_custom) return;
    for (int i = 0; i < ncustom; i++)
        if (strcmp(custom_names[i], was) == 0) {
            char e[40];
            if (load_custom(i, e, sizeof e)) { cur_scene = NBUILTIN + i; return; }
            break;
        }
    cur_scene = 0;                                // file gone or broken now: back to the first scene
}

static void scan_dir(void) {
    ncustom = 0;
    DIR dir; FILINFO fi;
    if (f_opendir(&dir, SCENE_DIR) != FR_OK) return;
    while (ncustom < MAX_CUSTOM && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        size_t n = strlen(fi.fname);
        if ((fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) || fi.fname[0] == '.' || n < 5) continue;
        if (!streq_ci(fi.fname + n - 4, ".txt")) continue;
        if (n > sizeof custom_files[0] - sizeof SCENE_DIR - 1) continue;   // name too long for a path
        char *nm = custom_names[ncustom];
        snprintf(nm, SCENE_NAME_LEN, "%.*s", (int)(n - 4 > SCENE_NAME_LEN - 1 ? SCENE_NAME_LEN - 1 : n - 4), fi.fname);
        for (char *p = nm; *p; p++) *p = (char)(*p == '_' ? ' ' : toupper((unsigned char)*p));
        snprintf(custom_files[ncustom], sizeof custom_files[0], SCENE_DIR "/%s", fi.fname);
        ncustom++;
    }
    f_closedir(&dir);
    // keep them in a stable, alphabetical order
    for (int i = 1; i < ncustom; i++)
        for (int j = i; j > 0 && strcmp(custom_names[j - 1], custom_names[j]) > 0; j--) {
            char tn[SCENE_NAME_LEN], tf[64];
            memcpy(tn, custom_names[j], sizeof tn); memcpy(custom_names[j], custom_names[j - 1], sizeof tn); memcpy(custom_names[j - 1], tn, sizeof tn);
            memcpy(tf, custom_files[j], sizeof tf); memcpy(custom_files[j], custom_files[j - 1], sizeof tf); memcpy(custom_files[j - 1], tf, sizeof tf);
        }
}

static const char example_txt[] =
    "# SC26 LED scene file. Lines starting with # are comments.\r\n"
    "# Each frame is one line of colours (RRGGBB hex, RGB short hex, or - for off):\r\n"
    "#   9 colours = every LED, clockwise from the nose: nose, right side top->fin,\r\n"
    "#               left fin, left side fin->top\r\n"
    "#   5 colours = nose + right side top->fin, mirrored onto the left side\r\n"
    "#   1 colour  = all LEDs\r\n"
    "# Extra on a frame line:  @300 = show this frame for 300 ms\r\n"
    "# Settings (apply to the frames after them):\r\n"
    "#   step 150   default ms per frame       fade 80   cross-fade ms into each frame\r\n"
    "# Save the file in the LEDS folder, then Menu -> LED SCENES and pick it.\r\n"
    "\r\n"
    "step 140\r\n"
    "fade 90\r\n"
    "# warp core charging up, nose to fins (5 colours = mirrored)\r\n"
    "00FFFF -      -      -      -\r\n"
    "0088AA 00FFFF -      -      -\r\n"
    "004455 0088AA 00FFFF -      -\r\n"
    "-      004455 0088AA 00FFFF -\r\n"
    "-      -      004455 0088AA 00FFFF\r\n"
    "-      -      -      004455 FFFFFF @220\r\n"
    "# full flash, then dark\r\n"
    "FFFFFF @80\r\n"
    "- @400\r\n";

static const char haunted_txt[] =
    "# HAUNTED - pumpkin glow with ghostly purple drifting up the sides\r\n"
    "# 5 colours per line = nose + right side (top -> fin), mirrored onto the left side\r\n"
    "step 160\r\n"
    "fade 140\r\n"
    "#  nose    side 1   side 2   side 3   fin\r\n"
    "   200040  -        -        FF5000   FF7A00\r\n"
    "   400080  200040   -        FF5000   FF6000\r\n"
    "   -       400080   200040   FF4000   FF7A00\r\n"
    "   -       -        400080   803000   FF5000\r\n"
    "   -       -        200040   FF5000   FF7A00\r\n"
    "   200040  -        -        FF6000   FF5000   @300\r\n"
    "# a sudden flicker, like a candle guttering\r\n"
    "   -       -        -        401800   601C00   @60\r\n"
    "   200040  -        -        FF6000   FF7A00\r\n";

static bool write_file(const char *path, const char *text) {
    FIL fp; UINT bw;
    if (f_open(&fp, path, FA_CREATE_NEW | FA_WRITE) != FR_OK) return false;
    bool ok = f_write(&fp, text, (UINT)strlen(text), &bw) == FR_OK && bw == strlen(text);
    f_close(&fp);
    return ok;
}

// An earlier build of this mod wrote LEDS/POLICE.TXT as an example. Remove it, but only if
// it's still that generated file (recognised by its first line), never a file of the user's.
static bool remove_old_police(void) {
    static const char sig[] = "# Red / blue strobe, left side vs right side";
    FIL fp; UINT br; char buf[sizeof sig];
    if (f_open(&fp, SCENE_DIR "/POLICE.TXT", FA_READ) != FR_OK) return false;
    bool ours = f_read(&fp, buf, sizeof sig - 1, &br) == FR_OK && br == sizeof sig - 1 &&
                memcmp(buf, sig, sizeof sig - 1) == 0;
    f_close(&fp);
    return ours && f_unlink(SCENE_DIR "/POLICE.TXT") == FR_OK;
}

bool scenes_make_example(void) {
    FILINFO fi;
    bool changed = remove_old_police();
    if (f_stat(SCENE_DIR, &fi) == FR_OK) return changed;   // folder exists: leave the rest alone
    if (f_mkdir(SCENE_DIR) != FR_OK) return changed;
    write_file(SCENE_DIR "/WARP_CORE.TXT", example_txt);
    write_file(SCENE_DIR "/HAUNTED.TXT", haunted_txt);
    return true;
}

// ------------------------------------------------------------------ state + API
static const uint16_t speeds[] = { 25, 50, 75, 100, 150, 200, 300, 400 };
#define NSPEED ((int)(sizeof speeds / sizeof speeds[0]))
static int        speed_idx = 3;                 // 100 %
static uint8_t    bright = 40;
static float      scene_t;
static uint32_t   last_ms;
static bool       have_last;


void scenes_init(void) { cur_scene = 0; scene_t = 0; have_last = false; }
int  scenes_builtin_count(void) { return NBUILTIN; }
int  scenes_count(void) { return NBUILTIN + ncustom; }
bool scenes_is_custom(int i) { return i >= NBUILTIN; }
const char *scenes_name(int i) {
    if (i < 0 || i >= scenes_count()) return "?";
    return i < NBUILTIN ? builtins[i].name : custom_names[i - NBUILTIN];
}
int scenes_find(const char *name) {
    for (int i = 0; i < scenes_count(); i++) if (strcmp(scenes_name(i), name) == 0) return i;
    return -1;
}
int scenes_current(void) { return cur_scene; }

bool scenes_select(int i, char *err, size_t errlen) {
    if (i < 0 || i >= scenes_count()) i = 0;
    scenes_lock();
    bool ok = i < NBUILTIN || load_custom(i - NBUILTIN, err, errlen);
    if (!ok && cur_scene >= NBUILTIN) {          // a failed load clobbered the running file scene
        char e[40];
        if (!load_custom(cur_scene - NBUILTIN, e, sizeof e)) cur_scene = 0;
    }
    if (ok) { cur_scene = i; scene_t = 0; memset(star, 0, sizeof star); }
    scenes_unlock();
    return ok;
}

// ------------------------------------------------------------------ recording a loop for DOOM
// DOOM can't run the scene engine (that build has no floating point), so before it starts
// SC26 OS records the running scene as a short loop of finished frames: u16 interval_ms,
// u16 n, then n x 27 bytes (physical LED order, gamma and brightness applied). A file scene
// is recorded for exactly one cycle; a built-in scene for as long as fits, with its last
// frames cross-faded into the first so the loop has no visible seam. It changes the scene's
// state, so it's only used on the way out to DOOM.
#define REC_FRAME (NLED * 3)
#define REC_XFADE 12
size_t scenes_record_loop(uint8_t *buf, size_t max, uint32_t start_ms) {
    if (max < 4 + 2 * REC_FRAME) return 0;
    int cap = (int)((max - 4) / REC_FRAME);
    uint32_t iv = 66;                            // ~15 frames a second; DOOM blends between them
    int n = cap;
    bool file = cur_scene >= NBUILTIN && nframes && total_ms;
    if (file) {                                  // one whole cycle at the chosen speed
        uint32_t loop_ms = total_ms * 100u / speeds[speed_idx];
        iv = (loop_ms + (uint32_t)cap - 1) / (uint32_t)cap;
        if (iv < 30) iv = 30;
        n = (int)((loop_ms + iv / 2) / iv);
        if (n < 2) n = 2;
        if (n > cap) n = cap;
    }
    scene_t = 0;
    have_last = false;
    scene_out_t o;
    static uint8_t tail[REC_XFADE][REC_FRAME];
    for (int k = 0; k < n + (file ? 0 : REC_XFADE); k++) {
        scenes_render(start_ms + (uint32_t)k * iv, &o);
        memcpy(k < n ? buf + 4 + k * REC_FRAME : tail[k - n], o.rgb, REC_FRAME);
    }
    if (!file) {                                 // blend the run-on into the start: seamless
        for (int k = 0; k < REC_XFADE; k++)
            for (int j = 0; j < REC_FRAME; j++) {
                uint8_t *p = buf + 4 + k * REC_FRAME + j;
                *p = (uint8_t)((tail[k][j] * (REC_XFADE - k) + *p * k) / REC_XFADE);
            }
    }
    buf[0] = (uint8_t)iv; buf[1] = (uint8_t)(iv >> 8);
    buf[2] = (uint8_t)n;  buf[3] = (uint8_t)(n >> 8);
    return 4 + (size_t)n * REC_FRAME;
}

int  scenes_speed_count(void) { return NSPEED; }
int  scenes_speed_pct(int idx) { return speeds[idx < 0 ? 0 : idx >= NSPEED ? NSPEED - 1 : idx]; }
int  scenes_speed_idx(void) { return speed_idx; }
void scenes_set_speed_idx(int idx) { speed_idx = idx < 0 ? 0 : idx >= NSPEED ? NSPEED - 1 : idx; }
uint8_t scenes_brightness(void) { return bright; }
void scenes_set_brightness(uint8_t pct) { bright = pct < 5 ? 5 : pct > 100 ? 100 : pct; }

static void render_custom(float t, float c[NLED][3]) {
    if (!nframes || !total_ms) { for (int k = 0; k < NLED; k++) set(c[k], 0, 0, 0); return; }
    uint32_t pos = (uint32_t)fmodf(t * 1000.0f, (float)total_ms);
    int i = 0;
    while (i < nframes - 1 && pos >= frames[i].ms) { pos -= frames[i].ms; i++; }
    const frame_t *f = &frames[i], *prev = &frames[(i + nframes - 1) % nframes];
    float a = fade_ms ? clamp01((float)pos / (float)fade_ms) : 1.0f;
    for (int k = 0; k < NLED; k++)
        for (int j = 0; j < 3; j++) c[k][j] = prev->c[k][j] + (f->c[k][j] - prev->c[k][j]) * a;
}

void scenes_led_pos(int led, float *x, float *y) {
    const uint8_t *map = ambient_clockwise() ? ring_right : ring_left;
    for (int k = 0; k < NLED; k++) if (map[k] == led) { *x = px[k]; *y = py[k]; return; }
    *x = *y = 0;
}

void scenes_render(uint32_t now_ms, scene_out_t *out) {
    float dt = have_last ? (float)(now_ms - last_ms) / 1000.0f : 0.0f;
    if (dt > 0.2f) dt = 0.2f;                   // after a pause, don't jump
    last_ms = now_ms; have_last = true;
    dt *= (float)speeds[speed_idx] / 100.0f;
    scene_t += dt;
    if (scene_t > 3600.0f * 4) scene_t -= 3600.0f * 4;   // keep float precision (4 h wrap)

    float c[NLED][3];
    if (cur_scene < NBUILTIN) builtins[cur_scene].fn(scene_t, dt, c);
    else render_custom(scene_t, c);

    const uint8_t *map = ambient_clockwise() ? ring_right : ring_left;
    float b = (float)bright / 100.0f;
    for (int k = 0; k < NLED; k++)
        for (int j = 0; j < 3; j++) {
            float v = clamp01(c[k][j] / 255.0f);
            out->rgb[map[k]][j] = (uint8_t)(powf(v, 2.2f) * b * 255.0f + 0.5f);
            out->view[map[k]][j] = (uint8_t)(v * 255.0f + 0.5f);
        }

}
