/* Bit-banged ST7565-family 128x64 LCD driver for the RA89R.
 *
 * Everything here mirrors the stock firmware (addresses in ra89r_lcd.md):
 *   init sequence   0x08014F42      (replayed verbatim)
 *   byte writer     0x08015004      (CS low, 8 bits MSB first, SCLK pulse)
 *   command/data    0x08015080 / 0x080150A6 (A0/DC pin)
 *   addressing      0x0800F698 / 0x0801C9A4 (page 0xB0|n, column 0x10|hi, lo)
 *
 * Unlike the stock firmware this driver keeps a shadow framebuffer
 * (8 pages x 128 bytes = 1 KB) so callers can draw freely and then push the
 * whole screen with lcd_refresh().
 *
 * Everything from lcd_fb onward is hardware independent; define LCD_HOST_TEST to
 * build only that part (used by firmware/tools/preview.c).
 */
#ifndef DRIVER_LCD_ST7565_H
#define DRIVER_LCD_ST7565_H

#include <stdint.h>

#include "driver/fonts.h"

/* Panel geometry (see ../ra89r_lcd.md): a 128x64 monochrome glass behind an
 * ST7565-family controller, addressed as 8 pages x 128 columns.  The stock
 * firmware adds 4 to the column address before writing it (0x0800F698) and the
 * open-source UV-K1/K5V3 ST7565 driver does the same; if a bring-up shows the
 * image shifted by 4 pixels, set LCD_COLUMN_OFFSET to 0. */
#define LCD_WIDTH           128u
#define LCD_HEIGHT          64u
#define LCD_PAGES           (LCD_HEIGHT / 8u)
#define LCD_COLUMN_OFFSET   4u

extern uint8_t lcd_fb[LCD_PAGES][LCD_WIDTH];

/* Init variants: the stock sequence (what the RA89R firmware sends) or the
 * standard ST7565 commands only, without the eight vendor-specific bytes.  If
 * the panel only works with one of them, that tells us which. */
#define LCD_INIT_STOCK   0
#define LCD_INIT_SIMPLE  1

/* Reset pulse plus the stock init sequence (variant LCD_INIT_STOCK). */
void lcd_init(void);
/* Re-run the panel init with a different variant; returns the variant used. */
int lcd_reinit(int variant);
int lcd_variant(void);

/* Panel primitives (write straight to the panel, bypassing the framebuffer). */
void lcd_write_cmd(uint8_t cmd);
void lcd_write_data(uint8_t data);
void lcd_set_addr(uint8_t page, uint8_t column);

/* Framebuffer drawing.  Coordinates are pixels: x = 0..127, y = 0..63. */
void lcd_fb_clear(uint8_t pattern);
void lcd_fb_pixel(uint16_t x, uint16_t y, int on);
void lcd_fb_hline(uint16_t x0, uint16_t x1, uint16_t y, int on);
void lcd_fb_vline(uint16_t x, uint16_t y0, uint16_t y1, int on);
void lcd_fb_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, int on, int filled);
void lcd_fb_invert_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void lcd_fb_glyph(uint16_t x, uint16_t y, const font_t *font, uint32_t ch);
/* Draw text, returns the x coordinate after the last glyph. */
uint16_t lcd_fb_text(uint16_t x, uint16_t y, const font_t *font, const char *s);

/* Push the whole framebuffer to the panel. */
void lcd_refresh(void);

#endif /* DRIVER_LCD_ST7565_H */
