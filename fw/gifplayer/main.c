// SC26 GIF player
//
// Plays every .gif in the root of the board's "SC26 GIFS" USB drive on the round 360x360
// ST77916 panel, with the 9-LED ring either reproducing the original firmware's animation
// or following the picture (ambient). Copy GIFs onto the drive from your computer while the
// player runs; playback pauses while files arrive and the newest file plays when done.
//
// Controls
//   LEFT / RIGHT     previous / next GIF
//   UP / DOWN        screen brightness
//   CENTER (tap)     zoom: auto -> fit circle -> fit square -> fill
//   CENTER (hold 1s) MENU: GIF player, HOW TO USE (button help), NES games in /GAMES, DOOM,
//                    original firmware, update mode
//   A (tap)          pause / resume
//   A (hold 1 s)     STAY on this GIF (no auto-advance) <-> auto-advance; L/R still change GIF
//   B (tap)          LED mode: original -> ambient -> off
//   B (hold 1.5 s)   swap the LED sides (if ambient colours look swapped left/right)
//   A + B (hold 2 s) reboot into BOOTSEL (USB bootloader) for firmware updates
// Also: opening the USB serial port at 1200 baud reboots into BOOTSEL.
// The current GIF, STAY, brightness, zoom and LED settings survive power-off (settings.c).

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "tusb.h"
#include "fatfs/ff.h"
#include "lcd.h"
#include "buttons.h"
#include "flash_disk.h"
#include "gif_player.h"
#include "ambient.h"
#include "usb_glue.h"
#include "settings.h"
#include "flash_layout.h"
#include "nes_cache.h"
#include "nes_platform.h"
#include "chain.h"
#include "boot_splash.h"
#include "help.h"
#include "linktest.h"
#include "twoplayer.h"
#include "leds.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#define MAX_FILES        64
#define NAME_LEN         96
#ifndef PLAY_MIN_MS
#define PLAY_MIN_MS      20000      // auto-advance after this long (at the end of a loop)
#endif
#define STILL_MS         8000       // single-frame GIFs
#define WRITE_SETTLE_MS  1500       // host must be quiet this long before we re-read the drive
#define FLUSH_IDLE_MS    300
#define OVERLAY_MS       2500
#define TEXT_Y           292

#define COL_BG   RGB565(0, 0, 0)
#define COL_FG   RGB565(235, 235, 235)
#define COL_ACC  RGB565(60, 230, 120)
#define COL_DIM  RGB565(110, 120, 140)

typedef struct { char name[NAME_LEN]; FSIZE_t size; uint32_t stamp; } entry_t;

static FATFS   fs;
static bool    fs_mounted;
static entry_t files[MAX_FILES];
static int     nfiles, cur = -1;

typedef enum { ST_PLAY, ST_EMPTY, ST_RECEIVING, ST_ERROR, ST_MENU, ST_HELP } state_t;
static state_t state;
static zoom_t  zoom = ZOOM_AUTO;
static bool    paused;
static int     brightness = 100;
static uint64_t play_started_us, next_frame_us;
static bool    loop_done, single_frame;
static int     frames_in_loop;
static char    overlay[40];
static uint64_t overlay_until_us;
static bool    overlay_on_screen;
static uint32_t fps_frames; static uint64_t fps_t0; static float fps;
static bool    stay;                 // keep playing the current GIF
static uint32_t prev_hash[MAX_FILES]; static int prev_n;

static uint32_t name_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)tolower((unsigned char)*s++); h *= 16777619u; }
    return h;
}

static void save_settings(void) {
    settings_t s = {0};
    s.stay = stay; s.led_mode = (uint8_t)ambient_mode(); s.brightness = (uint8_t)brightness;
    s.zoom = (uint8_t)zoom; s.ring_cw = ambient_clockwise();
    if (cur >= 0 && cur < nfiles) snprintf(s.gif, sizeof s.gif, "%s", files[cur].name);
    settings_changed(&s);
}

static int find_file(const char *name) {
    for (int i = 0; i < nfiles; i++) if (strcasecmp(files[i].name, name) == 0) return i;
    return -1;
}

// ------------------------------------------------------------------ helpers
static uint64_t now_us(void) { return time_us_64(); }

static void text_center(int y, const char *s, uint16_t fg, int scale) {
    lcd_draw_text((LCD_WIDTH - lcd_text_width(s, scale)) / 2, y, s, fg, COL_BG, scale);
}

static void screen_message(const char *l1, const char *l2, const char *l3, uint16_t c1) {
    lcd_blit_wait();
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COL_BG);
    if (l1) text_center(120, l1, c1, 2);
    if (l2) text_center(180, l2, COL_FG, 1);
    if (l3) text_center(205, l3, COL_DIM, 1);
    overlay_on_screen = false;
}

static void show_overlay(const char *fmt, const char *arg) {
    snprintf(overlay, sizeof overlay, fmt, arg);
    for (char *p = overlay; *p; p++) *p = (char)toupper((unsigned char)*p);
    overlay_until_us = now_us() + OVERLAY_MS * 1000ull;
    if (state != ST_PLAY) return;
    lcd_blit_wait();
    lcd_fill_rect(40, TEXT_Y - 3, LCD_WIDTH - 80, 25, COL_BG);
    text_center(TEXT_Y, overlay, COL_ACC, 1);
    overlay_on_screen = true;
}

