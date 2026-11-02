#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef void (*nescache_progress_fn)(int percent);
// Returns a memory-mapped pointer to the ROM (copied into the NES cache slot if needed), or NULL.
const uint8_t *nescache_load(const char *path, char *err, size_t errlen, nescache_progress_fn progress);

// 2-player helpers (twoplayer.c)
const uint8_t *nescache_current(uint32_t *size, uint32_t *crc);
uint32_t nescache_file_crc(const char *path, uint32_t *size);
bool nescache_rx_begin(uint32_t size, uint32_t crc);
bool nescache_rx_data(const uint8_t *d, uint32_t n);
uint32_t nescache_rx_progress(void);
const uint8_t *nescache_rx_end(const char *name, char *err, size_t errlen);
bool nescache_save_to(const char *path);
