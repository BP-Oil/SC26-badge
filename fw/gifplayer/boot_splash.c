// SC26 power-up: BIOS POST console + animated splash. See boot_splash.h.
//
// The splash is rendered live into the shared 360x360 framebuffer (gp_fb) every frame:
// starfield -> shooting star -> astronauts -> title -> loader bar, then one DMA blit.
// Sprites (tools/make_boot_assets.py) are 8-bit indexed with premultiplied RGBA palettes;
// draw_sprite() rotates/scales them with bilinear filtering and alpha-blends over the
// background, so the art stays smooth while it drifts and tilts.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "lcd.h"
#include "leds.h"
#include "buttons.h"
#include "ambient.h"
#include "gif_player.h"
#include "boot_splash.h"
#include "boot_assets.h"

#define W LCD_WIDTH
#define H LCD_HEIGHT

// ------------------------------------------------------------------ skip handling
static bool skip;
static uint64_t boot_t0;
static bool poll_skip(void) {
    if (!skip && buttons_read() && time_us_64() - boot_t0 > 150000) skip = true;
    return skip;
}
bool boot_skipped(void) { return skip; }

static void pause_ms(int ms, boot_service_fn svc) {
    uint64_t end = time_us_64() + (uint64_t)ms * 1000;
    while (time_us_64() < end && !poll_skip()) { if (svc) svc(); sleep_us(500); }
}

// ------------------------------------------------------------------ BIOS console
#define BIOS_X   58
#define BIOS_Y0  78
#define BIOS_DY  19
#define BIOS_FG  RGB565(200, 200, 200)
#define BIOS_BG  RGB565(0, 0, 0)
static int bios_line;

static void cursor(bool on) {
    lcd_fill_rect(BIOS_X, BIOS_Y0 + bios_line * BIOS_DY + 2, 9, 15, on ? BIOS_FG : BIOS_BG);
}

void bios_print(const char *s, int ms) {
    cursor(false);
    if (*s) lcd_draw_text(BIOS_X, BIOS_Y0 + bios_line * BIOS_DY, s, BIOS_FG, BIOS_BG, 1);
    bios_line++;
    cursor(true);
    pause_ms(ms, NULL);
}

void bios_printf_pause(int ms, const char *fmt, ...) {
    char buf[40];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    bios_print(buf, ms);
}

void bios_wait(int ms, boot_service_fn svc) {
    uint64_t end = time_us_64() + (uint64_t)ms * 1000;
    while (time_us_64() < end && !poll_skip()) {
        cursor(((time_us_64() / 250000) & 1) == 0);
        pause_ms(20, svc);
    }
}

void bios_begin(void) {
    boot_t0 = time_us_64();
    skip = false;
    bios_line = 0;
    lcd_blit_wait();
    lcd_fill_rect(0, 0, W, H, BIOS_BG);
    // POST "beep": the LED ring flashes once, like a PC's power-on self test
    leds_fill(28, 28, 28); leds_show(); status_led(true);
    sleep_ms(140);
    leds_fill(0, 0, 0); leds_show(); status_led(false);

    bios_print("SC26 v2.1 by BP  (c) 2026", 140);
    bios_print("", 0);
    bios_printf_pause(140, "CPU : RP2350 @ %lu MHz", (unsigned long)(clock_get_hz(clk_sys) / 1000000));
    // memory count-up
    int y = BIOS_Y0 + bios_line * BIOS_DY;
    for (int kb = 0; kb <= 520 && !poll_skip(); kb += 20) {
        char s[24]; snprintf(s, sizeof s, "MEM : %3d KB", kb);
        lcd_draw_text(BIOS_X, y, s, BIOS_FG, BIOS_BG, 1);
        sleep_ms(18);
    }
    lcd_draw_text(BIOS_X, y, "MEM : 520 KB  OK", BIOS_FG, BIOS_BG, 1);
    bios_line++;
    bios_print("ROM : 16 MB QSPI FLASH", 160);
    bios_print("", 0);
}

