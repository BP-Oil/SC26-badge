// Flash block device with a single 4 KB write-back sector cache.
// Erase granularity is 4 KB, so 512-byte writes are merged in RAM and committed
// (erase + program) when a different sector is touched, on flush, or after idle.
// Must be called from core 0 with core 1 idle (flash ops stall XIP).

#include "flash_disk.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#define BLOCKS_PER_SECTOR (FLASH_SECTOR_SIZE / DISK_BLOCK_SIZE)

static uint8_t  cache[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
static int32_t  cache_sector = -1;
static bool     cache_dirty;
static uint64_t last_write_us;

const uint8_t *flashdisk_xip_ptr(uint32_t lba) {
    return (const uint8_t *)(XIP_BASE + FS_FLASH_OFFSET + lba * DISK_BLOCK_SIZE);
}

static bool all_ff(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i] != 0xFF) return false;
    return true;
}

void flashdisk_flush(void) {
    if (cache_sector < 0 || !cache_dirty) return;
    const uint8_t *cur = flashdisk_xip_ptr((uint32_t)cache_sector * BLOCKS_PER_SECTOR);
    if (memcmp(cur, cache, FLASH_SECTOR_SIZE) != 0) {
        uint32_t addr = FS_FLASH_OFFSET + (uint32_t)cache_sector * FLASH_SECTOR_SIZE;
        bool need_erase = !all_ff(cur, FLASH_SECTOR_SIZE);
        uint32_t ints = save_and_disable_interrupts();
        if (need_erase) flash_range_erase(addr, FLASH_SECTOR_SIZE);
        flash_range_program(addr, cache, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);
    }
    cache_dirty = false;
}

bool flashdisk_dirty(void) { return cache_dirty; }
uint64_t flashdisk_last_write_us(void) { return last_write_us; }

void flashdisk_read(uint32_t lba, uint8_t *buf, uint32_t count) {
    for (; count; count--, lba++, buf += DISK_BLOCK_SIZE) {
        if (lba >= DISK_BLOCK_COUNT) { memset(buf, 0, DISK_BLOCK_SIZE); continue; }
        int32_t sector = (int32_t)(lba / BLOCKS_PER_SECTOR);
        if (sector == cache_sector)
            memcpy(buf, cache + (lba % BLOCKS_PER_SECTOR) * DISK_BLOCK_SIZE, DISK_BLOCK_SIZE);
        else
            memcpy(buf, flashdisk_xip_ptr(lba), DISK_BLOCK_SIZE);
    }
}

void flashdisk_write(uint32_t lba, const uint8_t *buf, uint32_t count) {
    for (; count; count--, lba++, buf += DISK_BLOCK_SIZE) {
        if (lba >= DISK_BLOCK_COUNT) continue;
        int32_t sector = (int32_t)(lba / BLOCKS_PER_SECTOR);
        if (sector != cache_sector) {
            flashdisk_flush();
            memcpy(cache, flashdisk_xip_ptr((uint32_t)sector * BLOCKS_PER_SECTOR), FLASH_SECTOR_SIZE);
            cache_sector = sector;
        }
        memcpy(cache + (lba % BLOCKS_PER_SECTOR) * DISK_BLOCK_SIZE, buf, DISK_BLOCK_SIZE);
        cache_dirty = true;
    }
    last_write_us = time_us_64();
}
