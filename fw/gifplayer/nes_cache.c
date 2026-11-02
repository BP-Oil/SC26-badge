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

// ---------------------------------------------------------------- 2-player helpers
// The currently cached ROM (pointer, size, CRC), as written by nescache_load / nescache_rx_end.
const uint8_t *nescache_current(uint32_t *size, uint32_t *crc) {
    const hdr_t *cur = (const hdr_t *)(XIP_BASE + NESCACHE_OFFSET);
    if (cur->magic != CACHE_MAGIC) return NULL;
    if (size) *size = cur->size;
    if (crc) *crc = cur->crc;
    return (const uint8_t *)(XIP_BASE + DATA_OFFSET);
}

// CRC32 + size of a file on the drive (0 if it can't be read).
uint32_t nescache_file_crc(const char *path, uint32_t *size) {
    FIL f; UINT br; uint32_t crc = 0;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;
    if (size) *size = (uint32_t)f_size(&f);
    while (f_read(&f, sector, sizeof sector, &br) == FR_OK && br) crc = crc32_update(crc, sector, br);
    f_close(&f);
    return crc;
}

// Receive a ROM straight into the cache slot (PLAYER 2 getting a game it doesn't have).
// Data must arrive in order; each full 4 KB sector is programmed as soon as it is complete.
static uint32_t rx_size, rx_crc, rx_got;
static bool rx_ok;

bool nescache_rx_begin(uint32_t size, uint32_t crc) {
    rx_ok = size >= 16 && size <= MAX_ROM;
    if (!rx_ok) return false;
    rx_size = size; rx_crc = crc; rx_got = 0;
    memset(sector, 0xFF, sizeof sector);
    flash_sector(NESCACHE_OFFSET, sector);                     // no valid header until the end
    return true;
}

bool nescache_rx_data(const uint8_t *d, uint32_t n) {
    while (rx_ok && n) {
        if (rx_got >= rx_size) { rx_ok = false; break; }
        uint32_t in_sec = rx_got % FLASH_SECTOR_SIZE;
        uint32_t take = FLASH_SECTOR_SIZE - in_sec;
        if (take > n) take = n;
        if (take > rx_size - rx_got) take = rx_size - rx_got;
        memcpy(sector + in_sec, d, take);
        rx_got += take; d += take; n -= take;
        if (rx_got % FLASH_SECTOR_SIZE == 0 || rx_got == rx_size) {
            flash_sector(DATA_OFFSET + (rx_got - 1) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE, sector);
            memset(sector, 0xFF, sizeof sector);
        }
    }
    return rx_ok;
}

uint32_t nescache_rx_progress(void) { return rx_size ? rx_got * 100 / rx_size : 0; }

const uint8_t *nescache_rx_end(const char *name, char *err, size_t errlen) {
    if (!rx_ok || rx_got != rx_size) { snprintf(err, errlen, "TRANSFER INCOMPLETE"); return NULL; }
    uint32_t vcrc = crc32_update(0, (const uint8_t *)(XIP_BASE + DATA_OFFSET), rx_size);
    if (vcrc != rx_crc) { snprintf(err, errlen, "ROM CRC MISMATCH"); return NULL; }
    memset(sector, 0xFF, sizeof sector);
    hdr_t h = { CACHE_MAGIC, rx_size, rx_crc, {0} };
    snprintf(h.name, sizeof h.name, "%s", name);
    memcpy(sector, &h, sizeof h);
    flash_sector(NESCACHE_OFFSET, sector);
    return (const uint8_t *)(XIP_BASE + DATA_OFFSET);
}

// Copy the cached ROM into a file on the drive (so PLAYER 2 keeps the game it was sent).
bool nescache_save_to(const char *path) {
    uint32_t size = 0;
    const uint8_t *rom = nescache_current(&size, NULL);
    if (!rom) return false;
    FIL f; UINT bw;
    if (f_open(&f, path, FA_CREATE_NEW | FA_WRITE) != FR_OK) return false;
    bool ok = true;
    for (uint32_t off = 0; off < size && ok; off += sizeof sector) {
        uint32_t n = size - off < sizeof sector ? size - off : sizeof sector;
        memcpy(sector, rom + off, n);                           // via RAM: flash writes can't read XIP
        ok = f_write(&f, sector, n, &bw) == FR_OK && bw == n;
    }
    f_close(&f);
    if (!ok) f_unlink(path);
    return ok;
}