// ------------------------------------------------------------------ splash rendering
static inline uint16_t be(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static void draw_sprite(const boot_sprite_t *s, float cx, float cy, float ang_deg, float scale, int fade) {
    if (fade <= 0) return;
    if (fade > 256) fade = 256;
    const float a = ang_deg * (float)M_PI / 180.0f;
    const float c = cosf(a), sn = sinf(a);
    const float ca = c / scale, sa = sn / scale;
    const int sw = s->w, sh = s->h;
    const float hw = sw * 0.5f * scale, hh = sh * 0.5f * scale;
    const float ex = fabsf(c) * hw + fabsf(sn) * hh, ey = fabsf(sn) * hw + fabsf(c) * hh;
    int x0 = (int)floorf(cx - ex) - 1, x1 = (int)ceilf(cx + ex) + 1;
    int y0 = (int)floorf(cy - ey) - 1, y1 = (int)ceilf(cy + ey) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1) return;
    const int32_t du = (int32_t)(ca * 65536.0f), dv = (int32_t)(-sa * 65536.0f);
    const uint8_t *px = s->px;
    const uint32_t *prb = s->rb, *pag = s->ag;
    for (int y = y0; y < y1; y++) {
        const float dy = (float)y + 0.5f - cy, dx = (float)x0 + 0.5f - cx;
        int32_t u = (int32_t)((dx * ca + dy * sa + sw * 0.5f - 0.5f) * 65536.0f);
        int32_t v = (int32_t)((-dx * sa + dy * ca + sh * 0.5f - 0.5f) * 65536.0f);
        uint16_t *row = &gp_fb[y * W];
        for (int x = x0; x < x1; x++, u += du, v += dv) {
            const int iu = u >> 16, iv = v >> 16;
            if ((unsigned)iu >= (unsigned)(sw - 1) || (unsigned)iv >= (unsigned)(sh - 1)) continue;
            const uint8_t *p = px + iv * sw + iu;
            const uint32_t i0 = p[0], i1 = p[1], i2 = p[sw], i3 = p[sw + 1];
            if ((i0 | i1 | i2 | i3) == 0) continue;
            uint32_t rb, ag;
            if (i0 == i1 && i0 == i2 && i0 == i3) {
                rb = prb[i0]; ag = pag[i0];
            } else {
                const uint32_t fu = ((uint32_t)u >> 8) & 0xFF, fv = ((uint32_t)v >> 8) & 0xFF;
                const uint32_t w3 = (fu * fv) >> 8, w2 = fv - w3, w1 = fu - w3, w0 = 256 - fu - fv + w3;
                rb = ((prb[i0] * w0 + prb[i1] * w1 + prb[i2] * w2 + prb[i3] * w3) >> 8) & 0x00FF00FFu;
                ag = ((pag[i0] * w0 + pag[i1] * w1 + pag[i2] * w2 + pag[i3] * w3) >> 8) & 0x00FF00FFu;
            }
            if (fade < 256) { rb = ((rb * (uint32_t)fade) >> 8) & 0x00FF00FFu; ag = ((ag * (uint32_t)fade) >> 8) & 0x00FF00FFu; }
            const uint32_t A = ag >> 16;
            if (A == 0) continue;
            uint32_t r = rb >> 16, g = ag & 0xFF, b = rb & 0xFF;
            if (A < 255) {
                const uint32_t inv = 255 - A;
                const uint16_t bg = be(row[x]);
                r += ((((uint32_t)bg >> 11) & 31) * 8 * inv) >> 8;
                g += ((((uint32_t)bg >> 5) & 63) * 4 * inv) >> 8;
                b += (((uint32_t)bg & 31) * 8 * inv) >> 8;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
            }
            row[x] = be((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)));
        }
    }
}

static inline void put_gray(int x, int y, int v) {
    if ((unsigned)x >= W || (unsigned)y >= H || v <= 0) return;
    if (v > 255) v = 255;
    uint16_t *d = &gp_fb[y * W + x];
    uint16_t old = be(*d);
    int ov = ((old >> 5) & 63) * 4;           // stars are grey: compare on green
    if (v <= ov) return;
    *d = be(RGB565(v, v, v));
}

#define NSTARS 90
typedef struct { float x, y, ph, sp; uint8_t br, big; } star_t;
static star_t stars[NSTARS];

static uint32_t rng = 26;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 16777216.0f; }

