// ST77916 QSPI LCD driver for the SC26 board (PIO1 + DMA).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "board_sc26.h"

// RGB565 helpers. Pixel buffers handed to lcd_blit() must be big-endian
// (high byte first), which is what the panel expects on the wire.
#define RGB565(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define BSWAP16(x)       ((uint16_t)(((x) >> 8) | ((x) << 8)))
#define RGB565_BE(r,g,b) BSWAP16(RGB565(r, g, b))

void lcd_init(void);                                 // QSPI at 37.5 MHz
void lcd_init_ex(float pio_clkdiv);                  // 1.0 = 75 MHz (original firmware's speed)
bool lcd_wait_te(uint32_t timeout_us);               // wait for start of V-blank; false on timeout
void lcd_backlight(uint8_t percent);                 // 0..100
void lcd_set_rotation(uint16_t degrees);             // 0, 90, 180, 270
void lcd_send_cmd(uint8_t cmd, const uint8_t *data, size_t len);
void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);   // color: native RGB565
// Blit a w*h big-endian RGB565 buffer. Uses DMA; returns when the transfer is finished.
void lcd_blit(int x, int y, int w, int h, const uint16_t *pixels_be);
// Start a blit and return immediately; call lcd_blit_wait() before touching the buffer again.
void lcd_blit_async(int x, int y, int w, int h, const uint16_t *pixels_be);
void lcd_blit_wait(void);
// Streaming: open one window, then push big-endian RGB565 data in any number of chunks
// (each chunk is DMA'd while the caller prepares the next; at most one in flight).
void lcd_stream_begin(int x, int y, int w, int h);
void lcd_stream_push(const void *data_be, size_t bytes);
void lcd_stream_end(void);
bool lcd_blit_busy(void);

// Text (built-in 10x19 font), scale >= 1. Returns x after the last character.
int  lcd_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
int  lcd_text_width(const char *s, int scale);
