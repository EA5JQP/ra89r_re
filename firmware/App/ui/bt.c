/* Bluetooth menu screen -- see ui/bt.h and docs/ra89r_bluetooth_design.md.
 *
 * The nine items are the stock's `BT Menu` descriptor (0x080256AC).  Phase 1
 * only acts on BT Switch and the value items; Pairing and Paired Dev are shown
 * but inert until their phases. */
#include "ui/bt.h"

#include "app/bt.h"
#include "driver/st7565.h"
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

void UI_DisplayBT(void)
{
    uint8_t  top = 0;
    unsigned i;

    if (s_cursor >= 6u)
        top = (uint8_t)(s_cursor - 5u);

    UI_StatusClear();
    UI_DisplayClear();
    UI_PrintStringSmallNormal("BT Menu", 0, 127, 0);

    for (i = 0; i < 7u && (unsigned)top + i < BT_MenuCount(); i++) {
        const char *name = bt_items[top + i];
        uint8_t     line = (uint8_t)(i + 1u);

        if ((unsigned)top + i == (unsigned)s_cursor)
            UI_PrintStringSmallNormalInverse(name, 0, 127, line);
        else
            UI_PrintStringSmallNormal(name, 0, 127, line);
    }

    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
}

/* Phase 1: BT Switch toggles the service; the value items are placeholders. */
static void bt_activate(void)
{
    switch (s_cursor) {
    case 0:     /* BT Switch */
        gEeprom.BT_Switch = !gEeprom.BT_Switch;
        bt_set_enabled(gEeprom.BT_Switch);
        gRequestSaveSettings = true;
        break;
    default:
        break;
    }
}

void BT_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    (void)bKeyHeld;

    if (!bKeyPressed)
        return;

    switch (Key) {
    case KEY_UP:
        if (s_cursor > 0u)
            s_cursor--;
        gUpdateDisplay = true;
        break;

    case KEY_DOWN:
        if ((unsigned)s_cursor + 1u < BT_MenuCount())
            s_cursor++;
        gUpdateDisplay = true;
        break;

    case KEY_MENU:
        bt_activate();
        gUpdateDisplay = true;
        break;

    case KEY_EXIT:
        gRequestDisplayScreen = DISPLAY_MAIN;
        break;

    default:
        break;
    }
}
