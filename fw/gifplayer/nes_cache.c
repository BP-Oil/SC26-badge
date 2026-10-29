// Copies the chosen .nes file from the USB drive into a fixed flash slot so the emulator can run
// it in place (memory-mapped). Re-flashes only when a different ROM (name/size/CRC) is chosen.
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "fatfs/ff.h"
#include "flash_layout.h"
#include "nes_cache.h"

#define CACHE_MAGIC 0x4E455343u          // "NESC"
#define DATA_OFFSET (NESCACHE_OFFSET + FLASH_SECTOR_SIZE)
#define MAX_ROM     (NESCACHE_SIZE - FLASH_SECTOR_SIZE)

typedef struct { uint32_t magic, size, crc; char name[64]; } hdr_t;

static uint8_t sector[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    c = ~c;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}

static void flash_sector(uint32_t off, const uint8_t *data) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    flash_range_program(off, data, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

const uint8_t *nescache_load(const char *path, char *err, size_t errlen, nescache_progress_fn progress) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) { snprintf(err, errlen, "CAN'T OPEN ROM"); return NULL; }
    uint32_t size = (uint32_t)f_size(&f);
    if (size < 16 || size > MAX_ROM) { f_close(&f); snprintf(err, errlen, "ROM TOO BIG (MAX %uK)", MAX_ROM / 1024); return NULL; }

    // CRC of the file on the drive
    uint32_t crc = 0; UINT br;
    while (f_read(&f, sector, sizeof sector, &br) == FR_OK && br) crc = crc32_update(crc, sector, br);

    const hdr_t *cur = (const hdr_t *)(XIP_BASE + NESCACHE_OFFSET);
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    if (cur->magic == CACHE_MAGIC && cur->size == size && cur->crc == crc) {
        f_close(&f);
        return (const uint8_t *)(XIP_BASE + DATA_OFFSET);
    }

    // (re)write: data first, header last
    f_lseek(&f, 0);
    for (uint32_t off = 0; off < size; off += FLASH_SECTOR_SIZE) {
        memset(sector, 0xFF, sizeof sector);
        f_read(&f, sector, sizeof sector, &br);
        flash_sector(DATA_OFFSET + off, sector);
        if (progress) progress((int)((off + FLASH_SECTOR_SIZE) * 100 / size));
    }
    f_close(&f);
    memset(sector, 0xFF, sizeof sector);
    hdr_t h = { CACHE_MAGIC, size, crc, {0} };
    snprintf(h.name, sizeof h.name, "%s", base);
    memcpy(sector, &h, sizeof h);
    flash_sector(NESCACHE_OFFSET, sector);

    // verify
    uint32_t vcrc = crc32_update(0, (const uint8_t *)(XIP_BASE + DATA_OFFSET), size);
    if (vcrc != crc) { snprintf(err, errlen, "FLASH VERIFY FAILED"); return NULL; }
    return (const uint8_t *)(XIP_BASE + DATA_OFFSET);
}
