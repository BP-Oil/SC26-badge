// Streaming GIF decoder -> 360x360 big-endian RGB565 framebuffer, with scaling for the round panel.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "board_sc26.h"

typedef enum {
    ZOOM_AUTO = 0,    // default: square GIFs 1:1 to the 360x360 square, others fit inside the circle
    ZOOM_CIRCLE,      // whole image visible inside the round panel
    ZOOM_SQUARE,      // fit the 360x360 square (corners of the image fall outside the circle)
    ZOOM_FILL,        // fill the whole circle, cropping the image edges
    ZOOM_COUNT
} zoom_t;

// 256 KB shared arena: the GIF player's 360x360 big-endian RGB565 framebuffer, and (only while a
// NES game runs) the active NES mapper's large RAM buffers. Never both at the same time.
#define SC26_ARENA_BYTES 0x40000u
extern uint16_t gp_fb[SC26_ARENA_BYTES / 2];

typedef struct {
    int canvas_w, canvas_h;
    int dest_x, dest_y, dest_w, dest_h;   // where the scaled image lands on screen
    bool memory_mapped;                    // file is contiguous in flash -> zero-copy decode
    int dirty_y0, dirty_y1;                // rows changed by the last frame (inclusive), y1 < y0 = none
} gp_info_t;

// Called every few decoded lines so USB keeps running during long frames.
typedef void (*gp_service_fn)(void);

bool gp_open(const char *path, zoom_t zoom, gp_service_fn service);  // false on error (see gp_error)
int  gp_frame(int *delay_ms);         // 1 = more frames, 0 = loop finished, -1 = error
void gp_close(void);
void gp_set_zoom(zoom_t zoom);        // re-layout; clears screen, restarts at frame 0
const gp_info_t *gp_info(void);
const char *gp_error(void);
const char *gp_zoom_name(zoom_t z);
