// Host stand-in for the badge LCD: draws into a 360x360 RGB565 buffer so help.c can be
// previewed and checked on a PC (see tools/preview_help.sh).
#include <stdio.h>
#include <string.h>
#include "lcd.h"
#include "font.h"
#include "help.h"

static uint16_t fb[LCD_HEIGHT][LCD_WIDTH];

void lcd_blit_wait(void) {}
void lcd_fill_rect(int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++)
        if (i >= 0 && j >= 0 && i < LCD_WIDTH && j < LCD_HEIGHT) fb[j][i] = c;
}
int lcd_text_width(const char *s, int scale) { return (int)strlen(s) * FONT_W * scale; }
int lcd_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale) {
    int cw = FONT_W * scale, ch = FONT_H * scale;
    for (; *s; s++, x += cw) {
        unsigned c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const uint16_t *g = font_glyphs[c - 32];
        for (int r = 0; r < ch; r++) for (int col = 0; col < cw; col++)
            lcd_fill_rect(x + col, y + r, 1, 1, (g[r / scale] & (0x8000u >> (col / scale))) ? fg : bg);
    }
    return x;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    int bad = 0;
    for (int p = 0; p < help_page_count(); p++) {
        memset(fb, 0, sizeof fb);
        help_draw(p);
        char path[256]; snprintf(path, sizeof path, "%s/help_%d.ppm", dir, p);
        FILE *f = fopen(path, "wb");
        fprintf(f, "P6 %d %d 255\n", LCD_WIDTH, LCD_HEIGHT);
        int out = 0;
        for (int y = 0; y < LCD_HEIGHT; y++) for (int x = 0; x < LCD_WIDTH; x++) {
            uint16_t c = fb[y][x];
            unsigned char rgb[3] = { (unsigned char)((c >> 8) & 0xF8), (unsigned char)((c >> 3) & 0xFC), (unsigned char)((c << 3) & 0xF8) };
            fwrite(rgb, 1, 3, f);
            float dx = x + 0.5f - 180, dy = y + 0.5f - 180;
            if (c && dx * dx + dy * dy > 176.0f * 176.0f) out++;     // keep 4 px clear of the bezel
        }
        fclose(f);
        if (out) { printf("page %d: %d pixels outside the circle\n", p, out); bad = 1; }
    }
    printf("%d pages%s\n", help_page_count(), bad ? "  (OVERFLOW)" : ", all inside the circle");
    return bad;
}
