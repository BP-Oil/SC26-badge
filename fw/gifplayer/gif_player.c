// GIF playback into a full-screen framebuffer.
//  * AnimatedGIF (RAW mode) hands us one line of 8-bit palette indices at a time.
//  * Each source line is nearest-neighbour scaled into the 360x360 RGB565 framebuffer,
//    honouring transparency and the GIF disposal methods (1 = keep, 2 = clear to background).
//  * Files that FatFs reports as contiguous are decoded straight from memory-mapped flash.

#include <string.h>
#include <math.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "fatfs/ff.h"
#include "flash_disk.h"
#include "gif_player.h"

// AnimatedGIF's C core is compiled into this translation unit so we can use GIFInit()
#include "gif.inl"

uint16_t gp_fb[SC26_ARENA_BYTES / 2] __attribute__((aligned(8)));

static GIFIMAGE gif;
static FIL      fil;
static bool     fil_open;
static DWORD    clmt[16];
static gp_info_t info;
static zoom_t   cur_zoom;
static char     path_buf[128];
static char     err[48];
static gp_service_fn service_cb;

// scaling state (16.16 fixed point)
static uint32_t scale_fp;             // dest pixels per source pixel
static int16_t  xmap[LCD_WIDTH];      // dest x -> source canvas x (-1 = outside)
// frame bookkeeping for disposal
static bool frame_started;
static int  prev_x, prev_y, prev_w, prev_h, prev_disposal = 0;
static int  lines_since_service;

static const char *zoom_names[ZOOM_COUNT] = { "AUTO", "FIT CIRCLE", "FIT SQUARE", "FILL" };
const char *gp_zoom_name(zoom_t z) { return z < ZOOM_COUNT ? zoom_names[z] : "?"; }
const gp_info_t *gp_info(void) { return &info; }
const char *gp_error(void) { return err; }

// ---------------------------------------------------------------- file I/O callbacks
static int32_t fat_read(GIFFILE *f, uint8_t *buf, int32_t len) {
    UINT br = 0;
    f_read((FIL *)f->fHandle, buf, (UINT)len, &br);
    f->iPos = (int32_t)f_tell((FIL *)f->fHandle);
    return (int32_t)br;
}
static int32_t fat_seek(GIFFILE *f, int32_t pos) {
    f_lseek((FIL *)f->fHandle, (FSIZE_t)pos);
    f->iPos = pos;
    return pos;
}

// ---------------------------------------------------------------- geometry
static void mark_dirty(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 >= LCD_HEIGHT) y1 = LCD_HEIGHT - 1;
    if (y1 < y0) return;
    if (info.dirty_y1 < info.dirty_y0) { info.dirty_y0 = y0; info.dirty_y1 = y1; return; }
    if (y0 < info.dirty_y0) info.dirty_y0 = y0;
    if (y1 > info.dirty_y1) info.dirty_y1 = y1;
}

static inline int src_to_dest_y(int sy) { return info.dest_y + (int)(((int64_t)sy * scale_fp) >> 16); }
static inline int src_to_dest_x(int sx) { return info.dest_x + (int)(((int64_t)sx * scale_fp) >> 16); }

static void layout(void) {
    float w = (float)info.canvas_w, h = (float)info.canvas_h, s;
    zoom_t z = cur_zoom;
    if (z == ZOOM_AUTO) {   // square canvases are assumed to be made for this screen (e.g. gifprep output)
        float aspect = w / h;
        z = (aspect > 0.9f && aspect < 1.11f) ? ZOOM_SQUARE : ZOOM_CIRCLE;
    }
    switch (z) {
        case ZOOM_SQUARE: s = fminf(LCD_WIDTH / w, LCD_HEIGHT / h); break;
        case ZOOM_FILL:   s = LCD_WIDTH / fminf(w, h); break;
        default:          s = LCD_WIDTH / sqrtf(w * w + h * h); break;    // diagonal = circle diameter
    }
    scale_fp = (uint32_t)(s * 65536.0f);
    if (scale_fp == 0) scale_fp = 1;
    info.dest_w = (int)(w * s);
    info.dest_h = (int)(h * s);
    info.dest_x = (LCD_WIDTH - info.dest_w) / 2;
    info.dest_y = (LCD_HEIGHT - info.dest_h) / 2;
    for (int dx = 0; dx < LCD_WIDTH; dx++) {
        int rel = dx - info.dest_x;
        int sx = rel < 0 ? -1 : (int)(((int64_t)rel << 16) / scale_fp);
        xmap[dx] = (int16_t)((sx >= 0 && sx < info.canvas_w) ? sx : -1);
    }
    memset(gp_fb, 0, LCD_WIDTH * LCD_HEIGHT * 2);
    info.dirty_y0 = 0; info.dirty_y1 = LCD_HEIGHT - 1;
    prev_disposal = 0;
}

