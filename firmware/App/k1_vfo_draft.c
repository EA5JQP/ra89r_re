/* First draft of the K1 VFO screen -- see k1_vfo_draft.h. */
#include <stdio.h>
#include <string.h>

#include "bitmaps.h"
#include "driver/st7565.h"
#include "font.h"
#include "k1_vfo_draft.h"
#include "ui/helper.h"

void k1_vfo_draft_draw(uint32_t freq_hz, const char *channel,
                       uint8_t rssi_bars, uint8_t battery_bars,
                       bool vfo_b, bool locked)
{
    char     freq[16];
    uint8_t  i;

    UI_StatusClear();
    memset(gFrameBuffer, 0, sizeof gFrameBuffer);

    /* ---- status line (panel page 0) ------------------------------------- */
    UI_PrintStringSmallBufferBold(vfo_b ? "VFO B" : "VFO A", gStatusLine);
    if (locked) {
        uint8_t x = 46;
        memcpy(gStatusLine + x, gFontKeyLock, sizeof gFontKeyLock);
    }
    /* Battery: the K1's outline bitmap, then one level segment per bar.  The
     * real geometry lives in the K1's ui/status.c (UI_DrawBattery), which the
     * port brings in with the status line. */
    {
        uint8_t x = LCD_WIDTH - (uint8_t)sizeof BITMAP_BatteryLevel1;
        memcpy(gStatusLine + x, BITMAP_BatteryLevel1, sizeof BITMAP_BatteryLevel1);
        for (i = 0; i < battery_bars && i < 5; i++) {
            uint8_t seg = (uint8_t)(x + 3 + (i * 3));
            if (seg + 1 < LCD_WIDTH)
                memcpy(gStatusLine + seg, BITMAP_BatteryLevel, sizeof BITMAP_BatteryLevel);
        }
    }

    /* ---- frequency, big digits across frame lines 1 and 2 ---------------- */
    snprintf(freq, sizeof freq, "%u.%04u",
             (unsigned)(freq_hz / 1000000u),
             (unsigned)((freq_hz % 1000000u) / 100u));
    UI_DisplayFrequency(freq, 0, 1, true);

    /* ---- channel name and signal level ---------------------------------- */
    UI_PrintStringSmallBold(channel, 0, 0, 5);
    UI_PrintStringSmallNormal("FM", 100, 0, 5);
    ST7565_Gauge(6, 0, 5, rssi_bars);
}

void k1_vfo_draft_show(uint32_t freq_hz, const char *channel,
                       uint8_t rssi_bars, uint8_t battery_bars,
                       bool vfo_b, bool locked)
{
    k1_vfo_draft_draw(freq_hz, channel, rssi_bars, battery_bars, vfo_b, locked);
#ifdef ST7565_HOST_TEST
    /* The host build renders the buffers, nothing else. */
#else
    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
#endif
}
