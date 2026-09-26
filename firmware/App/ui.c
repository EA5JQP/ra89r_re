#include "ui.h"

#include "board_pins.h"
#include "driver/fonts.h"
#include "driver/lcd_st7565.h"

/* so the banner cannot go stale when the baud changes in board.h */
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

void ui_clear(void)
{
    lcd_fb_clear(0x00);
}

void ui_test_card(void)
{
    lcd_fb_clear(0x00);
    lcd_fb_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 1, 0);

    lcd_fb_text(6, 4, &font_8x16, "RA89R LCD");
    lcd_fb_text(6, 22, &font_8x16, "UART " STRINGIFY(BOARD_UART_BAUD));

    /* 5x7 font sample; longer than the screen, so it is clipped on purpose */
    lcd_fb_text(6, 48, &font_5x7, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
}

void ui_status(uint32_t seconds)
{
    char buf[16];
    char digits[11];
    uint32_t n = 0;
    uint32_t i = 0;
    uint32_t v = seconds;

    lcd_fb_rect(2, 39, LCD_WIDTH - 3, 47, 0, 1);        /* clear the status row */

    buf[i++] = 'U';
    buf[i++] = 'P';
    buf[i++] = 'T';
    if (v == 0) {
        buf[i++] = '0';
    } else {
        while (v && n < sizeof(digits)) {
            digits[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (n && i < sizeof(buf) - 1u)
            buf[i++] = digits[--n];
    }
    buf[i] = '\0';
    lcd_fb_text(6, 40, &font_5x7, buf);
}

void ui_echo(const char *text)
{
    lcd_fb_rect(2, 55, LCD_WIDTH - 3, 62, 0, 1);
    lcd_fb_text(6, 56, &font_5x7, text);
}

void ui_bench(const char *title, const char *detail)
{
    lcd_fb_rect(2, 21, LCD_WIDTH - 3, 38, 0, 1);
    lcd_fb_text(6, 22, &font_8x16, title);
    lcd_fb_rect(2, 48, LCD_WIDTH - 3, 54, 0, 1);
    lcd_fb_text(6, 48, &font_5x7, detail);
}

void ui_border(int on)
{
    lcd_fb_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, on, 0);
}

void ui_pattern(uint8_t value)
{
    lcd_fb_clear(value);
}

void ui_animate_bar(uint16_t x)
{
    lcd_fb_rect(2, 47, LCD_WIDTH - 3, 55, 0, 1);        /* the sample row */
    lcd_fb_rect(x, 48, (uint16_t)(x + 23u), 54, 1, 1);
}