static void restore_overlay_area(void) {
    lcd_blit_wait();
    lcd_blit(0, TEXT_Y - 3, LCD_WIDTH, 25, &gp_fb[(TEXT_Y - 3) * LCD_WIDTH]);
    overlay_on_screen = false;
}

static void service(void);          // USB + buttons + LEDs; safe to call during decode

// ------------------------------------------------------------------ filesystem
static const char README[] =
    "SC26 GIF PLAYER\r\n"
    "===============\r\n\r\n"
    "Drag .gif files into the top level of this drive. Playback pauses while files\r\n"
    "copy, then the newest GIF starts. Eject the drive before unplugging.\r\n\r\n"
    "Best results: 360x360 (or smaller) GIFs, under ~30 fps, content centred - the\r\n"
    "screen is round. Run gifprep.py on your computer to fit any GIF automatically.\r\n\r\n"
    "Limits: up to 1024 px wide; 64 files; ~6.5 MB total. Larger GIFs play slower.\r\n"
    "macOS Trash: deleted files stay in .Trashes until you Empty Trash.\r\n\r\n"
    "Buttons: L/R prev/next, U/D brightness, CENTER zoom, A pause (hold A: stay on this GIF),\r\n"
    "B LED mode (hold: swap LED sides), hold A+B: firmware update mode.\r\n"
    "Hold CENTER for the menu; HOW TO USE there shows every button on screen.\r\n\r\n"
    "GAMES: put NES games (.nes files) in the GAMES folder. Hold CENTER for the menu.\r\n";

static const char GAMES_README[] =
    "Drop NES games (.nes, iNES format, up to 504 KB) into this folder.\r\n"
    "Hold CENTER on the board to open the menu and pick a game.\r\n\r\n"
    "In game: joystick = D-pad, A = A, B = B, tap CENTER = START,\r\n"
    "CENTER + B = SELECT, hold CENTER 1.5 s = back to the menu.\r\n"
    "Only use ROMs you own.\r\n";

