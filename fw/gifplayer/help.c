// SC26 OS "HOW TO USE" screens (menu -> HOW TO USE). LEFT/RIGHT flip pages, B goes back.
//
// Each page is a title, an optional subtitle and a list of rows:
//   'K' key row: a button "chip" on the left, what it does on the right. In the key text,
//       '|' starts a modifier drawn in amber, e.g. "CENTER|hold" -> CENTER hold.
//   'T' centred text, 'N' centred note (dimmer), 'G' small gap.
// Everything is laid out to stay inside the round 360x360 panel (tools/preview_help.sh
// renders the pages on the host and checks that).
#include <string.h>
#include "lcd.h"
#include "help.h"

#define C_BG     RGB565(0, 0, 0)
#define C_TITLE  RGB565(60, 230, 120)
#define C_SUB    RGB565(140, 150, 170)
#define C_TEXT   RGB565(235, 235, 235)
#define C_NOTE   RGB565(140, 150, 170)
#define C_CHIP   RGB565(36, 46, 64)
#define C_KEY    RGB565(130, 255, 170)
#define C_HOLD   RGB565(255, 190, 80)
#define C_DOT    RGB565(70, 80, 100)

typedef struct { char kind; const char *key, *text; } hrow_t;
typedef struct { const char *title, *sub; hrow_t rows[10]; } hpage_t;

static const hpage_t pages[] = {
    { "HOW TO USE", NULL, {
        {'T', 0, "Joystick: UP DOWN"},
        {'T', 0, "LEFT RIGHT, push it"},
        {'T', 0, "in for CENTER."},
        {'T', 0, "Plus the A and B buttons."},
        {'G'},
        {'K', "A", "press and let go"},
        {'K', "A|hold", "keep pressed 1-2 s"},
        {'G'},
        {'N', 0, "LEFT / RIGHT: next page"},
    } },
    { "GIF PLAYER", "joystick", {
        {'K', "LEFT", "previous GIF"},
        {'K', "RIGHT", "next GIF"},
        {'K', "UP/DOWN", "brightness"},
        {'K', "CENTER", "zoom / fit"},
        {'K', "CENTER|hold", "MENU"},
        {'G'},
        {'N', 0, "zoom: auto, circle,"},
        {'N', 0, "square, fill"},
    } },
    { "GIF PLAYER", "A and B", {
        {'K', "A", "pause / play"},
        {'K', "A|hold", "stay on this GIF"},
        {'K', "B", "LED mode"},
        {'K', "B|hold", "swap LED sides"},
        {'K', "A+B|hold", "update mode"},
        {'G'},
        {'N', 0, "LEDs: original, ambient,"},
        {'N', 0, "scenes or off. Menu ->"},
        {'N', 0, "LED SCENES to pick one"},
    } },
    { "MENU", NULL, {
        {'K', "CENTER|hold", "open the menu"},
        {'K', "UP/DOWN", "choose"},
        {'K', "A or RIGHT", "start"},
        {'K', "B", "back to GIFs"},
        {'G'},
        {'N', 0, "GIFs, help, NES games,"},
        {'N', 0, "DOOM, original firmware,"},
        {'N', 0, "firmware update"},
    } },
    { "NES GAMES", NULL, {
        {'K', "JOYSTICK", "D-pad"},
        {'K', "A/B", "A / B"},
        {'K', "CENTER", "START"},
        {'K', "CENTER+DOWN", "SELECT"},
        {'K', "CENTER|hold", "quit to menu"},
        {'G'},
        {'N', 0, "Add .nes files to the"},
        {'N', 0, "GAMES folder on the drive"},
    } },
    { "DOOM", NULL, {
        {'K', "UP/DOWN", "walk"},
        {'K', "LEFT/RIGHT", "turn"},
        {'K', "A", "fire"},
        {'K', "B", "use / open"},
        {'K', "CENTER", "game menu"},
        {'K', "CENTER|hold", "quit DOOM"},
    } },
    { "DOOM", "hold CENTER and press", {
        {'K', "LEFT/RIGHT", "strafe"},
        {'K', "UP", "next weapon"},
        {'K', "DOWN", "map"},
        {'K', "B", "run on / off"},
        {'G'},
        {'N', 0, "In menus: A = OK / yes,"},
        {'N', 0, "B = back / no"},
    } },
    { "ADD GIFS", NULL, {
        {'T', 0, "Plug the badge into"},
        {'T', 0, "a computer and copy"},
        {'T', 0, ".gif files onto the"},
        {'T', 0, "SC26 GIFS drive."},
        {'G'},
        {'N', 0, "The new GIF plays when"},
        {'N', 0, "the copy is done."},
        {'N', 0, "Up to 1024 px wide."},
    } },
    { "UPDATE", NULL, {
        {'T', 0, "Hold A + B for 2 s"},
        {'T', 0, "(or MENU > FIRMWARE"},
        {'T', 0, "UPDATE MODE), then drop"},
        {'T', 0, "sc26_os_update.uf2"},
        {'T', 0, "on the RP2350 drive."},
        {'G'},
        {'N', 0, "GIFs, games and"},
        {'N', 0, "saves are kept."},
    } },
};
#define NPAGES ((int)(sizeof pages / sizeof pages[0]))

