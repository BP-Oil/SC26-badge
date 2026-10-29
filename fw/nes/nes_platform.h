#pragma once
#include <stdint.h>
// Run the NES ROM image at `rom` (iNES file contents, memory-mapped). Returns when the player
// holds CENTER to quit.
int nes_run(const uint8_t *rom);
