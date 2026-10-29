#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void (*nescache_progress_fn)(int percent);
// Returns a memory-mapped pointer to the ROM (copied into the NES cache slot if needed), or NULL.
const uint8_t *nescache_load(const char *path, char *err, size_t errlen, nescache_progress_fn progress);
