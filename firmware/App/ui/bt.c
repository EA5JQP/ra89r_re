/* Bluetooth menu screen -- see ui/bt.h and docs/ra89r_bluetooth_design.md.
 *
 * The nine items are the stock's `BT Menu` descriptor (0x080256AC).  Phase 1
 * acts on BT Switch, Scan, Spk Volume, Mic Gain, PTT Type and Blooth Inf; the
 * pairing items are shown but inert until Phase 2.  Only six item rows are
 * drawn (lines 1..6): gFrameBuffer has seven pages (0..6), so a seventh row
 * would run off the end into gStatusLine. */
#include "ui/bt.h"

#include <string.h>

#include "app/bt.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/ui.h"

static const char *const bt_items[] = {
    "BT Switch",
    "Pairing",
    "Paired Dev",
    "Hold Time",
    "Scan",
    "Spk Volume",
    "Mic Gain",
    "Blooth Inf",
    "PTT Type",
};

static uint8_t s_cursor;
static bool    s_scan;      /* transient: the Scan item is an action, not a setting */

unsigned BT_MenuCount(void)
{
    return (unsigned)(sizeof bt_items / sizeof bt_items[0]);
}

const char *BT_MenuName(unsigned index)
{
    if (index >= BT_MenuCount())
        return "";
    return bt_items[index];
}

static const char *bt_state_str(bt_state_t state)
{
    switch (state) {
    case BT_STATE_OFF:       return "Off";
    case BT_STATE_RESET:     return "Wait";
    case BT_STATE_CONFIG:    return "Cfg";
    case BT_STATE_IDLE:      return "Ready";
    case BT_STATE_SCAN:      return "Scan";
    case BT_STATE_CONNECT:   return "Conn";
    case BT_STATE_CONNECTED: return "Linked";
    }
    return "?";
}

/* The value shown after an item's label, or "" for none. */
static const char *bt_item_value(unsigned index, char *buf, unsigned cap)
{
    switch (index) {
    case 0:
        return gEeprom.BT_Switch ? "On" : "Off";
    case 3:
        sprintf(buf, "%u", (unsigned)gEeprom.BT_HoldTime);
        return buf;
    case 4:
        return s_scan ? "On" : "Off";
    case 5:
        sprintf(buf, "%u", (unsigned)gEeprom.BT_SpkGain);
        return buf;
    case 6:
        sprintf(buf, "%u", (unsigned)gEeprom.BT_MicGain);
        return buf;
    case 7:
        return bt_state_str(bt_state());
    case 8:
        return gEeprom.BT_PTTType ? "Type 2" : "Type 1";
    default:
        (void)cap;
        return "";
    }
}

void UI_DisplayBT(void)
{
    uint8_t  top = 0;
    unsigned i;

    if (s_cursor >= 6u)
        top = (uint8_t)(s_cursor - 5u);

    UI_StatusClear();
    UI_DisplayClear();
    UI_PrintStringSmallNormal("BT Menu", 0, 127, 0);

    for (i = 0; i < 6u && (unsigned)top + i < BT_MenuCount(); i++) {
        const unsigned idx = (unsigned)top + i;
        const uint8_t  line = (uint8_t)(i + 1u);
        char           vbuf[16];
        char           text[32];
        const char    *val = bt_item_value(idx, vbuf, sizeof vbuf);

        if (val[0] != '\0')
            sprintf(text, "%s %s", bt_items[idx], val);
        else
            sprintf(text, "%s", bt_items[idx]);

        if (idx == (unsigned)s_cursor) {
            /* Center the inverse highlight around the text: the inverse
             * helper inverts from the Start it is given, so pass the centered
             * Start and End = 0 (every other caller does). */
            const unsigned len = (unsigned)strlen(text);
            const uint8_t  start = (uint8_t)((127u - len * 7u + 1u) / 2u);

            UI_PrintStringSmallNormalInverse(text, start, 0, line);
        } else {
            UI_PrintStringSmallNormal(text, 0, 127, line);
        }
    }

    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
}

/* Phase 1: the non-pairing items act; pairing is inert until Phase 2. */
static void bt_activate(void)
{
    switch (s_cursor) {
    case 0:     /* BT Switch */
        gEeprom.BT_Switch = !gEeprom.BT_Switch;
        bt_set_enabled(gEeprom.BT_Switch);
        gRequestSaveSettings = true;
        break;

    case 3:     /* Hold Time */
        gEeprom.BT_HoldTime = (uint8_t)((gEeprom.BT_HoldTime + 1u) % 10u);
        gRequestSaveSettings = true;
        break;

    case 4:     /* Scan */
        s_scan = !s_scan;
        bt_set_scan(s_scan);
        break;

    case 5:     /* Spk Volume */
        gEeprom.BT_SpkGain =
            (uint8_t)((gEeprom.BT_SpkGain + 1u) % bt_spk_gain_levels());
        bt_set_spk_gain(gEeprom.BT_SpkGain);
        gRequestSaveSettings = true;
        break;

    case 6:     /* Mic Gain */
        gEeprom.BT_MicGain =
            (uint8_t)((gEeprom.BT_MicGain + 1u) % bt_mic_gain_levels());
        bt_set_mic_gain(gEeprom.BT_MicGain);
        gRequestSaveSettings = true;
        break;

    case 8:     /* PTT Type */
        gEeprom.BT_PTTType = (uint8_t)((gEeprom.BT_PTTType + 1u) % 2u);
        gRequestSaveSettings = true;
        break;

    default:
        break;
    }
}

void BT_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    switch (Key) {
    case KEY_UP:
        if (bKeyPressed && !bKeyHeld && s_cursor > 0u)
            s_cursor--;
        gUpdateDisplay = true;
        break;

    case KEY_DOWN:
        if (bKeyPressed && !bKeyHeld &&
            (unsigned)s_cursor + 1u < BT_MenuCount())
            s_cursor++;
        gUpdateDisplay = true;
        break;

    case KEY_MENU:
        if (bKeyPressed && !bKeyHeld) {     /* act on press, not on hold-repeat */
            bt_activate();
            gUpdateDisplay = true;
        }
        break;

    case KEY_EXIT:
        if (bKeyPressed && !bKeyHeld)
            gRequestDisplayScreen = DISPLAY_MAIN;
        break;

    default:
        break;
    }
}
