// ST77916 QSPI LCD driver for the SC26 board.
//
// Wire protocol (matches the original firmware):
//   register write : CS low, single-line {0x02, 0x00, cmd, 0x00}, single-line params, CS high
//   pixel write    : CS low, single-line {0x32, 0x00, 0x2C, 0x00}, quad-line RGB565 bytes, CS high
// "Single-line" bytes go through the same 4-bit PIO program with each bit expanded onto D0.

#include "lcd.h"
#include "st77916_init.h"
#include "font.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "qspi_lcd.pio.h"
#include <string.h>

static PIO  lcd_pio = pio1;
static uint lcd_sm;
static int  lcd_dma = -1;
static volatile bool blit_active;
static volatile bool streaming;        // inside lcd_stream_begin/end: keep CS low between DMA chunks

static inline void cs_low(void)  { gpio_put(PIN_LCD_CS, 0); }
static inline void cs_high(void) { gpio_put(PIN_LCD_CS, 1); }

static inline void put_raw(uint8_t b) {
    pio_sm_put_blocking(lcd_pio, lcd_sm, (uint32_t)b << 24);
}

// One byte on D0 only: 4 FIFO bytes, each carrying two bits (one per nibble).
static void put_single(uint8_t b) {
    for (int i = 7; i > 0; i -= 2)
        put_raw((uint8_t)((((b >> i) & 1) << 4) | ((b >> (i - 1)) & 1)));
}

// Wait until every queued bit has been clocked out.
static void wait_idle(void) {
    uint32_t stall = 1u << (PIO_FDEBUG_TXSTALL_LSB + lcd_sm);
    while (!pio_sm_is_tx_fifo_empty(lcd_pio, lcd_sm)) tight_loop_contents();
    lcd_pio->fdebug = stall;
    while (!(lcd_pio->fdebug & stall)) tight_loop_contents();
}

void lcd_send_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    lcd_blit_wait();
    cs_low();
    put_single(0x02); put_single(0x00); put_single(cmd); put_single(0x00);
    for (size_t i = 0; i < len; i++) put_single(data[i]);
    wait_idle();
    cs_high();
}

static void set_window(int x0, int y0, int x1, int y1) {
    uint8_t ca[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t ra[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    lcd_send_cmd(0x2A, ca, 4);
    lcd_send_cmd(0x2B, ra, 4);
}

static void begin_pixels(int x, int y, int w, int h) {
    set_window(x, y, x + w - 1, y + h - 1);
    cs_low();
    put_single(0x32); put_single(0x00); put_single(0x2C); put_single(0x00);
}

static bool clip(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > LCD_WIDTH)  *w = LCD_WIDTH - *x;
    if (*y + *h > LCD_HEIGHT) *h = LCD_HEIGHT - *y;
    return *w > 0 && *h > 0;
}

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!clip(&x, &y, &w, &h)) return;
    begin_pixels(x, y, w, h);
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int n = w * h; n > 0; n--) { put_raw(hi); put_raw(lo); }
    wait_idle();
    cs_high();
}

static void dma_irq_handler(void) {
    if (lcd_dma >= 0 && dma_channel_get_irq1_status(lcd_dma)) {
        dma_channel_acknowledge_irq1(lcd_dma);
        if (!streaming) {
            wait_idle();      // at most 8 FIFO bytes left: a few hundred ns
            cs_high();
        }
        blit_active = false;
    }
}

void lcd_blit_async(int x, int y, int w, int h, const uint16_t *pixels_be) {
    lcd_blit_wait();
    begin_pixels(x, y, w, h);
    blit_active = true;
    dma_channel_set_read_addr(lcd_dma, pixels_be, false);
    dma_channel_set_trans_count(lcd_dma, (uint32_t)w * h * 2, true);
}

bool lcd_blit_busy(void) { return blit_active; }

// ---- streaming: one pixel-write transaction fed in chunks (e.g. one emulator scanline at a time)
void lcd_stream_begin(int x, int y, int w, int h) {
    lcd_blit_wait();
    begin_pixels(x, y, w, h);
    streaming = true;
}

void lcd_stream_push(const void *data_be, size_t bytes) {
    while (blit_active) tight_loop_contents();     // one chunk in flight at a time
    blit_active = true;
    dma_channel_set_read_addr(lcd_dma, data_be, false);
    dma_channel_set_trans_count(lcd_dma, (uint32_t)bytes, true);
}

void lcd_stream_end(void) {
    while (blit_active) tight_loop_contents();
    wait_idle();
    cs_high();
    streaming = false;
}
void lcd_blit_wait(void) { while (blit_active) tight_loop_contents(); }