static bool fs_mount_or_format(void) {
    FRESULT r = f_mount(&fs, "", 1);
    if (r == FR_NO_FILESYSTEM) {
        bios_print("  new drive: formatting ...", 0);
        static BYTE work[FF_MAX_SS * 8];
        MKFS_PARM opt = { FM_FAT | FM_SFD, 0, 0, 0, 4096 };
        r = f_mkfs("", &opt, work, sizeof work);
        if (r == FR_OK) r = f_mount(&fs, "", 1);
        if (r == FR_OK) {
            f_setlabel("SC26 GIFS");
            FIL f; UINT bw;
            if (f_open(&f, "README.TXT", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
                f_write(&f, README, sizeof README - 1, &bw);
                f_close(&f);
            }
            flashdisk_flush();
        }
        printf("format: %d\n", r);
    }
    fs_mounted = (r == FR_OK);
    if (fs_mounted) {                       // make sure the GAMES folder exists (before USB is up)
        FILINFO fi;
        if (f_stat("GAMES", &fi) != FR_OK) {
            f_mkdir("GAMES");
            FIL f; UINT bw;
            if (f_open(&f, "GAMES/README.TXT", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
                f_write(&f, GAMES_README, sizeof GAMES_README - 1, &bw);
                f_close(&f);
            }
            flashdisk_flush();
        }
    }
    return fs_mounted;
}

static bool is_gif(const FILINFO *fi) {
    if (fi->fattrib & (AM_DIR | AM_HID | AM_SYS)) return false;
    if (fi->fname[0] == '.') return false;                  // macOS "._" resource forks etc.
    size_t n = strlen(fi->fname);
    return n > 4 && strcasecmp(fi->fname + n - 4, ".gif") == 0;
}

static int cmp_entry(const void *a, const void *b) { return strcasecmp(((const entry_t *)a)->name, ((const entry_t *)b)->name); }

static void scan_files(void) {
    DIR dir; FILINFO fi;
    nfiles = 0;
    if (f_opendir(&dir, "/") != FR_OK) return;
    while (nfiles < MAX_FILES && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (!is_gif(&fi) || strlen(fi.fname) >= NAME_LEN) continue;
        strcpy(files[nfiles].name, fi.fname);
        files[nfiles].size = fi.fsize;
        files[nfiles].stamp = ((uint32_t)fi.fdate << 16) | fi.ftime;
        nfiles++;
    }
    f_closedir(&dir);
    qsort(files, (size_t)nfiles, sizeof files[0], cmp_entry);
    printf("found %d GIF(s)\n", nfiles);
}

// ------------------------------------------------------------------ playback
static void start_file(int idx) {
    if (nfiles == 0) {
        state = ST_EMPTY;
        screen_message("NO GIFS YET", "Copy .gif files onto the", "SC26 GIFS USB drive", COL_ACC);
        return;
    }
    cur = (idx % nfiles + nfiles) % nfiles;
    lcd_blit_wait();
    if (!gp_open(files[cur].name, zoom, service)) {
        printf("%s: %s\n", files[cur].name, gp_error());
        state = ST_ERROR;
        screen_message("CAN'T PLAY", files[cur].name, gp_error(), RGB565(255, 90, 60));
        play_started_us = now_us();
        return;
    }
    const gp_info_t *gi = gp_info();
    printf("play %d/%d %s  %dx%d -> %dx%d %s\n", cur + 1, nfiles, files[cur].name, gi->canvas_w, gi->canvas_h,
           gi->dest_w, gi->dest_h, gi->memory_mapped ? "(mapped)" : "(streamed)");
    state = ST_PLAY;
    paused = false;
    play_started_us = next_frame_us = now_us();
    loop_done = single_frame = false;
    frames_in_loop = 0;
    fps_frames = 0; fps_t0 = now_us();
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COL_BG);
    char info[32];
    snprintf(info, sizeof info, "%d/%d%s", cur + 1, nfiles, stay ? "  STAY" : "");
    show_overlay("%s", info);
    save_settings();
}

static void play_step(void) {
    if (paused || now_us() < next_frame_us) return;
    uint64_t t0 = now_us();
    int delay = 0;
    int rc = gp_frame(&delay);
    if (rc < 0) {
        printf("decode error: %s\n", gp_error());
        start_file(cur + 1);
        return;
    }
    const gp_info_t *gi = gp_info();
    if (gi->dirty_y1 >= gi->dirty_y0) {
        lcd_wait_te(20000);
        lcd_blit_async(0, gi->dirty_y0, LCD_WIDTH, gi->dirty_y1 - gi->dirty_y0 + 1, &gp_fb[gi->dirty_y0 * LCD_WIDTH]);
    }
    ambient_sample(gp_fb, gi->dest_x, gi->dest_y, gi->dest_w, gi->dest_h);
    if (overlay_on_screen) {
        lcd_blit_wait();
        if (now_us() < overlay_until_us) text_center(TEXT_Y, overlay, COL_ACC, 1);
        else restore_overlay_area();
    }
    frames_in_loop++;
    fps_frames++;
    if (now_us() - fps_t0 >= 2000000) {
        fps = fps_frames * 1e6f / (float)(now_us() - fps_t0);
        fps_frames = 0; fps_t0 = now_us();
    }
    if (delay < 20) delay = 100;               // browser convention for 0/10 ms delays
    next_frame_us = t0 + (uint64_t)delay * 1000;
    if (rc == 0) {                              // end of loop
        loop_done = true;
        if (frames_in_loop <= 1) single_frame = true;
        frames_in_loop = 0;
        uint64_t played = now_us() - play_started_us;
        uint64_t min_us = (single_frame ? STILL_MS : PLAY_MIN_MS) * 1000ull;
        if (!stay && nfiles > 1 && played >= min_us) start_file(cur + 1);
    }
}

// ------------------------------------------------------------------ drive activity
static void drive_state(void) {
    uint64_t lw = flashdisk_last_write_us();
    bool recent = lw && (now_us() - lw) < WRITE_SETTLE_MS * 1000ull;
    if (flashdisk_dirty() && lw && (now_us() - lw) > FLUSH_IDLE_MS * 1000ull) flashdisk_flush();

    if (recent && state != ST_RECEIVING) {
        prev_n = nfiles;
        for (int i = 0; i < nfiles; i++) prev_hash[i] = name_hash(files[i].name);
        gp_close();
        if (fs_mounted) { f_unmount(""); fs_mounted = false; }
        state = ST_RECEIVING;
        screen_message("RECEIVING", "Copying files...", "playback resumes when done", COL_ACC);
        printf("drive: host writing\n");
    } else if (state == ST_RECEIVING && !recent) {
        flashdisk_flush();
        // Play the newest *newly added* GIF; if nothing new arrived (e.g. macOS housekeeping
        // writes), carry on with the GIF that was playing.
        char was[NAME_LEN] = "";
        if (cur >= 0 && cur < nfiles) snprintf(was, sizeof was, "%s", files[cur].name);
        fs_mount_or_format();
        scan_files();
        int pick = -1; uint32_t newest_stamp = 0;
        for (int i = 0; i < nfiles; i++) {
            uint32_t h = name_hash(files[i].name);
            bool seen = false;
            for (int k = 0; k < prev_n; k++) if (prev_hash[k] == h) { seen = true; break; }
            if (!seen && files[i].stamp >= newest_stamp) { newest_stamp = files[i].stamp; pick = i; }
        }
        if (pick < 0 && was[0]) pick = find_file(was);
        if (pick < 0) pick = 0;
        printf("drive: settled, %d GIF(s), playing %d\n", nfiles, pick + 1);
        start_file(pick);
    }
}


// ------------------------------------------------------------------ button state
static uint32_t btn_prev;
static uint64_t press_t[BTN_COUNT];
static bool     b_long_fired, a_long_fired, ab_fired, c_long_fired;
static uint32_t swallow;             // buttons still held from the menu: ignore until released
static int      help_page;

// ------------------------------------------------------------------ menu
typedef enum { MI_GIFS, MI_HELP, MI_2P, MI_NES, MI_DOOM, MI_ORIG, MI_LINK, MI_UPDATE } mi_kind_t;
typedef struct { mi_kind_t kind; char label[30]; char path[NAME_LEN + 8]; } mitem_t;
static mitem_t mitems[MAX_FILES + 5];
static int     mcount, msel, mtop;
#define MENU_ROWS 7
#define MENU_Y0   92
#define MENU_DY   27

static void menu_add(mi_kind_t k, const char *label, const char *path) {
    if (mcount >= (int)(sizeof mitems / sizeof mitems[0])) return;
    mitem_t *it = &mitems[mcount++];
    it->kind = k;
    snprintf(it->label, sizeof it->label, "%s", label);
    for (char *p = it->label; *p; p++) *p = (char)toupper((unsigned char)*p);
    snprintf(it->path, sizeof it->path, "%s", path ? path : "");
}

static int cmp_mitem(const void *a, const void *b) { return strcasecmp(((const mitem_t *)a)->label, ((const mitem_t *)b)->label); }

// DOOM needs its game data (a WHD file made from a WAD) after the engine; the public build
// ships the engine only, so the menu offers DOOM once someone has installed their own data.
static bool doom_data_present(void) {
    const uint8_t *p = (const uint8_t *)(XIP_BASE + DOOM_WHD_OFFSET);
    return p[1] == 'W' && p[2] == 'H' && p[3] == 'D' && (p[0] == 'I' || p[0] == 'P');
}

static void menu_build(void) {
    mcount = 0;
    menu_add(MI_GIFS, "GIF PLAYER", NULL);
    menu_add(MI_HELP, "HOW TO USE", NULL);
    menu_add(MI_2P, "2 PLAYER (USB-C)", NULL);
    int first_game = mcount;
    DIR dir; FILINFO fi;
    if (fs_mounted && f_opendir(&dir, "GAMES") == FR_OK) {
        while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
            size_t n = strlen(fi.fname);
            if ((fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) || fi.fname[0] == '.' || n < 5) continue;
            if (strcasecmp(fi.fname + n - 4, ".nes") != 0) continue;
            char label[30], path[NAME_LEN + 8];
            snprintf(label, sizeof label, "%.*s", (int)(n - 4 > 26 ? 26 : n - 4), fi.fname);
            snprintf(path, sizeof path, "GAMES/%s", fi.fname);
            menu_add(MI_NES, label, path);
        }
        f_closedir(&dir);
    }
    qsort(&mitems[first_game], (size_t)(mcount - first_game), sizeof mitems[0], cmp_mitem);
    if (chain_image_present(DOOM_OFFSET) && doom_data_present()) menu_add(MI_DOOM, "DOOM", NULL);
    if (chain_image_present(ORIG_OFFSET)) menu_add(MI_ORIG, "ORIGINAL FIRMWARE", NULL);
    menu_add(MI_UPDATE, "FIRMWARE UPDATE MODE", NULL);
    if (msel >= mcount) msel = 0;
}

