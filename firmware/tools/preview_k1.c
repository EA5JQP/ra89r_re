/* preview_k1.c -- render the ported K1 screens on a PC.
 *
 * The VFO screen is the K1's own ui/main.c (UI_DisplayMain) compiled against
 * the port's headers, buffers and state facade; this tool just sets the state
 * up, calls it, and prints the panel as text.  Page 0 is the status line,
 * pages 1..7 are gFrameBuffer[0..6].
 *
 *   gcc -std=c11 -I App -I App/driver -include App/port_features.h \
 *       -DST7565_HOST_TEST tools/preview_k1.c App/ui/main.c App/ui/helper.c \
 *       App/ui/inputbox.c App/port_state.c App/settings.c App/font.c \
 *       App/bitmaps.c App/driver/st7565.c -o /tmp/preview_k1 && /tmp/preview_k1
 *
 * (AGENTS.md, "Offline checks", has the same command with the SDK path.)
 */
#include <stdio.h>

#include "driver/st7565.h"
#include "port_state.h"
#include "radio.h"
#include "settings.h"
#include "ui/main.h"
#include "ui/menu.h"

static void render(const char *title)
{
    unsigned x;
    unsigned y;

    printf("\n=== %s ===\n    +", title);
    for (x = 0; x < LCD_WIDTH; x++)
        putchar('-');
    printf("+\n");

    for (y = 0; y < LCD_HEIGHT; y++) {
        const uint8_t *page = (y < 8) ? gStatusLine : gFrameBuffer[(y >> 3) - 1];
        printf("%3u |", y);
        for (x = 0; x < LCD_WIDTH; x++)
            putchar((page[x] >> (y & 7u)) & 1u ? '#' : ' ');
        printf("|\n");
    }

    printf("    +");
    for (x = 0; x < LCD_WIDTH; x++)
        putchar('-');
    printf("+\n");
}

int main(void)
{
    ST7565_Init();
    port_state_init();

    gRxVfo->freq_config_RX.Frequency = 14575000u;
    gRxVfo->freq_config_TX.Frequency = 14575000u;
    UI_DisplayMain();
    render("K1 UI_DisplayMain(): 145.7500 MHz, channel, status line");

    /* The menu's entry sequence (app/main.c): build the view, then draw it.
     * ENABLE_FEAT_F4HWN_MENU_CAT is off (the K1's default preset), so this is
     * the flat list. */
    UI_MENU_BuildView();
    UI_DisplayMenu();
    render("K1 UI_DisplayMenu(): the menu list, first item");

    gMenuCursor = 3;
    UI_DisplayMenu();
    render("K1 UI_DisplayMenu(): the menu list, fourth item");

    gRxVfo->freq_config_RX.Frequency = 43350000u;
    gRxVfo->freq_config_TX.Frequency = 43350000u;
    UI_DisplayMain();
    render("K1 UI_DisplayMain(): 433.5000 MHz");

    return 0;
}
