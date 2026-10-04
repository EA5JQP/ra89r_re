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

/* Same layout as the K1 menu (UI_DisplayMenu, original layout): a three-row
 * left column (previous / current / next) with the current item inverted, a
 * dotted separator, the item value on the right and the index/count below. */
#define BT_LIST_CHARS   6u
#define BT_ITEM_X1      ((8u * BT_LIST_CHARS) + 2u)     /* 50 */
#define BT_ITEM_X2      (LCD_WIDTH - 1u)

void UI_DisplayBT(void)
{
    const unsigned n = BT_MenuCount();
    char           String[16];
    unsigned       i;

    UI_StatusClear();
    UI_DisplayClear();

    /* the vertical separating line, and the dotted bottom row */
    UI_DrawLineBuffer(gFrameBuffer, (uint8_t)(8u * BT_LIST_CHARS), 0,
                      (uint8_t)(8u * BT_LIST_CHARS), 55, 1);
    for (i = 0; i < (8u * BT_LIST_CHARS); i += 2u)
        gFrameBuffer[5][i] = 0x40;

    /* the three visible items: previous (line 0), current (line 2), next (4) */
    for (i = 0; i < 3u; i++) {
        if (s_cursor == 0u && i == 0u)
            continue;
        if (s_cursor + 1u == n && i == 2u)
            continue;
        {
            const unsigned idx = (unsigned)((int)s_cursor + (int)i - 1);
            if (idx < n)
                UI_PrintString(bt_items[idx], 0, 0, (uint8_t)(i * 2u), 8);
        }
    }

    /* invert the current item's pixels (the big-font row pair) */
    for (i = 0; i < (8u * BT_LIST_CHARS); i++) {
        gFrameBuffer[2][i] ^= 0xFF;
        gFrameBuffer[3][i] ^= 0xFF;
    }

    /* the index/count, bottom left */
    sprintf(String, "%2u.%u", 1u + s_cursor, n);
    UI_PrintStringSmallNormal(String, 2, 0, 6);

    /* the current item's value, on the right */
    {
        char        vbuf[16];
        const char *val = bt_item_value(s_cursor, vbuf, sizeof vbuf);

        if (val[0] != '\0')
            UI_PrintString(val, BT_ITEM_X1, BT_ITEM_X2, 2, 8);
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
