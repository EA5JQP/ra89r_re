#include "driver/lcd_st7565.h"

#ifndef LCD_HOST_TEST
#include "board.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#endif

uint8_t lcd_fb[LCD_PAGES][LCD_WIDTH];

#ifndef LCD_HOST_TEST
/* ------------------------------------------------------------------------ */
/* bit-banged 4-wire interface (target build only)                          */
/* ------------------------------------------------------------------------ */

static void lcd_byte(uint8_t value)
{
    int i;

    gpio_clear(LCD_CTRL_PORT, LCD_CS_PIN);
    for (i = 0; i < 8; i++) {
        gpio_clear(LCD_CTRL_PORT, LCD_SCK_PIN);
        gpio_write(LCD_DATA_PORT, LCD_DATA_PIN, (value & 0x80u) ? 1 : 0);
        gpio_set(LCD_CTRL_PORT, LCD_SCK_PIN);
        value <<= 1;
    }
    gpio_clear(LCD_CTRL_PORT, LCD_SCK_PIN);
    gpio_set(LCD_CTRL_PORT, LCD_CS_PIN);
}

void lcd_write_cmd(uint8_t cmd)
{
    gpio_clear(LCD_CTRL_PORT, LCD_DC_PIN);      /* A0 = 0 -> command */
    lcd_byte(cmd);
}

void lcd_write_data(uint8_t data)
{
    gpio_set(LCD_CTRL_PORT, LCD_DC_PIN);        /* A0 = 1 -> data */
    lcd_byte(data);
}

/* ------------------------------------------------------------------------ */
/* init                                                                    */
/* ------------------------------------------------------------------------ */

static int s_variant = LCD_INIT_STOCK;

int lcd_variant(void)
{
    return s_variant;
}

int lcd_reinit(int variant)
{
    if (variant != LCD_INIT_SIMPLE)
        variant = LCD_INIT_STOCK;
    s_variant = variant;

    gpio_config_output(LCD_CTRL_PORT, LCD_SCK_PIN | LCD_DC_PIN | LCD_CS_PIN | LCD_RST_PIN);
    gpio_config_output(LCD_DATA_PORT, LCD_DATA_PIN);

    gpio_set(LCD_CTRL_PORT, LCD_CS_PIN);
    gpio_set(LCD_CTRL_PORT, LCD_DC_PIN);

    /* hardware reset, as the stock firmware does it */
    gpio_set(LCD_CTRL_PORT, LCD_RST_PIN);
    gpio_clear(LCD_CTRL_PORT, LCD_RST_PIN);
    systick_delay_ms(10);
    gpio_set(LCD_CTRL_PORT, LCD_RST_PIN);
    systick_delay_ms(10);

    lcd_write_cmd(0xE2);            /* software reset */
    systick_delay_ms(10);
    lcd_write_cmd(0xA2);            /* bias select 1/9 */
    lcd_write_cmd(0xA1);            /* SEG direction = reverse */
    lcd_write_cmd(0xC0);            /* COM direction = normal */
    lcd_write_cmd(0xA6);            /* inverse display off */
    lcd_write_cmd(0xF8);            /* booster ratio */
    lcd_write_cmd(0x01);
    lcd_write_cmd(0x2F);            /* power circuit on (VB/VR/VF) */
    lcd_write_cmd(0x25);            /* regulation ratio */
    lcd_write_cmd(0x81);            /* electronic volume (contrast) */
    lcd_write_cmd(0x19);
    if (variant == LCD_INIT_STOCK) {
        /* The stock firmware also sends these eight bytes.  They are not part of
         * the standard ST7565 command set; the panel fitted in the RA89R
         * evidently accepts them, so they are replayed for bring-up parity.
         * `s` over the console re-inits without them. */
        lcd_write_cmd(0xFF);
        lcd_write_cmd(0x64);
        lcd_write_cmd(0x72);
        lcd_write_cmd(0xB4);
        lcd_write_cmd(0x90);
        lcd_write_cmd(0x98);
        lcd_write_cmd(0x70);
        lcd_write_cmd(0xFE);
    }
    lcd_write_cmd(0x40);            /* display start line = 0 */
    lcd_write_cmd(0xAF);            /* display on */
    systick_delay_ms(10);
    return s_variant;
}

void lcd_init(void)
{
    (void)lcd_reinit(LCD_INIT_STOCK);
}