static void clear_canvas_rect(int x, int y, int w, int h) {
    int dy0 = src_to_dest_y(y), dy1 = src_to_dest_y(y + h);
    int dx0 = src_to_dest_x(x), dx1 = src_to_dest_x(x + w);
    if (dy0 < 0) dy0 = 0;
    if (dy1 > LCD_HEIGHT) dy1 = LCD_HEIGHT;
    if (dx0 < 0) dx0 = 0;
    if (dx1 > LCD_WIDTH) dx1 = LCD_WIDTH;
    if (dx1 <= dx0) return;
    for (int dy = dy0; dy < dy1; dy++) memset(&gp_fb[dy * LCD_WIDTH + dx0], 0, (size_t)(dx1 - dx0) * 2);
    mark_dirty(dy0, dy1 - 1);
}

// ---------------------------------------------------------------- per-line draw callback
static void draw_line(GIFDRAW *d) {
    if (!frame_started) {
        frame_started = true;
        if (prev_disposal == 2) clear_canvas_rect(prev_x, prev_y, prev_w, prev_h);
        prev_x = d->iX; prev_y = d->iY; prev_w = d->iWidth; prev_h = d->iHeight;
        prev_disposal = d->ucDisposalMethod;
    }
    int sy = d->iY + d->y;
    if (scale_fp == 65536) {                       // 1:1 fast path (360x360 GIFs, gifprep output)
        int dy = info.dest_y + sy, dx = info.dest_x + d->iX;
        if (dy >= 0 && dy < LCD_HEIGHT) {
            int i0 = dx < 0 ? -dx : 0, i1 = d->iWidth;
            if (dx + i1 > LCD_WIDTH) i1 = LCD_WIDTH - dx;
            uint16_t *row = &gp_fb[dy * LCD_WIDTH + dx];
            const uint8_t *px = d->pPixels;
            const uint16_t *pal = d->pPalette;
            if (d->ucHasTransparency) {
                const uint8_t t = d->ucTransparent;
                for (int i = i0; i < i1; i++) { uint8_t c = px[i]; if (c != t) row[i] = pal[c]; }
            } else {
                for (int i = i0; i < i1; i++) row[i] = pal[px[i]];
            }
            mark_dirty(dy, dy);
        }
        if (service_cb && ++lines_since_service >= 12) { lines_since_service = 0; service_cb(); }
        return;
    }
    int dy0 = src_to_dest_y(sy), dy1 = src_to_dest_y(sy + 1);
    if (dy0 < 0) dy0 = 0;
    if (dy1 > LCD_HEIGHT) dy1 = LCD_HEIGHT;
    if (dy1 > dy0) {
        int dx0 = src_to_dest_x(d->iX), dx1 = src_to_dest_x(d->iX + d->iWidth) + 1;
        if (dx0 < 0) dx0 = 0;
        if (dx1 > LCD_WIDTH) dx1 = LCD_WIDTH;
        const uint8_t *px = d->pPixels;
        const uint16_t *pal = d->pPalette;
        const int x_lo = d->iX, x_hi = d->iX + d->iWidth;
        const bool has_t = d->ucHasTransparency;
        const uint8_t t = d->ucTransparent;
        for (int dy = dy0; dy < dy1; dy++) {
            uint16_t *row = &gp_fb[dy * LCD_WIDTH];
            for (int dx = dx0; dx < dx1; dx++) {
                int sx = xmap[dx];
                if (sx < x_lo || sx >= x_hi) continue;
                uint8_t idx = px[sx - x_lo];
                if (has_t && idx == t) continue;
                row[dx] = pal[idx];
            }
        }
        mark_dirty(dy0, dy1 - 1);
    }
    if (service_cb && ++lines_since_service >= 12) { lines_since_service = 0; service_cb(); }
}

