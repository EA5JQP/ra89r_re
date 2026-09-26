/* The K1's keyboard entry points, over the RA89R's own keypad reader.
 *
 * The K1 scans a 5x? matrix and reads PTT from a GPIO; the RA89R has an ADC
 * ladder on five lines plus PTT2 on PB9, all decoded by driver/keypad.c (with
 * the stock application's windows and its 4th-sample debounce).  So this file
 * is the adapter the imported application polls: same names, same return type,
 * our reader underneath.  See NOTICE.
 */
#include "driver/keyboard.h"

#include "driver/keypad.h"

/* The K1's keyboard state; the application reads the two readings and the
 * debounce counter, so they are kept, but the port's debounce lives in the
 * keypad reader. */
KEY_Code_t gKeyReading0 = KEY_INVALID;
KEY_Code_t gKeyReading1 = KEY_INVALID;
uint16_t   gDebounceCounter;
bool       gWasFKeyPressed;

KEY_Code_t KEYBOARD_Poll(void)
{
    KEY_Code_t key = keypad_poll();

    return key;
}

KEY_Code_t KEYBOARD_GetKey(void)
{
    KEY_Code_t key = keypad_poll();

    /* PB9 is the second PTT on this radio; the K1's PTT is the same function,
     * and the application only knows KEY_PTT. */
    if (key == KEY_PTT2)
        key = KEY_PTT;
    return key;
}

void HideFKeyIcon(void)
{
}
