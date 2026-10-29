// SC26 badge port glue for rp2040-doom (GPL-2.0+, like the rest of this tree).
// Replaces VGA/scanvideo output with the badge's 360x360 ST77916 QSPI panel, maps the
// 7 buttons to Doom keys, drives the 9-LED ring, and makes flash saves work when this
// image is chain-booted from the middle of flash by SC26 OS.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "pico.h"

#if SC26_BOARD
// Flash slot this image lives in (see SC26 OS flash_layout.h): image 512 KB, WHD after it,
// save games grow down from the end of the slot.
#define SC26_DOOM_SIZE      0x680000u
#define SC26_WHD_OFFSET     0x080000u           // relative to the slot start

// where the 320x200 picture sits on the round 360x360 panel
#define SC26_VIEW_X 20
#define SC26_VIEW_Y 70

void sc26_early_init(void);                     // first thing in main(): address translation, LEDs off
void sc26_lcd_init(void);                       // panel + PIO + DMA (call on core1, after clock setup)
void sc26_lcd_begin_pixels(int x, int y, int w, int h);
void sc26_lcd_start_dma(const void *src, uint32_t bytes);
void sc26_lcd_end_pixels(void);
void sc26_backlight(int percent);
extern int sc26_lcd_dma;

uint32_t sc26_flash_slot_base(void);            // physical flash offset of this image
bool sc26_flash_offset_ok(uint32_t logical_offs);

void sc26_input_init(void);
void sc26_input_poll(void);                     // call every tic: buttons -> Doom events
void sc26_leds_update(void);                    // called by the video code with fresh colours
void sc26_set_led_target(int i, uint8_t r, uint8_t g, uint8_t b);
void sc26_exit_to_os(void) __attribute__((noreturn));
extern bool sc26_always_run;

// Diagnostics for SC26 OS (it writes DOOMLOG.TXT after the reboot). Kept in the watchdog
// scratch registers, which survive a watchdog reboot:
//   scratch0 = magic<<24 | core1 stage<<16 | core0 stage<<8 | faulting core
//   scratch1 = PC / message address, scratch2 = LR / caller, scratch3 = CFSR
#define SC26_DIAG_RUN   0xD0u    // running (seen by SC26 OS => watchdog fired: a hang)
#define SC26_DIAG_FAULT 0xDFu    // hard fault
#define SC26_DIAG_ERROR 0xDEu    // I_Error(): scratch1 = format string
#define SC26_DIAG_PANIC 0xDCu    // SDK panic(): scratch1 = format string
#define SC26_DIAG_QUIT  0xDAu    // normal quit
#define SC26_DIAG_HANG  0xDBu    // core 0 stopped polling: scratch1/2 = its PC/LR, scratch3 = core 1 PC,
                                 // scratch5 = panel pump state, scratch6 = frames shown, scratch7 = pump resets
void sc26_lcd_recover(void);
#define SC26_DIAG_MAILBOX 0x20078000u     // RAM above SC26 OS's .bss: [magic, n, words..., crc]
void sc26_stage(int core, int stage);
void sc26_error(const char *fmt, ...) __attribute__((noreturn));
#endif