// ---------------------------------------------------------------- public API
static bool open_current(void) {
    memset(&info, 0, sizeof info);
    info.dirty_y1 = -1;
    if (f_open(&fil, path_buf, FA_READ) != FR_OK) { snprintf(err, sizeof err, "CAN'T OPEN FILE"); return false; }
    fil_open = true;

    GIF_begin(&gif, GIF_PALETTE_RGB565_BE);
    // The C API's GIF_begin() (unlike the C++ begin()) leaves pLineBufAligned NULL; without
    // this the decoder writes every line to address 0 (boot ROM on the RP2350 -> fault).
    uintptr_t lb = (uintptr_t)&gif.ucLineBuf[0];
    gif.pLineBufAligned = (uint8_t *)((lb + 15) & ~(uintptr_t)15);
    gif.iError = GIF_SUCCESS;
    gif.pfnDraw = draw_line;
    gif.pfnOpen = NULL;
    gif.pfnClose = NULL;
    gif.GIFFile.iSize = (int32_t)f_size(&fil);

    // Contiguous file? -> decode directly from XIP flash (no FatFs copies).
    clmt[0] = sizeof clmt / sizeof clmt[0];
    fil.cltbl = clmt;
    bool contiguous = f_lseek(&fil, CREATE_LINKMAP) == FR_OK && clmt[0] == 4;
    if (contiguous) {
        FATFS *fs = fil.obj.fs;
        LBA_t sect = fs->database + (LBA_t)(clmt[2] - 2) * fs->csize;
        flashdisk_flush();
        gif.pfnRead = readMem;
        gif.pfnSeek = seekMem;
        gif.GIFFile.pData = (uint8_t *)flashdisk_xip_ptr((uint32_t)sect);
        info.memory_mapped = true;
    } else {
        fil.cltbl = NULL;
        f_lseek(&fil, 0);
        gif.pfnRead = fat_read;
        gif.pfnSeek = fat_seek;
        gif.GIFFile.fHandle = &fil;
    }
    if (!GIFInit(&gif)) {
        snprintf(err, sizeof err, "NOT A VALID GIF (%d)", gif.iError);
        gp_close();
        return false;
    }
    info.canvas_w = gif.iCanvasWidth;
    info.canvas_h = gif.iCanvasHeight;
    if (info.canvas_w > MAX_WIDTH) {
        snprintf(err, sizeof err, "TOO WIDE: %d PX (MAX %d)", info.canvas_w, MAX_WIDTH);
        gp_close();
        return false;
    }
    layout();
    frame_started = false;
    return true;
}

bool gp_open(const char *path, zoom_t zoom, gp_service_fn service) {
    gp_close();
    err[0] = 0;
    cur_zoom = zoom;
    service_cb = service;
    snprintf(path_buf, sizeof path_buf, "%s", path);
    return open_current();
}

int gp_frame(int *delay_ms) {
    if (!fil_open) return -1;
    info.dirty_y0 = LCD_HEIGHT; info.dirty_y1 = -1;
    if (gif.GIFFile.iPos >= gif.GIFFile.iSize - 1) {   // loop restart: start from a clean canvas
        prev_disposal = 0;
    }
    frame_started = false;
    int delay = 0;
    int rc = GIF_playFrame(&gif, &delay, NULL);
    if (gif.iError != GIF_SUCCESS && gif.iError != GIF_EMPTY_FRAME && rc == 0 && !frame_started) {
        snprintf(err, sizeof err, "DECODE ERROR %d", gif.iError);
        return -1;
    }
    if (delay_ms) *delay_ms = delay;
    return rc ? 1 : 0;
}

void gp_close(void) {
    if (fil_open) { f_close(&fil); fil_open = false; }
}

void gp_set_zoom(zoom_t zoom) {
    if (!fil_open) return;
    cur_zoom = zoom;
    layout();
    GIF_reset(&gif);
}