static void menu_draw(void) {
    lcd_blit_wait();
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COL_BG);
    text_center(34, "SC26", COL_ACC, 2);
    if (msel < mtop) mtop = msel;
    if (msel >= mtop + MENU_ROWS) mtop = msel - MENU_ROWS + 1;
    for (int r = 0; r < MENU_ROWS && mtop + r < mcount; r++) {
        const mitem_t *it = &mitems[mtop + r];
        int y = MENU_Y0 + r * MENU_DY;
        bool sel = (mtop + r) == msel;
        char line[34];
        const char *tag = it->kind == MI_NES ? "NES " : "";
        snprintf(line, sizeof line, "%s%s", tag, it->label);
        if (sel) lcd_fill_rect(50, y - 4, LCD_WIDTH - 100, MENU_DY - 2, COL_ACC);
        lcd_draw_text((LCD_WIDTH - lcd_text_width(line, 1)) / 2, y, line, sel ? COL_BG : COL_FG, sel ? COL_ACC : COL_BG, 1);
    }
    if (mtop > 0) text_center(MENU_Y0 - 22, "^", COL_DIM, 1);
    if (mtop + MENU_ROWS < mcount) text_center(MENU_Y0 + MENU_ROWS * MENU_DY - 4, "v", COL_DIM, 1);
    text_center(300, "A: START   B: BACK", COL_DIM, 1);
}

static void menu_open(void) {
    gp_close();
    state = ST_MENU;
    menu_build();
    menu_draw();
}

static void nes_progress(int pct) {
    char s[24]; snprintf(s, sizeof s, "copying %d%%", pct > 100 ? 100 : pct);
    lcd_fill_rect(60, 202, LCD_WIDTH - 120, 24, COL_BG);
    text_center(205, s, COL_DIM, 1);
    tud_task();
}

