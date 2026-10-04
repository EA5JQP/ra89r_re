/* The Bluetooth menu screen (F + MENU) -- see docs/ra89r_bluetooth_design.md.
 *
 * A dedicated screen rather than a MenuList[] entry, so the K1 main menu stays
 * short.  The item set is the stock's own `BT Menu` descriptor. */
#ifndef UI_BT_H
#define UI_BT_H

#include <stdbool.h>

#include "driver/keyboard.h"    /* KEY_Code_t */

void        UI_DisplayBT(void);
void        BT_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);
unsigned    BT_MenuCount(void);
const char *BT_MenuName(unsigned index);

#endif /* UI_BT_H */
