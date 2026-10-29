#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "flash_disk.h"
#include "settings.h"

#define SETTINGS_MAGIC   0x53433236u                              // "SC26"
#define SAVE_DELAY_US    3000000ull

typedef struct {
    uint32_t   magic;
    uint32_t   version;
    settings_t s;
    uint32_t   crc;
} stored_t;

static settings_t pending;
static bool       dirty;
static uint64_t   changed_at;

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static const stored_t *flash_copy(void) { return (const stored_t *)(XIP_BASE + SETTINGS_OFFSET); }

bool settings_load(settings_t *s) {
    const stored_t *st = flash_copy();
    if (st->magic == SETTINGS_MAGIC && st->version == 1 &&
        st->crc == crc32((const uint8_t *)&st->s, sizeof st->s)) {
        *s = st->s;
        s->gif[SETTINGS_NAME_LEN - 1] = 0;
        return true;
    }
    memset(s, 0, sizeof *s);
    s->led_mode = 0; s->brightness = 100; s->zoom = 0; s->ring_cw = 0;
    return false;
}

void settings_changed(const settings_t *s) {
    pending = *s;
    dirty = true;
    changed_at = time_us_64();
}

void settings_poll(bool allowed) {
    if (!dirty || !allowed || time_us_64() - changed_at < SAVE_DELAY_US) return;
    dirty = false;
    const stored_t *cur = flash_copy();
    if (cur->magic == SETTINGS_MAGIC && memcmp(&cur->s, &pending, sizeof pending) == 0) return;   // unchanged
    static uint8_t page[FLASH_PAGE_SIZE * 2] __attribute__((aligned(4)));
    _Static_assert(sizeof(stored_t) <= sizeof page, "settings too big");
    memset(page, 0xFF, sizeof page);
    stored_t st = { SETTINGS_MAGIC, 1, pending, crc32((const uint8_t *)&pending, sizeof pending) };
    memcpy(page, &st, sizeof st);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_OFFSET, page, sizeof page);
    restore_interrupts(ints);
}

void settings_flush(void) {
    if (!dirty) return;
    changed_at = 0;
    settings_poll(true);
}