// Back to the GIF player. The button that got us here is still down: swallow it, or its
// release would act in the player (B would change the LED mode, A would pause).
static void back_to_gifs(void) {
    start_file(cur < 0 ? 0 : cur);
    swallow = buttons_read();
}

static void menu_launch(const mitem_t *it) {
    char err[48] = "";
    switch (it->kind) {
    case MI_GIFS:
        back_to_gifs();
        return;
    case MI_HELP:
        state = ST_HELP;
        help_page = 0;
        help_draw(help_page);
        return;
    case MI_NES: {
        screen_message("LOADING", it->label, "", COL_ACC);
        const uint8_t *rom = nescache_load(it->path, err, sizeof err, nes_progress);
        if (!rom) { screen_message("CAN'T START", it->label, err, RGB565(255, 90, 60)); sleep_ms(2500); menu_draw(); return; }
        led_mode_t prev = ambient_mode();
        if (prev != LEDMODE_OFF) ambient_set_mode(LEDMODE_AMBIENT);   // screen-reactive LEDs in games
        printf("nes: start %s\n", it->path);
        nes_run(rom);
        ambient_set_mode(prev);
        btn_prev = buttons_read();
        menu_draw();
        return; }
    case MI_DOOM:
    case MI_ORIG: {
        bool doom = it->kind == MI_DOOM;
        if (doom) screen_message("DOOM", "loading ...", "hold CENTER 2 s to quit", COL_ACC);
        else      screen_message("STARTING", it->label, "unplug & replug to return to SC26", COL_ACC);
        settings_flush();
        flashdisk_flush();
        tud_disconnect();                 // the other image doesn't speak USB: let the computer know
        leds_fill(0, 0, 0); leds_show(); status_led(false);
        sleep_ms(1200);
        int rc = doom ? chain_boot(DOOM_OFFSET, DOOM_SIZE) : chain_boot(ORIG_OFFSET, ORIG_SIZE);
        if (doom && rc < 0) rc = chain_boot(DOOM_OFFSET, 0x400000);   // DOOM maps the rest of its slot itself
        tud_connect();
        snprintf(err, sizeof err, "boot ROM error %d", rc);
        screen_message("CAN'T START", it->label, err, RGB565(255, 90, 60));
        sleep_ms(2500); menu_draw();
        return; }
    case MI_2P:
        twoplayer_p2_run();
        ambient_set_mode(ambient_mode());
        btn_prev = buttons_read();
        menu_draw();
        return;
    case MI_LINK:
        linktest_run(tud_task);
        ambient_set_mode(ambient_mode());       // hand the LEDs back
        btn_prev = buttons_read();
        menu_draw();
        return;
    case MI_UPDATE:
        screen_message("UPDATE MODE", "Board is now the RP2350 drive", "drop a .uf2 on it", COL_ACC);
        sleep_ms(300);
        usb_reboot_to_bootsel();
        return;
    }
}

static void menu_buttons(uint32_t b, uint32_t down) {
    if (down & BTN_UP)   { msel = (msel + mcount - 1) % mcount; menu_draw(); }
    if (down & BTN_DOWN) { msel = (msel + 1) % mcount; menu_draw(); }
    if (down & BTN_B)    { back_to_gifs(); return; }
    if (down & (BTN_A | BTN_RIGHT)) menu_launch(&mitems[msel]);
    (void)b;
}

// HOW TO USE pages: LEFT/RIGHT (or UP/DOWN) turn pages, A = next, B or CENTER = back to the menu
static void help_buttons(uint32_t down) {
    int n = help_page_count(), p = help_page;
    if (down & (BTN_B | BTN_CENTER)) { state = ST_MENU; menu_draw(); return; }
    if (down & (BTN_RIGHT | BTN_DOWN)) p = (p + 1) % n;
    if (down & (BTN_LEFT | BTN_UP))    p = (p + n - 1) % n;
    if (down & BTN_A) {
        if (p == n - 1) { state = ST_MENU; menu_draw(); return; }
        p++;
    }
    if (p != help_page) { help_page = p; help_draw(p); }
}

// ------------------------------------------------------------------ buttons

