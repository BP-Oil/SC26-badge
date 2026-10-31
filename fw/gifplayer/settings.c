// Settings sector layout (4 KB at SETTINGS_OFFSET):
//
//   offset   0  BP's original record, unchanged: { "SC26", version 1, settings_v1_t, crc }.
//               SC26 OS v2.0 and the DOOM image (sc26_doom.c) read exactly this, so they keep
//               the GIF, brightness, zoom, LED mode and LED-sides settings.
//   offset 512  LED scenes record: { "SCNE", 1, scene name, speed, brightness, blob length,
//               crc }, then the selected file scene's frames (scenes_export_blob) and their
//               crc, so the DOOM image can play a file scene too.
//
// An earlier build of the LED-scenes mod stored one "version 2" record at offset 0 instead;
// that is still read, and rewritten in this layout on the next save.
#include <string.h>
#include <stddef.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "flash_disk.h"
#include "settings.h"

#define SETTINGS_MAGIC   0x53433236u                              // "SC26"
#define SCENES_MAGIC     0x454E4353u                              // "SCNE"
#define SAVE_DELAY_US    3000000ull
#define EXT_OFFSET       512

typedef struct {
    uint8_t stay, led_mode, brightness, zoom, ring_cw;
    char    gif[SETTINGS_NAME_LEN];
} settings_v1_t;
typedef struct { uint32_t magic, version; settings_v1_t s; uint32_t crc; } stored_v1_t;

typedef struct {                                 // the scene fields of settings_t, in flash
    char     scene[24];
    uint8_t  speed, bright;
    uint16_t blob_len;
} scene_rec_t;
typedef struct { uint32_t magic, version; scene_rec_t r; uint32_t crc; } stored_scene_t;
#define BLOB_OFFSET (EXT_OFFSET + sizeof(stored_scene_t))

// the earlier mod build's single record ("version 2" at offset 0)
typedef struct {
    settings_v1_t v1;
    char    scene[24];
    uint8_t scene_speed, scene_bright, reserved;
} settings_old2_t;
typedef struct { uint32_t magic, version; settings_old2_t s; uint32_t crc; } stored_old2_t;

_Static_assert(sizeof(stored_v1_t) <= EXT_OFFSET, "v1 record overlaps the scene record");
_Static_assert(BLOB_OFFSET + SETTINGS_BLOB_MAX + 4 <= FLASH_SECTOR_SIZE, "scene blob too big");

static settings_t pending;
static uint8_t    pending_blob[SETTINGS_BLOB_MAX];
static size_t     pending_blob_len;
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

static const uint8_t *flash_sector(void) { return (const uint8_t *)(XIP_BASE + SETTINGS_OFFSET); }

static void scene_defaults(settings_t *s) {
    memset(s->scene, 0, sizeof s->scene);
    s->scene_speed = 3; s->scene_bright = 40;
}

static void from_v1(settings_t *s, const settings_v1_t *v) {
    s->stay = v->stay; s->led_mode = v->led_mode; s->brightness = v->brightness;
    s->zoom = v->zoom; s->ring_cw = v->ring_cw;
    memcpy(s->gif, v->gif, SETTINGS_NAME_LEN);
    s->gif[SETTINGS_NAME_LEN - 1] = 0;
}

static void to_v1(settings_v1_t *v, const settings_t *s) {
    memset(v, 0, sizeof *v);
    v->stay = s->stay; v->led_mode = s->led_mode; v->brightness = s->brightness;
    v->zoom = s->zoom; v->ring_cw = s->ring_cw;
    memcpy(v->gif, s->gif, SETTINGS_NAME_LEN);
}

bool settings_load(settings_t *s) {
    const uint8_t *f = flash_sector();
    memset(s, 0, sizeof *s);
    scene_defaults(s);

    const stored_v1_t *v1 = (const stored_v1_t *)f;
    const stored_old2_t *o2 = (const stored_old2_t *)f;
    if (v1->magic == SETTINGS_MAGIC && v1->version == 1 &&
        v1->crc == crc32((const uint8_t *)&v1->s, sizeof v1->s)) {
        from_v1(s, &v1->s);
        const stored_scene_t *sc = (const stored_scene_t *)(f + EXT_OFFSET);
        if (sc->magic == SCENES_MAGIC && sc->version == 1 &&
            sc->crc == crc32((const uint8_t *)&sc->r, sizeof sc->r)) {
            memcpy(s->scene, sc->r.scene, sizeof s->scene);
            s->scene[sizeof s->scene - 1] = 0;
            s->scene_speed = sc->r.speed; s->scene_bright = sc->r.bright;
        }
        return true;
    }
    if (o2->magic == SETTINGS_MAGIC && o2->version == 2 &&
        o2->crc == crc32((const uint8_t *)&o2->s, sizeof o2->s)) {
        from_v1(s, &o2->s.v1);
        memcpy(s->scene, o2->s.scene, sizeof s->scene);
        s->scene[sizeof s->scene - 1] = 0;
        s->scene_speed = o2->s.scene_speed; s->scene_bright = o2->s.scene_bright;
        return true;
    }
    s->brightness = 100;
    return false;
}

void settings_changed(const settings_t *s, const uint8_t *blob, size_t blob_len) {
    pending = *s;
    pending_blob_len = (blob && blob_len <= SETTINGS_BLOB_MAX) ? blob_len : 0;
    if (pending_blob_len) memcpy(pending_blob, blob, pending_blob_len);
    dirty = true;
    changed_at = time_us_64();
}

void settings_poll(bool allowed) {
    if (!dirty || !allowed || time_us_64() - changed_at < SAVE_DELAY_US) return;
    dirty = false;

    static uint8_t sector[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
    memset(sector, 0xFF, sizeof sector);
    stored_v1_t v1;
    memset(&v1, 0, sizeof v1);
    v1.magic = SETTINGS_MAGIC; v1.version = 1;
    to_v1(&v1.s, &pending);
    v1.crc = crc32((const uint8_t *)&v1.s, sizeof v1.s);
    memcpy(sector, &v1, sizeof v1);

    stored_scene_t sc;
    memset(&sc, 0, sizeof sc);
    sc.magic = SCENES_MAGIC; sc.version = 1;
    memcpy(sc.r.scene, pending.scene, sizeof sc.r.scene);
    sc.r.speed = pending.scene_speed; sc.r.bright = pending.scene_bright;
    sc.r.blob_len = (uint16_t)pending_blob_len;
    sc.crc = crc32((const uint8_t *)&sc.r, sizeof sc.r);
    memcpy(sector + EXT_OFFSET, &sc, sizeof sc);
    if (pending_blob_len) {
        memcpy(sector + BLOB_OFFSET, pending_blob, pending_blob_len);
        uint32_t bc = crc32(pending_blob, pending_blob_len);
        memcpy(sector + BLOB_OFFSET + pending_blob_len, &bc, 4);   // blob crc right after it
    }

    if (memcmp(sector, flash_sector(), sizeof sector) == 0) return;     // unchanged
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_OFFSET, sector, sizeof sector);
    restore_interrupts(ints);
}

void settings_flush(void) {
    if (!dirty) return;
    changed_at = 0;
    settings_poll(true);
}