#define ROW_H    24
#define GAP_H    10
#define CHIP_PAD 5
#define COL_GAP  12
#define BODY_Y0  92        // first row when there is no subtitle
#define BODY_Y1  282       // rows end above the page dots

int help_page_count(void) { return NPAGES; }

static int nrows(const hpage_t *p) {
    int n = 0;
    while (n < (int)(sizeof p->rows / sizeof p->rows[0]) && p->rows[n].kind) n++;
    return n;
}

// width of a key label in pixels, '|' replaced by a space (or dropped when it starts the label)
static int key_w(const char *k) {
    const char *bar = strchr(k, '|');
    int n = (int)strlen(k);
    if (bar == k) n--;                    // "|hold": no space before the modifier
    return n * 10;
}

static void center(int y, const char *s, uint16_t fg) {
    lcd_draw_text((LCD_WIDTH - lcd_text_width(s, 1)) / 2, y, s, fg, C_BG, 1);
}

static void chip(int x, int y, int w, const char *k) {
    lcd_fill_rect(x, y - 1, w, 21, C_CHIP);
    lcd_fill_rect(x, y - 1, 1, 1, C_BG);           lcd_fill_rect(x + w - 1, y - 1, 1, 1, C_BG);
    lcd_fill_rect(x, y + 19, 1, 1, C_BG);          lcd_fill_rect(x + w - 1, y + 19, 1, 1, C_BG);
    char main_part[24];
    const char *bar = strchr(k, '|');
    size_t n = bar ? (size_t)(bar - k) : strlen(k);
    if (n >= sizeof main_part) n = sizeof main_part - 1;
    memcpy(main_part, k, n); main_part[n] = 0;
    int tx = x + CHIP_PAD;
    if (n) tx = lcd_draw_text(tx, y, main_part, C_KEY, C_CHIP, 1);
    if (bar) {
        if (n) tx = lcd_draw_text(tx, y, " ", C_HOLD, C_CHIP, 1);
        lcd_draw_text(tx, y, bar + 1, C_HOLD, C_CHIP, 1);
    }
}

void help_draw(int page) {
    if (page < 0) page = 0;
    if (page >= NPAGES) page = NPAGES - 1;
    const hpage_t *p = &pages[page];
    int n = nrows(p);

    lcd_blit_wait();
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    lcd_draw_text((LCD_WIDTH - lcd_text_width(p->title, 2)) / 2, 40, p->title, C_TITLE, C_BG, 2);
    int top = BODY_Y0;
    if (p->sub) { center(82, p->sub, C_SUB); top = BODY_Y0 + 18; }

    // column widths for the key rows, so the chips and descriptions line up
    int kw = 0, tw = 0, h = 0;
    for (int i = 0; i < n; i++) {
        const hrow_t *r = &p->rows[i];
        h += r->kind == 'G' ? GAP_H : ROW_H;
        if (r->kind != 'K') continue;
        int a = key_w(r->key) + 2 * CHIP_PAD, b = lcd_text_width(r->text, 1);
        if (a > kw) kw = a;
        if (b > tw) tw = b;
    }
    int x0 = (LCD_WIDTH - (kw + COL_GAP + tw)) / 2;
    int y = top + (BODY_Y1 - top - h) / 2;
    if (y < top) y = top;

    for (int i = 0; i < n; i++) {
        const hrow_t *r = &p->rows[i];
        switch (r->kind) {
        case 'K': {
            int w = key_w(r->key) + 2 * CHIP_PAD;
            chip(x0 + kw - w, y, w, r->key);          // chips right-aligned against the gap
            lcd_draw_text(x0 + kw + COL_GAP, y, r->text, C_TEXT, C_BG, 1);
            break; }
        case 'T': center(y, r->text, C_TEXT); break;
        case 'N': center(y, r->text, C_NOTE); break;
        default: break;
        }
        y += r->kind == 'G' ? GAP_H : ROW_H;
    }

    // page dots + how to get around
    int dx = 12, dw = NPAGES * dx - 4, x = (LCD_WIDTH - dw) / 2;
    for (int i = 0; i < NPAGES; i++, x += dx) {
        if (i == page) { lcd_fill_rect(x - 1, 289, 10, 10, C_TITLE); }
        else           { lcd_fill_rect(x + 1, 291, 6, 6, C_DOT); }
    }
    center(308, "L/R: PAGE   B: BACK", C_SUB);
}