static void handle_buttons(void) {
    uint32_t raw = buttons_read();
    swallow &= raw;                         // a swallowed button counts again once released
    uint32_t b = raw & ~swallow;
    uint32_t down = b & ~btn_prev, up = ~b & btn_prev;
    uint64_t t = now_us();
    for (int i = 0; i < BTN_COUNT; i++) if (down & (1u << i)) press_t[i] = t;

    if (state == ST_MENU || state == ST_HELP) {
        if (!(b & BTN_CENTER)) c_long_fired = false;   // the hold that opened the menu is over
        if (state == ST_MENU) menu_buttons(b, down); else help_buttons(down);
        btn_prev = buttons_read() & ~swallow;
        return;
    }

    // CENTER: tap = zoom, hold 1 s = menu
    if ((b & BTN_CENTER) && !c_long_fired && t - press_t[4] > 1000000) {
        c_long_fired = true;
        menu_open();
        btn_prev = b;
        return;
    }
    if (up & BTN_CENTER) {
        bool was_long = c_long_fired;
        c_long_fired = false;
        if (!was_long && state == ST_PLAY) {
            zoom = (zoom_t)((zoom + 1) % ZOOM_COUNT);
            lcd_blit_wait();
            gp_set_zoom(zoom);
            lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COL_BG);
            next_frame_us = now_us();
            show_overlay("%s", gp_zoom_name(zoom));
            save_settings();
        }
    }

    // A+B held 2 s -> BOOTSEL
    if ((b & (BTN_A | BTN_B)) == (BTN_A | BTN_B)) {
        uint64_t since = t - (press_t[5] > press_t[6] ? press_t[5] : press_t[6]);
        if (!ab_fired && since > 2000000) {
            ab_fired = true;
            screen_message("UPDATE MODE", "Board is now the RP2350 drive", "drop a .uf2 on it", COL_ACC);
            sleep_ms(300);
            usb_reboot_to_bootsel();
        }
        btn_prev = b;
        return;
    }
    ab_fired = false;

    // A: tap = pause/resume, hold 1 s = STAY on this GIF <-> auto-advance
    if ((b & BTN_A) && !a_long_fired && t - press_t[6] > 1000000) {
        a_long_fired = true;
        stay = !stay;
        if (paused) { paused = false; next_frame_us = now_us(); }
        show_overlay("%s", stay ? "STAY ON THIS GIF" : "AUTO NEXT GIF");
        save_settings();
    }
    if ((up & BTN_A) && state == ST_PLAY) {
        if (!a_long_fired) {
            paused = !paused;
            if (paused) show_overlay("%s", "PAUSED");
            else { char s[16]; snprintf(s, sizeof s, "%.1f FPS", (double)fps); show_overlay("PLAY  %s", s); next_frame_us = now_us(); }
        }
    }
    if (up & BTN_A) a_long_fired = false;

    // B: tap = LED mode, hold = flip ring direction
    if ((b & BTN_B) && !b_long_fired && t - press_t[5] > 1500000) {
        b_long_fired = true;
        ambient_toggle_direction();
        show_overlay("LED SIDES %s", ambient_clockwise() ? "SWAPPED" : "NORMAL");
        save_settings();
    }
    if (up & BTN_B) {
        if (!b_long_fired) {
            led_mode_t m = (led_mode_t)((ambient_mode() + 1) % LEDMODE_COUNT);
            ambient_set_mode(m);
            show_overlay("LEDS: %s", ambient_mode_name(m));
            save_settings();
        }
        b_long_fired = false;
    }

    if (down & BTN_RIGHT) start_file(cur + 1);
    if (down & BTN_LEFT)  start_file(cur - 1);
    if (down & (BTN_UP | BTN_DOWN)) {
        brightness += (down & BTN_UP) ? 10 : -10;
        if (brightness > 100) brightness = 100;
        if (brightness < 10) brightness = 10;
        lcd_backlight((uint8_t)brightness);
        char s[8]; snprintf(s, sizeof s, "%d%%", brightness);
        show_overlay("BRIGHTNESS %s", s);
        save_settings();
    }
    btn_prev = b;
}

