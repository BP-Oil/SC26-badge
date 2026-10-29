// Power-up sequence: a BIOS-style POST with live system info, then an animated splash
// (the badge's two astronauts floating over a starfield, an "SC26 OS" title and a
// marching-blocks loader bar mirrored on the LED ring). Any button skips ahead.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef void (*boot_service_fn)(void);

void bios_begin(void);                          // clear, POST LED blip, header/CPU/MEM/ROM lines
void bios_print(const char *s, int pause_ms);   // next console line, then pause (skippable)
void bios_printf_pause(int pause_ms, const char *fmt, ...);
void bios_wait(int ms, boot_service_fn svc);    // blink the cursor
bool boot_skipped(void);                        // a button was pressed during boot

// Runs the splash (~6 s, or until a button press), then leaves the screen black.
// `svc` is called every frame (USB). `backlight` is the user's brightness to restore.
void boot_splash(boot_service_fn svc, uint8_t backlight);
