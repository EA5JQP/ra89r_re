/* Keypad -- the RA89R's 20 buttons.
 *
 * Nineteen of them are analog: five lines (PA2, PA3, PA6, PA7, PB0), each with a
 * four-value resistor ladder, sampled by the ADC.  The twentieth is PTT2 on PB9,
 * a plain digital input.  The stock application decodes the same way -- it scans
 * six ADC channels and stores a per-key hold counter -- and the windows below are
 * its own calibration, copied verbatim, so a reading can be compared with the
 * stock decode directly (see ra89r_findings.md, "Keypad").
 *
 * This module only *reads*: the ADC samples the ladder, nothing drives or pulls
 * those pins (that is what warmed the radio up while a pin probe held them up).
 */
#ifndef DRIVER_KEYPAD_H
#define DRIVER_KEYPAD_H

#include <stdint.h>
#include <stdbool.h>

/* Key codes, in the stock application's numbering: 10..19 are digits 0..9
 * (`digit = code - 10`), 4..9 the programmable keys, 0x14..0x25 the function
 * keys with their long / extra-long variants, and 100 the key PTT1 pulls -- the
 * same numbering the stock UI dispatches on. */
#define KEYPAD_NONE (-1)

/* The five analog lines, in the stock scan order (ADC ranks 0..4). */
enum {
    KEYPAD_LINE_PA2 = 0,
    KEYPAD_LINE_PA3,
    KEYPAD_LINE_PA6,
    KEYPAD_LINE_PA7,
    KEYPAD_LINE_PB0,
    KEYPAD_LINE_COUNT
};

/* Power up the ADC and give it a first conversion to settle. */
void keypad_init(void);

/* Code of the key currently held, or KEYPAD_NONE.  Level semantics, like the
 * port tree's KEYBOARD_Poll: the debounce / long-press logic belongs on top. */
int keypad_scan(void);

/* Last raw 12-bit sample of one line (for the console monitor). */
uint16_t keypad_raw(unsigned line);

const char *keypad_line_name(unsigned line);

/* "digit 3", "up", "code 0x14", ... -- for the monitor printout. */
const char *keypad_name(int code);

/* The three codes a window can produce (short/long/extra-long, 0xFF = none) for
 * the window that yielded this code; NULL if unknown.  The stock app picks
 * between them by how long the key was held. */
const uint8_t *keypad_variants(int code);

/* PTT2 on PB9 -- the one digital key.  Returns its level as read. */
bool keypad_ptt2_level(void);

#endif /* DRIVER_KEYPAD_H */
