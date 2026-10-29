// Launch another firmware image stored elsewhere in flash (original Wayfarer firmware, DOOM).
// The RP2350 boot ROM's chain_image() finds the image's IMAGE_DEF block, maps the region to the
// start of the XIP window with the QMI address translator, and starts it - so images linked for
// 0x10000000 run unmodified from any 4 KB-aligned flash offset. Power-cycling returns to SC26 OS.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/sync.h"
#include "hardware/structs/xip_ctrl.h"
#include "chain.h"

bool chain_image_present(uint32_t flash_offset) {
    // A picobin block starts with 0xffffded3 and the IMAGE_DEF must begin in the first 4 KB.
    const uint32_t *p = (const uint32_t *)(XIP_BASE + flash_offset);
    for (int i = 0; i < 1024; i++) if (p[i] == 0xffffded3u) return true;
    return false;
}

int chain_boot(uint32_t flash_offset, uint32_t size) {
    static uint8_t workarea[4 * 1024] __attribute__((aligned(4)));
    // Leave the hardware quiet: no IRQs while the boot ROM rearranges flash mapping.
    uint32_t ints = save_and_disable_interrupts();
    int rc = rom_chain_image(workarea, sizeof workarea, (uint32_t)(XIP_BASE + flash_offset), size);
    restore_interrupts(ints);
    return rc;   // only returns on failure
}
