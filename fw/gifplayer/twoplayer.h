// SC26 OS "2 PLAYER": two badges joined by a USB-C cable play NES games together.
//
// PLAYER 1's USB port runs as a host (the badge restarts into that mode, see linktest.h);
// PLAYER 2 stays a normal USB device and talks over its USB serial port. PLAYER 1 picks a game
// from its GAMES folder; if PLAYER 2 doesn't have it, PLAYER 1 sends it over and PLAYER 2 keeps
// a copy. Both badges then run the emulator in lockstep: every frame each badge sends its
// buttons for a frame a little in the future and waits for the other badge's buttons for the
// current frame, so both emulators see exactly the same input and stay identical.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void twoplayer_p2_run(void);     // normal OS menu: lobby as PLAYER 2 (A here = become PLAYER 1)
void twoplayer_p1_run(void);     // after the host-mode restart; returns when PLAYER 1 exits

// Used by nes_platform.cpp while a 2-player game is running.
bool tp_nes_active(void);
void tp_poll(void);
bool tp_nes_exchange(uint8_t pad, bool quit, uint8_t want_skip, uint32_t ram_crc,
                     uint8_t *pad1, uint8_t *pad2, bool *quit_any, uint8_t *skip, bool *desync);
