/* Bluetooth menu screen -- see ui/bt.h and docs/ra89r_bluetooth_design.md.
 *
 * Same look and interaction as the K1 menu: the previous and next items in the
 * small font, the current item big, the value on the right and the `nn/nn`
 * index below.  Selecting follows the K1 model -- UP/DOWN move the cursor,
 * MENU enters the item, then UP/DOWN change its value and MENU confirms. */
#include "ui/bt.h"

#include <string.h>

#include "app/bt.h"
#include "audio.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/ui.h"

/* Six characters max, like MenuList[].name. */
static const char *const bt_items[] = {
    "Switch",   /* BT Switch    -- the codeplug Bluetooth bool */
    "Pair",     /* Pairing      -- scan and pick a device */
    "Paired",   /* Paired Dev   -- the connected/saved device */
    "Hold",     /* Hold Time    -- byte 7 */
    "Scan",     /* Scan         -- AT+BT_SCAN on/off */
    "Volume",   /* Spk Volume   -- byte 8 bits 0-3 */
    "Mic",      /* Mic Gain     -- byte 8 bits 4-7 */
    "Info",     /* Blooth Inf   -- module state */
    "PTT",      /* PTT Type     -- byte 9 bits 1-2 */
};

static uint8_t s_cursor;
static bool    s_editing;   /* MENU entered the item; UP/DOWN change its value */
static bool    s_scan;      /* transient: the Scan item is an action, not a setting */
static bool    s_pairing;   /* showing the found-device list */
static uint8_t s_pair_cursor;

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

/* The items MENU enters to change a value (the rest are actions or read-outs). */
static bool bt_item_is_value(unsigned index)
{
    return index == 0u || index == 3u || index == 4u ||
           index == 5u || index == 6u || index == 8u;
}

