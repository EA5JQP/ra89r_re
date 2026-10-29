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
static bool    s_connecting;/* a found device was selected; show progress/result */
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
        if (!bt_enabled())
            return "BT Off";
        if (bt_connected())
            return "Linked";
        return (bt_state() == BT_STATE_SCAN) ? "Scanning" : "Search";
    case 2:                     /* drawn by bt_draw_paired() */
        return "";
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

/* The K1 menu's value pane (ui/menu.c: menu_item_x1/x2): right of the
 * 6-character item column and its divider. */
#define BT_PANE_X1  50u
#define BT_PANE_X2  127u
#define BT_PANE_BIG   ((BT_PANE_X2 - BT_PANE_X1) / 8u)    /* 9 big chars */
#define BT_PANE_SMALL ((BT_PANE_X2 - BT_PANE_X1) / 7u)    /* 11 small chars */

/* `text` centred in the value pane over pages `line` and `line + 1`: the big
 * font when it fits, as the K1 prints a value; else the small font, on one
 * line or wrapped at a space onto two, each clipped to the pane. */
static void bt_pane_text(const char *text, uint8_t line)
{
    char           buf[BT_PANE_SMALL + 1u];
    const unsigned len = (unsigned)strlen(text);
    unsigned       cut;

    if (len <= BT_PANE_BIG) {
        UI_PrintString(text, BT_PANE_X1, BT_PANE_X2, line, 8);
        return;
    }
    if (len <= BT_PANE_SMALL) {
        UI_PrintStringSmallNormal(text, BT_PANE_X1, BT_PANE_X2, (uint8_t)(line + 1u));
        return;
    }

    /* Two lines: break at the last space that keeps the first one in the
     * pane, or hard at the pane width when there is none. */
    for (cut = BT_PANE_SMALL; cut > 0u && text[cut] != ' '; cut--)
        ;
    if (cut == 0u)
        cut = BT_PANE_SMALL;
    memcpy(buf, text, cut);
    buf[cut] = '\0';
    UI_PrintStringSmallNormal(buf, BT_PANE_X1, BT_PANE_X2, line);

    text += cut;
    while (*text == ' ')
        text++;
    strncpy(buf, text, BT_PANE_SMALL);
    buf[BT_PANE_SMALL] = '\0';
    UI_PrintStringSmallNormal(buf, BT_PANE_X1, BT_PANE_X2, (uint8_t)(line + 1u));
}

/* Small text centred in the value pane, clipped to it. */
static void bt_pane_small(const char *text, uint8_t line)
{
    char buf[BT_PANE_SMALL + 1u];

    strncpy(buf, text, BT_PANE_SMALL);
    buf[BT_PANE_SMALL] = '\0';
    UI_PrintStringSmallNormal(buf, BT_PANE_X1, BT_PANE_X2, line);
}

/* The K1's top-right rounded index badge (CTCSS `nn/nn`), centred in the pane:
 * small inverse text on `line`. */
static void bt_pane_badge(const char *text, uint8_t line)
{
    const unsigned width = (unsigned)strlen(text) * 7u + 1u;
    const unsigned start = BT_PANE_X1 + ((BT_PANE_X2 - BT_PANE_X1 + 1u) - width) / 2u;

    UI_PrintStringSmallNormalInverse(text, (uint8_t)start, 0, line);
}

/* Paired: the K1 MEM-CH layout -- what the device is to us on top, its name
 * big below, "None" alone when there is nothing. */
static void bt_draw_paired(void)
{
    if (bt_connected()) {
        bt_pane_small("Linked", 1);
        bt_pane_text(bt_linked_name(), 2);
    } else if (gEeprom.BT_PairedName[0] != '\0') {
        bt_pane_small("Saved", 1);
        bt_pane_text(gEeprom.BT_PairedName, 2);
    } else {
        bt_pane_text("None", 2);
    }
}

/* Pair, entered: a K1 list value -- index badge, the device big, its signal
 * below; progress and the result replace it once one is chosen. */
static void bt_draw_pairing(void)
{
    const unsigned n = bt_found_count();
    char           buf[24];

    if (s_connecting) {
        const bool  linked = bt_connected();
        const char *name   = linked ? bt_linked_name()
                           : (gEeprom.BT_PairedName[0] != '\0'
                              ? gEeprom.BT_PairedName : "Device");

        bt_pane_small(linked ? "Linked" : "Connecting", 1);
        bt_pane_text(name, 2);
        if (linked)
            bt_pane_small("EXIT: done", 5);
        return;
    }

    if (n == 0u) {
        bt_pane_text("Scanning", 2);
        bt_pane_small("no device", 5);
        return;
    }

    sprintf(buf, "%02u/%02u", 1u + (unsigned)s_pair_cursor, n);
    bt_pane_badge(buf, 0);
    bt_pane_text(bt_found_dev(s_pair_cursor), 2);
    if (bt_found_rssi(s_pair_cursor)[0] != '\0') {
        snprintf(buf, sizeof buf, "%s dBm", bt_found_rssi(s_pair_cursor));
        bt_pane_small(buf, 5);
    }
    if (bt_state() == BT_STATE_SCAN)
        GUI_DisplaySmallest("SCAN", 112, 50, false, true);
}

void UI_DisplayBT(void)
{
    const int cnt = (int)BT_MenuCount();
    const int idx = (int)s_cursor;
    char      String[32];
    unsigned  i;

    UI_DisplayClear();

    /* The K1 menu frame: the divider and the dotted rule over the index. */
    UI_DrawLineBuffer(gFrameBuffer, (uint8_t)(8u * 6u), 0, (uint8_t)(8u * 6u), 55, 1);
    for (i = 0; i < (8u * 6u); i += 2u)
        gFrameBuffer[5][i] = 0x40;

    if (s_editing || s_pairing) {
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

    /* The current item's value, in the pane on the right. */
    if (s_pairing) {
        bt_draw_pairing();
    } else if (idx == 2) {
        bt_draw_paired();
    } else {
        char        vbuf[32];
        const char *val = bt_item_value((unsigned)idx, vbuf, sizeof vbuf);

        if (val[0] != '\0')
            bt_pane_text(val, 2);
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
        case KEY_UP:            /* the list wraps, like a K1 menu value */
            if (bKeyPressed && !bKeyHeld && !s_connecting && bt_found_count() > 0u)
                s_pair_cursor = (s_pair_cursor == 0u)
                              ? (uint8_t)(bt_found_count() - 1u)
                              : (uint8_t)(s_pair_cursor - 1u);
            gUpdateDisplay = true;
            break;

        case KEY_DOWN:
            if (bKeyPressed && !bKeyHeld && !s_connecting && bt_found_count() > 0u)
                s_pair_cursor = (uint8_t)(((unsigned)s_pair_cursor + 1u) %
                                          bt_found_count());
            gUpdateDisplay = true;
            break;

        case KEY_MENU:
            if (bKeyPressed && !bKeyHeld && s_connecting && bt_connected()) {
                /* linked: MENU confirms, like leaving a K1 value */
                s_pairing    = false;
                s_connecting = false;
                gUpdateDisplay = true;
            } else if (bKeyPressed && !bKeyHeld && !s_connecting &&
                       bt_found_count() > 0u) {
                s_connecting = true;
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
                s_connecting  = false;
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
            } else if (s_cursor == 1u && bt_enabled()) {  /* Pair: scan and pick */
                s_pairing     = true;
                s_connecting  = false;
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
