/* preview_k1.c -- render the ported K1 screen buffers on a PC.
 *
 * Builds the same buffers the target builds (gStatusLine + gFrameBuffer through
 * the imported K1 drawing helpers) and prints them as text, so the port's
 * layout can be checked and reviewed in a diff without a radio:
 *
 *   gcc -std=c11 -I App -I App/driver -include App/port_features.h \
 *       -DST7565_HOST_TEST tools/preview_k1.c App/k1_vfo_draft.c \
 *       App/ui/helper.c App/ui/inputbox.c App/settings.c App/font.c \
 *       App/bitmaps.c App/driver/st7565.c -o /tmp/preview_k1 && /tmp/preview_k1
 *
 * Page 0 of the panel is the status line, pages 1..7 are gFrameBuffer[0..6].
 */
#include <stdio.h>
#include <string.h>

#include "driver/st7565.h"
#include "k1_vfo_draft.h"

static uint8_t page_byte(const uint8_t *page, unsigned x)
{
    return page[x];
}

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
            putchar((page_byte(page, x) >> (y & 7u)) & 1u ? '#' : ' ');
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
    k1_vfo_draft_draw(14575000u, "CALL 1", 3, 3, false, false);
    render("K1 VFO draft: 145.7500, CALL 1, rssi 3/5, battery 3/5");

    k1_vfo_draft_draw(43350000u, "REPEATER", 5, 1, true, true);
    render("K1 VFO draft: 433.5000 VFO B, locked, battery 1/5");
    return 0;
}