// ------------------------------------------------------------------ DOOM crash report
// The DOOM image leaves a status in the watchdog scratch registers (see sc26_doom.h in the
// DOOM port) and reboots; we turn it into a line in DOOMLOG.TXT and a BIOS line.
static uint32_t diag[8];
#define DOOM_MAILBOX 0x20078000u            // DOOM's freeze register dump (RAM we never use)
static uint32_t mbox[24], mbox_n;
static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}
static void doom_diag_capture(void) {      // before anything else touches the scratch registers
    volatile uint32_t *mb = (volatile uint32_t *)DOOM_MAILBOX;
    if (mb[0] == 0x53433244u && mb[1] <= 24) {
        uint32_t n = mb[1];
        for (uint32_t i = 0; i < n; i++) mbox[i] = mb[2 + i];
        if (mb[2 + n] == crc32_buf((const uint8_t *)mbox, n * 4)) mbox_n = n;
    }
    mb[0] = 0;
    for (int i = 0; i < 8; i++) diag[i] = watchdog_hw->scratch[i];
    watchdog_hw->scratch[0] = 0;
    for (int i = 5; i < 8; i++) watchdog_hw->scratch[i] = 0;
}
static const char *doom_string(uint32_t addr) {   // a string inside the DOOM image, as seen by DOOM
    if (addr < 0x10000000u || addr >= 0x10000000u + DOOM_SIZE - 128) return NULL;
    const char *p = (const char *)(XIP_BASE + DOOM_OFFSET + (addr - 0x10000000u));
    for (int i = 0; i < 100; i++) { if (!p[i]) return p; if ((unsigned char)p[i] < 9 || (unsigned char)p[i] > 126) return NULL; }
    return NULL;
}
static void doom_report(void) {
    uint32_t magic = diag[0] >> 24, s0 = (diag[0] >> 8) & 0xFF, s1 = (diag[0] >> 16) & 0xFF, core = diag[0] & 0xFF;
    if (magic != 0xD0 && magic != 0xDF && magic != 0xDE && magic != 0xDC && magic != 0xDA && magic != 0xDB) return;
    char line[320];
    const char *msg;
    switch (magic) {
        case 0xD0: snprintf(line, sizeof line, "HANG (watchdog) at stage %lu/%lu", (unsigned long)s0, (unsigned long)s1); break;
        case 0xDF: snprintf(line, sizeof line, "CRASH core %lu pc %08lx lr %08lx cfsr %08lx stage %lu/%lu", (unsigned long)core,
                            (unsigned long)diag[1], (unsigned long)diag[2], (unsigned long)diag[3], (unsigned long)s0, (unsigned long)s1); break;
        case 0xDA: snprintf(line, sizeof line, "quit normally (stage %lu/%lu)", (unsigned long)s0, (unsigned long)s1); break;
        case 0xDB: snprintf(line, sizeof line, "FREEZE core0 pc %08lx lr %08lx | core1 pc %08lx lr %08lx %s%s%s exc %lu stackfree %lu | "
                            "frames %lu, core1 timer %lums ago%s",
                            (unsigned long)diag[1], (unsigned long)diag[2], (unsigned long)diag[3], (unsigned long)diag[5],
                            (diag[6] >> 31) ? "nmi" : "no-nmi", ((diag[6] >> 30) & 1) ? " HALTED" : "",
                            ((diag[6] >> 29) & 1) ? " irqs-off" : "", (unsigned long)(diag[6] & 0x1FF),
                            (unsigned long)(((diag[6] >> 16) & 0xFFF) * 4), (unsigned long)(diag[7] & 0xFFFFF),
                            (unsigned long)(diag[7] >> 20), ((diag[6] >> 28) & 1) ? ", pump busy" : ""); break;
        default:
            msg = doom_string(diag[1]);
            snprintf(line, sizeof line, "%s \"%.90s\" from %08lx stage %lu/%lu", magic == 0xDC ? "PANIC" : "ERROR",
                     msg ? msg : "?", (unsigned long)diag[2], (unsigned long)s0, (unsigned long)s1);
    }
    size_t n = strlen(line);
    snprintf(line + n, sizeof line - n, "  [%08lx %08lx %08lx %08lx | %08lx %08lx %08lx]", (unsigned long)diag[0],
             (unsigned long)diag[1], (unsigned long)diag[2], (unsigned long)diag[3], (unsigned long)diag[5],
             (unsigned long)diag[6], (unsigned long)diag[7]);
    FIL f; UINT bw;
    if (f_open(&f, "DOOMLOG.TXT", FA_OPEN_APPEND | FA_WRITE) == FR_OK) {
        f_write(&f, line, (UINT)strlen(line), &bw); f_write(&f, "\r\n", 2, &bw);
        if (mbox_n && magic == 0xDB) {
            char hx[12];
            f_write(&f, "  regs:", 7, &bw);
            for (uint32_t i = 0; i < mbox_n; i++) { snprintf(hx, sizeof hx, " %08lx", (unsigned long)mbox[i]); f_write(&f, hx, 9, &bw); }
            f_write(&f, "\r\n", 2, &bw);
        }
        f_close(&f);
        flashdisk_flush();
    }
    if (magic != 0xDA) bios_printf_pause(600, "  DOOM: %s", (magic == 0xD0 || magic == 0xDB) ? "froze" : magic == 0xDF ? "crashed" : "error");
}