void lcd_set_addr(uint8_t page, uint8_t column)
{
    uint16_t col = (uint16_t)column + LCD_COLUMN_OFFSET;

    lcd_write_cmd((uint8_t)(0xB0u | (page & 0x0Fu)));
    lcd_write_cmd((uint8_t)(0x10u | ((col >> 4) & 0x0Fu)));
    lcd_write_cmd((uint8_t)(col & 0x0Fu));
}

void lcd_refresh(void)
{
    uint8_t page;
    uint8_t col;

    for (page = 0; page < LCD_PAGES; page++) {
        lcd_set_addr(page, 0);
        for (col = 0; col < LCD_WIDTH; col++)
            lcd_write_data(lcd_fb[page][col]);
    }
}

#endif /* !LCD_HOST_TEST */

/* ------------------------------------------------------------------------ */
/* framebuffer (hardware independent)                                      */
/* ------------------------------------------------------------------------ */

void lcd_fb_clear(uint8_t pattern)
{
    uint8_t page;
    uint8_t col;

    for (page = 0; page < LCD_PAGES; page++)
        for (col = 0; col < LCD_WIDTH; col++)
            lcd_fb[page][col] = pattern;
}

void lcd_fb_pixel(uint16_t x, uint16_t y, int on)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;

    if (on)
        lcd_fb[y >> 3][x] |= (uint8_t)(1u << (y & 7u));
    else
        lcd_fb[y >> 3][x] &= (uint8_t)~(1u << (y & 7u));
}

void lcd_fb_hline(uint16_t x0, uint16_t x1, uint16_t y, int on)
{
    uint16_t x;

    for (x = x0; x <= x1; x++)
        lcd_fb_pixel(x, y, on);
}

void lcd_fb_vline(uint16_t x, uint16_t y0, uint16_t y1, int on)
{
    uint16_t y;

    for (y = y0; y <= y1; y++)
        lcd_fb_pixel(x, y, on);
}

void lcd_fb_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, int on, int filled)
{
    uint16_t y;

    if (x1 < x0 || y1 < y0)
        return;

    if (filled) {
        for (y = y0; y <= y1; y++)
            lcd_fb_hline(x0, x1, y, on);
        return;
    }
    lcd_fb_hline(x0, x1, y0, on);
    lcd_fb_hline(x0, x1, y1, on);
    lcd_fb_vline(x0, y0, y1, on);
    lcd_fb_vline(x1, y0, y1, on);
}

void lcd_fb_invert_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint16_t x;
    uint16_t y;

    for (y = y0; y <= y1 && y < LCD_HEIGHT; y++)
        for (x = x0; x <= x1 && x < LCD_WIDTH; x++)
            lcd_fb[y >> 3][x] ^= (uint8_t)(1u << (y & 7u));
}

/* Draw one glyph.  The glyph is stored column-major/page-major, so each
 * column byte holds 8 vertical pixels of one page; when y is not page
 * aligned the bits are split across two pages. */
void lcd_fb_glyph(uint16_t x, uint16_t y, const font_t *font, uint32_t ch)
{
    uint8_t p;
    uint8_t c;

    if (ch < font->first || ch > font->last)
        return;

    {
        const uint8_t *glyph = font->glyphs + (ch - font->first) * font->bytes;
        uint8_t shift = (uint8_t)(y & 7u);
        uint8_t page = (uint8_t)(y >> 3);

        for (p = 0; p < font->pages; p++) {
            for (c = 0; c < font->stride; c++) {
                uint16_t px = (uint16_t)(x + c);
                uint8_t bits = glyph[p * font->stride + c];

                if (px >= LCD_WIDTH)
                    continue;
                if (page + p < LCD_PAGES)
                    lcd_fb[page + p][px] |= (uint8_t)(bits << shift);
                if (shift && (uint8_t)(page + p + 1u) < LCD_PAGES)
                    lcd_fb[page + p + 1][px] |= (uint8_t)(bits >> (8u - shift));
            }
        }
    }
}

uint16_t lcd_fb_text(uint16_t x, uint16_t y, const font_t *font, const char *s)
{
    while (*s) {
        uint32_t ch = (uint8_t)*s++;

        if (ch == ' ') {
            x = (uint16_t)(x + font->stride + 2u);
            continue;
        }
        /* skip control characters rather than drawing garbage */
        if (ch < 0x20u)
            continue;

        lcd_fb_glyph(x, y, font, ch);
        x = (uint16_t)(x + font->stride + 2u);
        if (x >= LCD_WIDTH)
            break;
    }
    return x;
}
