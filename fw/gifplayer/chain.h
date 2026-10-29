#pragma once
#include <stdint.h>
#include <stdbool.h>
bool chain_image_present(uint32_t flash_offset);   // is there a bootable image in this slot?
int  chain_boot(uint32_t flash_offset, uint32_t size);  // does not return on success