void lcd_blit(int x, int y, int w, int h, const uint16_t *pixels_be) {
    lcd_blit_async(x, y, w, h, pixels_be);
    lcd_blit_wait();
}

void lcd_backlight(uint8_t percent) {
    if (percent > 100) percent = 100;
    pwm_set_gpio_level(PIN_LCD_BL, percent * 10);   // TOP = 1000, like the original
}

void lcd_set_rotation(uint16_t deg) {
    uint8_t madctl = deg == 90 ? 0x60 : deg == 180 ? 0xC0 : deg == 270 ? 0xA0 : 0x00;
    lcd_send_cmd(0x36, &madctl, 1);
}

bool lcd_wait_te(uint32_t timeout_us) {
    // Wait for a rising edge on the panel's tearing-effect line (start of vertical blank).
    absolute_time_t t_end = make_timeout_time_us(timeout_us);
    while (gpio_get(PIN_LCD_TE)) if (time_reached(t_end)) return false;
    while (!gpio_get(PIN_LCD_TE)) if (time_reached(t_end)) return false;
    return true;
}

void lcd_init(void) { lcd_init_ex(2.0f); }

void lcd_init_ex(float pio_clkdiv) {
    // Backlight PWM: 150 MHz / 15 / 1000 = 10 kHz (same as original). Start dark.
    gpio_set_function(PIN_LCD_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_LCD_BL);
    pwm_config pc = pwm_get_default_config();
    pwm_config_set_clkdiv(&pc, 15.f);
    pwm_config_set_wrap(&pc, 999);
    pwm_init(slice, &pc, true);
    lcd_backlight(0);

    gpio_init(PIN_LCD_CS);  gpio_set_dir(PIN_LCD_CS, GPIO_OUT);  gpio_put(PIN_LCD_CS, 1);
    gpio_init(PIN_LCD_RST); gpio_set_dir(PIN_LCD_RST, GPIO_OUT); gpio_put(PIN_LCD_RST, 1);
    gpio_init(PIN_LCD_TE);  gpio_set_dir(PIN_LCD_TE, GPIO_IN);   gpio_pull_up(PIN_LCD_TE);

    uint offset = pio_add_program(lcd_pio, &qspi_lcd_program);
    lcd_sm = pio_claim_unused_sm(lcd_pio, true);
    // SCK = sys_clk / (2 * clkdiv): clkdiv 2 -> 37.5 MHz (~14 ms/frame), 1 -> 75 MHz like the original (~7 ms).
    qspi_lcd_program_init(lcd_pio, lcd_sm, offset, PIN_LCD_SCK, PIN_LCD_D0, pio_clkdiv);

    lcd_dma = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(lcd_dma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(lcd_pio, lcd_sm, true));
    dma_channel_configure(lcd_dma, &dc, &lcd_pio->txf[lcd_sm], NULL, 0, false);
    dma_channel_set_irq1_enabled(lcd_dma, true);
    irq_add_shared_handler(DMA_IRQ_1, dma_irq_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);

    // Hardware reset
    gpio_put(PIN_LCD_RST, 1); sleep_ms(10);
    gpio_put(PIN_LCD_RST, 0); sleep_ms(10);
    gpio_put(PIN_LCD_RST, 1); sleep_ms(120);

    for (size_t i = 0; i < ST77916_INIT_SEQ_LEN; i++) {
        const st77916_init_cmd_t *c = &st77916_init_seq[i];
        lcd_send_cmd(c->cmd, c->data, c->len);
        if (c->delay_ms) sleep_ms(c->delay_ms);
    }
    uint8_t te_on = 0x00;
    lcd_send_cmd(0x35, &te_on, 1);       // tearing-effect output on (V-blank only)
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0x0000);
    lcd_backlight(100);
}

// ---- text -------------------------------------------------------------------
int lcd_text_width(const char *s, int scale) { return (int)strlen(s) * FONT_W * scale; }

int lcd_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale) {
    static uint16_t buf[FONT_W * 4 * FONT_H * 4];
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    uint16_t fgb = BSWAP16(fg), bgb = BSWAP16(bg);
    int cw = FONT_W * scale, ch = FONT_H * scale;
    for (; *s; s++, x += cw) {
        unsigned c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const uint16_t *g = font_glyphs[c - 32];
        lcd_blit_wait();
        for (int r = 0; r < ch; r++)
            for (int col = 0; col < cw; col++)
                buf[r * cw + col] = (g[r / scale] & (0x8000u >> (col / scale))) ? fgb : bgb;
        lcd_blit(x, y, cw, ch, buf);
    }
    return x;
}
