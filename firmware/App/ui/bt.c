/* Bluetooth menu screen -- see ui/bt.h and docs/ra89r_bluetooth_design.md.
 *
 * Same layout as the K1 menu (UI_DisplayMenu, original layout): a three-row
 * left column (previous / current / next) with the current item inverted, a
 * dotted separator, the item value on the right and the index/count below.
 * The labels are kept to six characters so they fit the column, like the K1
 * menu's own names.  Only six item rows are ever drawn (the K1's window is
 * three), so nothing runs past gFrameBuffer[6]. */
#include "ui/bt.h"

#include <string.h>

#include "app/bt.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/ui.h"

/* Six characters max, like MenuList[].name. */
static const char *const bt_items[] = {
    "Switch",   /* BT Switch    -- the codeplug Bluetooth bool (byte 9 bit 0) */
    "Pair",     /* Pairing      -- scan and auto-connect */
    "Paired",   /* Paired Dev   -- the connected/last device */
    "Hold",     /* Hold Time    -- byte 7 bits 0-3 */
    "Scan",     /* Scan         -- AT+BT_SCAN on/off */
    "Volume",   /* Spk Volume   -- byte 8 bits 0-3 */
    "Mic",      /* Mic Gain     -- byte 8 bits 4-7 */
    "Info",     /* Blooth Inf   -- module state */
    "PTT",      /* PTT Type     -- byte 9 bits 1-2 */
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

static const char *bt_ptt_str(uint8_t type)
{
    switch (type) {
    case 0:  return "BT";
    case 1:  return "Local";
    default: return "Both";
    }
}

/* The value shown after an item's label, or "" for none. */
static const char *bt_item_value(unsigned index, char *buf, unsigned cap)
{
    (void)cap;

    switch (index) {
    case 0:
        return gEeprom.BT_Switch ? "On" : "Off";
    case 1:
        if (bt_connected())
            return "Linked";
        return bt_state() == BT_STATE_SCAN ? "Scan" : "";
    case 2:
        return bt_connected() ? "Linked" : "None";
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
        return bt_ptt_str(gEeprom.BT_PTTType);
    default:
        return "";
    }
}

/* The K1 menu is built with ENABLE_CUSTOM_MENU_LAYOUT, so its layout is the
 * "new" one in UI_DisplayMenu: the previous and next items in the small font
 * (lines 1 and 4), the current item big (line 2), the `%02u/%02u` index at
 * x=6 line 6, and no inverted bar.  This matches it exactly. */
void UI_DisplayBT(void)
{
    const int cnt = (int)BT_MenuCount();
    const int idx = (int)s_cursor;
    char      String[16];
    unsigned  i;

    UI_StatusClear();
    UI_DisplayClear();

    UI_DrawLineBuffer(gFrameBuffer, (uint8_t)(8u * 6u), 0, (uint8_t)(8u * 6u), 55, 1);
    for (i = 0; i < (8u * 6u); i += 2u)
        gFrameBuffer[5][i] = 0x40;

    {
        int prev = idx - 1;
        int next = idx + 1;

        if (prev < 0)
            prev = cnt - 1;
        if (next >= cnt)
            next = 0;

        UI_PrintStringSmallNormal(bt_items[prev], 0, 0, 1);   /* previous */
        UI_PrintString(bt_items[idx], 0, 0, 2, 8);            /* current  */
        UI_PrintStringSmallNormal(bt_items[next], 0, 0, 4);   /* next     */
    }

    sprintf(String, "%02u/%02u", 1u + (unsigned)idx, (unsigned)cnt);
    UI_PrintStringSmallNormal(String, 6, 0, 6);

    /* the current item's value, on the right (big font, line 2) */
    {
        char        vbuf[16];
        const char *val = bt_item_value((unsigned)idx, vbuf, sizeof vbuf);

        if (val[0] != '\0')
            UI_PrintString(val, (8u * 6u) + 2u, LCD_WIDTH - 1u, 2, 8);
    }

    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
}

/* Every item acts now; the value fields mirror the stock's codeplug fields. */
static void bt_activate(void)
{
    switch (s_cursor) {
    case 0:     /* BT Switch */
        gEeprom.BT_Switch = !gEeprom.BT_Switch;
        bt_set_enabled(gEeprom.BT_Switch);
        gRequestSaveSettings = true;
        break;

    case 1:     /* Pairing: scan and auto-connect */
        bt_start_connect();
        break;

    case 3:     /* Hold Time: 0..15 (4S..15S, Infinite) */
        gEeprom.BT_HoldTime = (uint8_t)((gEeprom.BT_HoldTime + 1u) & 0x0Fu);
        gRequestSaveSettings = true;
        break;

    case 4:     /* Scan: AT+BT_SCAN on/off */
        s_scan = !s_scan;
        bt_set_scan(s_scan);
        break;

    case 5:     /* Spk Volume: the stock's gain levels */
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

    case 8:     /* PTT Type: BT / local / both */
        gEeprom.BT_PTTType = (uint8_t)((gEeprom.BT_PTTType + 1u) % 3u);
        gRequestSaveSettings = true;
        break;

    default:    /* Paired Dev (2) and Blooth Inf (7) are read-outs */
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