static void stars_init(void) {
    int n = 0;
    while (n < NSTARS) {
        float x = frand() * W, y = frand() * H;
        if ((x - 180) * (x - 180) + (y - 180) * (y - 180) >= 178.0f * 178.0f) continue;
        stars[n] = (star_t){ x, y, frand() * 6.28f, 1.5f + frand() * 2.5f, (uint8_t)(70 + frand() * 185), frand() < 0.12f };
        n++;
    }
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float ease_out(float x) { x = clamp01(x); float k = 1 - x; return 1 - k * k * k; }
static float ease_back(float x) {
    x = clamp01(x);
    const float c1 = 1.4f, c3 = c1 + 1, k = x - 1;
    return 1 + c3 * k * k * k + c1 * k * k;
}
#define TAU 6.2831853f

// loader-bar geometry (an homage to the classic marching-blocks boot bar)
#define BAR_X0 126
#define BAR_X1 234
#define BAR_Y0 298
#define BAR_Y1 311
#define CHUNK_W 8
#define CHUNK_STEP 11
#define BAR_SPEED 62.0f       // px per second

static void draw_bar(float t, float vis) {
    if (vis <= 0) return;
    int g = (int)(150 * vis);
    uint16_t edge = be(RGB565(g, g, g));
    for (int x = BAR_X0 + 3; x <= BAR_X1 - 3; x++) { gp_fb[BAR_Y0 * W + x] = edge; gp_fb[BAR_Y1 * W + x] = edge; }
    for (int y = BAR_Y0 + 3; y <= BAR_Y1 - 3; y++) { gp_fb[y * W + BAR_X0] = edge; gp_fb[y * W + BAR_X1] = edge; }
    // rounded corners
    static const int8_t cx[] = { 1, 2, 1 }, cy[] = { 2, 1, 1 };
    for (int k = 0; k < 3; k++) {
        gp_fb[(BAR_Y0 + cy[k]) * W + BAR_X0 + cx[k]] = edge;  gp_fb[(BAR_Y0 + cy[k]) * W + BAR_X1 - cx[k]] = edge;
        gp_fb[(BAR_Y1 - cy[k]) * W + BAR_X0 + cx[k]] = edge;  gp_fb[(BAR_Y1 - cy[k]) * W + BAR_X1 - cx[k]] = edge;
    }
    if (t < 1.0f) return;
    const float span = (BAR_X1 - BAR_X0) + 3 * CHUNK_STEP;
    float pos = fmodf((t - 1.0f) * BAR_SPEED, span) - 3 * CHUNK_STEP;
    for (int k = 0; k < 3; k++) {
        int xa = BAR_X0 + 2 + (int)pos + k * CHUNK_STEP, xb = xa + CHUNK_W;
        if (xa < BAR_X0 + 3) xa = BAR_X0 + 3;
        if (xb > BAR_X1 - 2) xb = BAR_X1 - 2;
        for (int y = BAR_Y0 + 3; y <= BAR_Y1 - 3; y++) {
            float s = (float)(y - (BAR_Y0 + 3)) / (float)(BAR_Y1 - BAR_Y0 - 6);
            uint16_t col = be(RGB565((int)((110 - 80 * s) * vis), (int)((170 - 90 * s) * vis), (int)((255 - 60 * s) * vis)));
            for (int x = xa; x < xb; x++) gp_fb[y * W + x] = col;
        }
    }
}

// LED ring mirrors the loader: a blue comet runs clockwise from the top, one lap per pass
static void ring_leds(float t, float vis) {
    const float span = (BAR_X1 - BAR_X0) + 3 * CHUNK_STEP;
    float head = t < 1.0f ? -1000.0f : fmodf((t - 1.0f) * BAR_SPEED, span) / span * 360.0f;
    bool cw = ambient_clockwise();
    for (int k = 0; k < WS2812_COUNT; k++) {
        float d = head - k * 40.0f;
        while (d < 0) d += 360.0f;
        float in = d < 120.0f ? (1.0f - d / 120.0f) : 0.0f;
        in *= in * vis;
        int i = cw ? (7 + k) % WS2812_COUNT : (7 - k + WS2812_COUNT) % WS2812_COUNT;
        leds_set(i, (uint8_t)(14 * in), (uint8_t)(34 * in), (uint8_t)(110 * in));
    }
    leds_show();
}

static void render(float t) {
    memset(gp_fb, 0, W * H * 2);
    const float sfade = clamp01(t / 0.8f);
    for (int i = 0; i < NSTARS; i++) {
        const star_t *s = &stars[i];
        float x = fmodf(s->x - 4.0f * t + W, W);
        float tw = 0.65f + 0.35f * sinf(s->ph + t * s->sp);
        int v = (int)(s->br * tw * sfade);
        int ix = (int)x, iy = (int)s->y;
        put_gray(ix, iy, v);
        if (s->big) { put_gray(ix + 1, iy, v); put_gray(ix, iy + 1, v); put_gray(ix + 1, iy + 1, v); }
    }
    if (t >= 3.2f && t < 3.75f) {                       // shooting star
        float p = (t - 3.2f) / 0.55f, hx = 330 - 230 * p, hy = 60 + 90 * p;
        for (int k = 0; k < 28; k++) {
            float q = 1.0f - k / 28.0f;
            put_gray((int)(hx + k * 2.3f), (int)(hy - k * 0.9f), (int)(255 * q * (1 - p * 0.5f)));
        }
    }
    // floating astronaut drifts in from the top-left, tumbling to a slow sway
    float e = ease_out((t - 0.2f) / 1.6f);
    float ax = -70 + (118 + 3 * sinf(t * TAU / 5.3f) + 70) * e;
    float ay = 40 + (148 + 5 * sinf(t * TAU / 3.1f) - 40) * e;
    float aang = -50 * (1 - e) + 7 * sinf(t * TAU / 4.6f);
    draw_sprite(&spr_float, ax, ay, aang, 1.0f, (int)(256 * clamp01((t - 0.2f) / 0.4f)));
    // thumbs-up astronaut rises from below with a little overshoot and a "pop"
    float e2 = ease_back((t - 0.7f) / 1.4f);
    float by = 430 + (158 - 430) * e2 + 4 * sinf((t - 0.7f) * TAU / 2.7f) * clamp01(t - 2.1f);
    float bang = 2.5f * sinf(t * TAU / 3.3f);
    float pop = 1 + 0.035f * expf(-((t - 2.25f) / 0.12f) * ((t - 2.25f) / 0.12f));
    draw_sprite(&spr_thumbs, 242, by, bang, pop, (int)(256 * clamp01((t - 0.7f) / 0.3f)));
    // title + version line
    float tf = clamp01((t - 1.6f) / 0.8f);
    if (tf > 0) {
        draw_sprite(&spr_title, 180, 276 - 8 * ease_out(tf), 0, 1.0f, (int)(256 * tf));
        draw_sprite(&spr_version, 180, 330, 0, 1.0f, (int)(256 * tf));
    }
    draw_bar(t, clamp01((t - 0.9f) / 0.3f));
}

#define SPLASH_SECONDS 6.0f
#define FADE_SECONDS   0.35f

void boot_splash(boot_service_fn svc, uint8_t backlight) {
    stars_init();
    lcd_blit_wait();
    uint64_t t0 = time_us_64();
    float fade_at = skip ? 0.0f : -1.0f;               // already skipped during the BIOS? go straight to the fade
    while (true) {
        float t = (float)(time_us_64() - t0) / 1e6f;
        if (fade_at < 0 && (t >= SPLASH_SECONDS || (t > 0.3f && poll_skip()))) fade_at = t;
        float out = fade_at < 0 ? 0.0f : (t - fade_at) / FADE_SECONDS;
        if (out >= 1.0f) break;
        render(t);
        float vis = 1.0f - clamp01(out);
        ring_leds(t, vis);
        lcd_backlight((uint8_t)(backlight * vis));
        if (svc) svc();
        lcd_wait_te(18000);
        lcd_blit(0, 0, W, H, gp_fb);
        if (svc) svc();
    }
    lcd_blit_wait();
    lcd_fill_rect(0, 0, W, H, RGB565(0, 0, 0));
    leds_fill(0, 0, 0); leds_show();
    // don't let the key that skipped the splash also act as the first key press of the player
    uint64_t rel = time_us_64() + 2000000;
    while (buttons_read() && time_us_64() < rel) { if (svc) svc(); sleep_ms(5); }
    lcd_backlight(backlight);
}
