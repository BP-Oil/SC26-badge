// 512-byte block device on the board's QSPI flash, shared by USB mass storage and FatFs.
//
// The drive lives at the end of flash (see flash_layout.h); OS-only UF2 updates never touch it.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "flash_layout.h"
#define DISK_BLOCK_SIZE   512u
#define DISK_BLOCK_COUNT  (FS_FLASH_SIZE / DISK_BLOCK_SIZE)

void     flashdisk_read(uint32_t lba, uint8_t *buf, uint32_t count);
void     flashdisk_write(uint32_t lba, const uint8_t *buf, uint32_t count);
void     flashdisk_flush(void);              // commit the cached 4 KB sector, if dirty
bool     flashdisk_dirty(void);
uint64_t flashdisk_last_write_us(void);      // time of the most recent write (0 = never)
// Memory-mapped (XIP) address of a block, for zero-copy reads of contiguous files.
const uint8_t *flashdisk_xip_ptr(uint32_t lba);
