/* preview.c -- render the firmware's screen layout on a PC.
 *
 * Builds the same shadow framebuffer the target builds and prints it as text,
 * so the layout can be checked (and reviewed in a diff) without a radio:
 *
 *   gcc -I App -I App/driver -DLCD_HOST_TEST tools/preview.c App/ui.c \
 *       App/driver/lcd_st7565.c -o /tmp/preview && /tmp/preview
 *
 * It draws the bring-up test card, then the 1 s status update and one echoed
 * command, which is exactly what the panel shows after boot.
 */
#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "driver/lcd_st7565.h"
#include "ui.h"

static void render(const char *title)
{
    unsigned x;
    unsigned y;

    printf("\n=== %s ===\n    +", title);
    for (x = 0; x < LCD_WIDTH; x++)
        putchar('-');
    printf("+\n");
    for (y = 0; y < LCD_HEIGHT; y++) {
        printf("%3u |", y);
        for (x = 0; x < LCD_WIDTH; x++) {
            unsigned on = (lcd_fb[y >> 3][x] >> (y & 7u)) & 1u;
            putchar(on ? '#' : ' ');
        }
        printf("|\n");
    }
    printf("    +");
    for (x = 0; x < LCD_WIDTH; x++)
        putchar('-');
    printf("+\n");
}

int main(void)
{
    memset(lcd_fb, 0, sizeof lcd_fb);

    ui_test_card();
    render("boot: test card");

    ui_status(7);                 /* the 1 s tick redraws this row */
    render("after 7 s (status row updated)");

    ui_echo("HELLO");             /* a command typed on the UART */
    render("after typing HELLO");

    ui_animate_bar(30);
    render("animation enabled ('p'), bar at x=30");

    ui_pattern(0x55);
    render("'f' checkerboard");
    return 0;
}