// ------------------------------------------------------------------ install check
// SYSINFO.TXT on the drive lists a CRC of each flash slot so an install can be verified
// from the computer. Written only when the file is missing (e.g. right after a fresh install).
static uint32_t crc_tab[256];
static uint32_t flash_crc(uint32_t off, uint32_t len) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); crc_tab[i] = c; }
    const uint8_t *p = (const uint8_t *)(XIP_BASE + off);
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void write_sysinfo(void) {
    FILINFO fi;
    if (f_stat("SYSINFO.TXT", &fi) == FR_OK) return;
    static const struct { const char *name; uint32_t off, len; } slots[] = {
        { "OS      ", OS_OFFSET, 0x43000 },
        { "WAYFARER", ORIG_OFFSET, 0x176b00 },
        { "DOOM    ", DOOM_OFFSET, 0x40900 },
        { "DOOMWHD ", DOOM_WHD_OFFSET, 0x5b3100 },
    };
    char buf[400]; int n = snprintf(buf, sizeof buf, "SC26 OS v2.1 install check (CRC32 of each flash slot)\r\n");
    for (size_t i = 0; i < sizeof slots / sizeof slots[0]; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s 0x%06lx +0x%06lx  %08lx\r\n", slots[i].name,
                      (unsigned long)slots[i].off, (unsigned long)slots[i].len, (unsigned long)flash_crc(slots[i].off, slots[i].len));
    FIL f; UINT bw;
    if (f_open(&f, "SYSINFO.TXT", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) { f_write(&f, buf, (UINT)n, &bw); f_close(&f); }
    flashdisk_flush();
}

// ------------------------------------------------------------------ main loop
static void service(void) {
    tud_task();
    ambient_tick();
}

// PLAYER 1 of a USB LINK: the badge was restarted to run its USB port as a host. Skip the
// normal OS (no USB drive in this mode) and go back to it with a restart when done.
static void usb_host_link_boot(void) {
    settings_t cfg;
    bool have_cfg = settings_load(&cfg);
    buttons_init();
    lcd_init_ex(1.0f);
    lcd_backlight(have_cfg && cfg.brightness >= 10 && cfg.brightness <= 100 ? cfg.brightness : 100);
    ambient_init();
    ambient_set_mode(have_cfg && cfg.led_mode < LEDMODE_COUNT ? (led_mode_t)cfg.led_mode : LEDMODE_ORIGINAL);
    ambient_set_clockwise(have_cfg ? cfg.ring_cw : false);
    tusb_rhport_init_t host = { .role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_FULL };
    tusb_rhport_init(0, &host);
    fs_mount_or_format();                   // PLAYER 1 picks games from its GAMES folder
    twoplayer_p1_run();
    flashdisk_flush();
    sleep_ms(300);
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
}

int main(void) {
    if (watchdog_hw->scratch[0] == LINKTEST_HOST_MAGIC) { watchdog_hw->scratch[0] = 0; usb_host_link_boot(); }
    doom_diag_capture();
    watchdog_disable();                     // DOOM runs with a watchdog; don't let it follow us here
    settings_t cfg;
    bool have_cfg = settings_load(&cfg);
    if (have_cfg) {
        stay = cfg.stay;
        brightness = (cfg.brightness >= 10 && cfg.brightness <= 100) ? cfg.brightness : 100;
        zoom = cfg.zoom < ZOOM_COUNT ? (zoom_t)cfg.zoom : ZOOM_AUTO;
    }
    buttons_init();
    lcd_init_ex(1.0f);                // 75 MHz QSPI, same as the original firmware
    lcd_backlight(brightness);
    ambient_init();
    ambient_set_mode(have_cfg && cfg.led_mode < LEDMODE_COUNT ? (led_mode_t)cfg.led_mode : LEDMODE_ORIGINAL);
    ambient_set_clockwise(have_cfg ? cfg.ring_cw : false);   // SAINTCON rocket: LEDs 8,0,1,2 run down the LEFT side

    // Power-up: BIOS-style POST with live info, then the animated splash (boot_splash.c)
    bios_begin();
    bios_print("Detecting drives ...", 120);
    status_led(true);
    bool fs_ok = fs_mount_or_format();      // before USB: the host must never see a half-made drive
    status_led(false);
    if (fs_ok) {
        write_sysinfo();
        scan_files();
        doom_report();
        menu_build();
        int games = 0;
        for (int i = 0; i < mcount; i++) games += (mitems[i].kind == MI_NES || mitems[i].kind == MI_DOOM);
        DWORD fre; FATFS *pfs;
        uint32_t kb = f_getfree("", &fre, &pfs) == FR_OK ? (uint32_t)((pfs->n_fatent - 2) * pfs->csize / 2) : 0;
        bios_printf_pause(120, "  SC26 GIFS   %lu.%lu MB", (unsigned long)(kb / 1024), (unsigned long)(kb % 1024 * 10 / 1024));
        bios_printf_pause(120, "  %d GIF%s  %d GAME%s", nfiles, nfiles == 1 ? "" : "S", games, games == 1 ? "" : "S");
        bios_print("Booting SC26 OS ...", 0);
    } else {
        bios_print("  DRIVE ERROR", 0);
    }
    usb_glue_init();
    if (!fs_ok) {
        screen_message("STORAGE ERROR", "Could not mount the GIF drive", NULL, RGB565(255, 90, 60));
        while (true) { service(); if (usb_bootsel_requested()) usb_reboot_to_bootsel(); }
    }
    bios_wait(500, tud_task);
    boot_splash(tud_task, (uint8_t)brightness);
    ambient_set_mode(ambient_mode());       // hand the LED ring back to the chosen mode
    int first = have_cfg && cfg.gif[0] ? find_file(cfg.gif) : -1;
    start_file(first < 0 ? 0 : first);
    if (state == ST_PLAY) {                 // first GIF after power-up: point at the menu / help
        show_overlay("%s", "HOLD CENTER: MENU/HELP");
        overlay_until_us = now_us() + 5000000;
    }

    uint64_t next_poll = 0;
    while (true) {
        service();
        if (usb_bootsel_requested()) usb_reboot_to_bootsel();
        if (now_us() >= next_poll) {
            next_poll = now_us() + 5000;
            handle_buttons();
            drive_state();
            settings_poll(state != ST_RECEIVING && !flashdisk_dirty());
        }
        if (state == ST_PLAY) {
            lcd_blit_wait();
            play_step();
        } else if (state == ST_ERROR && nfiles > 1 && now_us() - play_started_us > 3000000) {
            start_file(cur + 1);
        }
        if (overlay_on_screen && state == ST_PLAY && paused && now_us() >= overlay_until_us) restore_overlay_area();
    }
}
