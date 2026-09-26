/* Screen layout for the RA89R bring-up firmware.
 *
 * These functions only touch the shadow framebuffer (lcd_fb), so they can be
 * compiled and rendered on a PC -- see firmware/tools/preview.c.  Keep them free
 * of hardware access.
 *
 * Zone map (128x64, border on rows 0 and 63, columns 0 and 127):
 *
 *   rows  4..19   title,       8x16 font,   x=6
 *   rows 22..37   second line, 8x16 font,   x=6
 *   rows 40..46   status,      5x7 font,    x=6      (uptime)
 *   rows 48..54   sample line, 5x7 font,    x=6      (font demo / animation)
 *   rows 56..62   echo line,   5x7 font,    x=6      (last typed characters)
 */
#ifndef APP_UI_H
#define APP_UI_H

#include <stdint.h>

void ui_clear(void);
void ui_test_card(void);
void ui_status(uint32_t seconds);
void ui_echo(const char *text);
void ui_border(int on);
void ui_pattern(uint8_t value);
void ui_animate_bar(uint16_t x);
void ui_logo_tag(uint32_t seconds);

/* Bench read-out: a title on the 8x16 line and a detail line under it, so the
 * state of a test can be read with the console cable unplugged. */
void ui_bench(const char *title, const char *detail);

#endif /* APP_UI_H */