/* The value shown to the right of an item's label, or "" for none. */
static const char *bt_item_value(unsigned index, char *buf, unsigned cap)
{
    (void)cap;

    switch (index) {
    case 0:
        return gEeprom.BT_Switch ? "On" : "Off";
    case 1:
        if (bt_connected())
            return "Linked";
        return (bt_state() == BT_STATE_SCAN) ? "Scanning" : "";
    case 2:
        if (bt_connected())
            sprintf(buf, "Linked %s", bt_linked_name());
        else if (gEeprom.BT_PairedName[0] != '\0')
            sprintf(buf, "Saved %s", gEeprom.BT_PairedName);
        else
            sprintf(buf, "None");
        return buf;
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

void UI_DisplayBT(void)
{
    const int cnt = (int)BT_MenuCount();
    const int idx = (int)s_cursor;
    char      String[32];
    unsigned  i;

    UI_DisplayClear();

    UI_DrawLineBuffer(gFrameBuffer, (uint8_t)(8u * 6u), 0, (uint8_t)(8u * 6u), 55, 1);
    for (i = 0; i < (8u * 6u); i += 2u)
        gFrameBuffer[5][i] = 0x40;

    if (s_pairing) {
        const unsigned n = bt_found_count();

        UI_PrintStringSmallNormal("Pairing", 0, 0, 0);

        if (n == 0u) {
            /* the scan status on the right, like an item value */
            GUI_DisplaySmallest("Scanning...", 66, 9, false, true);
        } else {
            unsigned k;
            uint8_t  top = 0;

            if (s_pair_cursor >= 6u)
                top = (uint8_t)(s_pair_cursor - 5u);
            for (k = 0; k < 6u && (unsigned)top + k < n; k++) {
                const char   *dev = bt_found_dev((unsigned)top + k);
                char          disp[18];
                const uint8_t line = (uint8_t)(k + 1u);

                strncpy(disp, dev, 16);      /* keep the inverse box in bounds */
                disp[16] = '\0';

                if ((unsigned)top + k == (unsigned)s_pair_cursor) {
                    const unsigned len = (unsigned)strlen(disp);
                    const uint8_t  start = (uint8_t)((127u - len * 7u + 1u) / 2u);

                    UI_PrintStringSmallNormalInverse(disp, start, 0, line);
                } else {
                    UI_PrintStringSmallNormal(disp, 0, 0, line);
                }
            }
            sprintf(String, "%02u/%02u", 1u + (unsigned)s_pair_cursor, n);
            UI_PrintStringSmallNormal(String, 92, 0, 0);
        }

        ST7565_BlitStatusLine();
        ST7565_BlitFullScreen();
        return;
    }

    if (s_editing) {
        /* Entering an item moves its name to the top, the way the K1 menu
         * shows a selected item. */
        UI_PrintString(bt_items[idx], 0, 0, 0, 8);
    } else {
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

    /* the current item's value, on the right */
    if (idx == 2) {
        /* Paired: the state on one line (small font) and the device on the
         * next (the smallest font), like `Linked:` / `Ear Stick`. */
        const char *name = bt_connected() ? bt_linked_name()
                         : (gEeprom.BT_PairedName[0] != '\0' ? gEeprom.BT_PairedName
                                                             : "None");

        UI_PrintStringSmallNormal(bt_connected() ? "Linked:" : "Saved:", 52, 0, 2);
        GUI_DisplaySmallest(name, 52, 34, false, true);
    } else {
        char        vbuf[32];
        const char *val = bt_item_value((unsigned)idx, vbuf, sizeof vbuf);

        if (val[0] != '\0') {
            if (strlen(val) > 6u)
                GUI_DisplaySmallest(val, 52, 28, false, true);
            else
                UI_PrintString(val, 52, 127, 2, 8);
        }
    }

    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
}

/* Change the current item's value (in edit mode).  `dir` is +1 for UP, -1 for
 * DOWN; toggles ignore it. */
static void bt_change(int dir)
{
    switch (s_cursor) {
    case 0:     /* BT Switch */
        gEeprom.BT_Switch = !gEeprom.BT_Switch;
        bt_set_enabled(gEeprom.BT_Switch);
        gRequestSaveSettings = true;
        break;

    case 3:     /* Hold Time: 0..15 */
        gEeprom.BT_HoldTime = (uint8_t)((gEeprom.BT_HoldTime + (unsigned)(dir + 16)) % 16u);
        gRequestSaveSettings = true;
        break;

    case 4:     /* Scan: AT+BT_SCAN on/off */
        s_scan = !s_scan;
        bt_set_scan(s_scan);
        break;

    case 5:     /* Spk Volume */
        gEeprom.BT_SpkGain = (uint8_t)((gEeprom.BT_SpkGain +
            (unsigned)(dir + (int)bt_spk_gain_levels())) % bt_spk_gain_levels());
        bt_set_spk_gain(gEeprom.BT_SpkGain);
        gRequestSaveSettings = true;
        break;

    case 6:     /* Mic Gain */
        gEeprom.BT_MicGain = (uint8_t)((gEeprom.BT_MicGain +
            (unsigned)(dir + (int)bt_mic_gain_levels())) % bt_mic_gain_levels());
        bt_set_mic_gain(gEeprom.BT_MicGain);
        gRequestSaveSettings = true;
        break;

    case 8:     /* PTT Type: BT / local / both */
        gEeprom.BT_PTTType = (uint8_t)((gEeprom.BT_PTTType + (unsigned)(dir + 3)) % 3u);
        gRequestSaveSettings = true;
        break;

    default:
        break;
    }
}

void BT_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    if (bKeyPressed && !bKeyHeld)
        AUDIO_PlayKeyBeep(BEEP_1KHZ_60MS_OPTIONAL);

    if (s_pairing) {
        switch (Key) {
        case KEY_UP:
            if (bKeyPressed && !bKeyHeld && s_pair_cursor > 0u)
                s_pair_cursor--;
            gUpdateDisplay = true;
            break;

        case KEY_DOWN:
            if (bKeyPressed && !bKeyHeld &&
                (unsigned)s_pair_cursor + 1u < bt_found_count())
                s_pair_cursor++;
            gUpdateDisplay = true;
            break;

        case KEY_MENU:
            if (bKeyPressed && !bKeyHeld && bt_found_count() > 0u) {
                bt_connect_dev(s_pair_cursor);
                strncpy(gEeprom.BT_PairedName, bt_found_dev(s_pair_cursor),
                        sizeof gEeprom.BT_PairedName - 1u);
                gEeprom.BT_PairedName[sizeof gEeprom.BT_PairedName - 1u] = '\0';
                gRequestSaveSettings = true;
                gUpdateDisplay = true;
            }
            break;

        case KEY_EXIT:
            if (bKeyPressed && !bKeyHeld) {
                bt_stop_connect();
                s_pairing     = false;
                s_pair_cursor = 0;
                gUpdateDisplay = true;
            }
            break;

        default:
            break;
        }
        return;
    }

    switch (Key) {
    case KEY_UP:
        if (bKeyPressed && !bKeyHeld) {
            if (s_editing)
                bt_change(1);
            else
                s_cursor = (s_cursor == 0u) ? (uint8_t)(BT_MenuCount() - 1u)
                                            : (uint8_t)(s_cursor - 1u);
        }
        gUpdateDisplay = true;
        break;

    case KEY_DOWN:
        if (bKeyPressed && !bKeyHeld) {
            if (s_editing)
                bt_change(-1);
            else
                s_cursor = (uint8_t)((s_cursor + 1u) % BT_MenuCount());
        }
        gUpdateDisplay = true;
        break;

    case KEY_MENU:
        if (bKeyPressed && !bKeyHeld) {
            if (s_editing) {
                s_editing = false;          /* confirm */
            } else if (s_cursor == 1u) {    /* Pair: scan and pick */
                s_pairing     = true;
                s_pair_cursor = 0;
                bt_start_connect();
            } else if (bt_item_is_value(s_cursor)) {
                s_editing = true;           /* enter the item */
            }
            gUpdateDisplay = true;
        }
        break;

    case KEY_EXIT:
        if (bKeyPressed && !bKeyHeld) {
            if (s_editing)
                s_editing = false;
            else
                gRequestDisplayScreen = DISPLAY_MAIN;
        }
        break;

    default:
        break;
    }
}
