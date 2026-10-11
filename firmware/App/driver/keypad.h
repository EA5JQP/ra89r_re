/* Keypad -- the RA89R's 20 buttons.
 *
 * Nineteen of them are analog: five lines (PA2, PA3, PA6, PA7, PB0), each with a
 * four-value resistor ladder, sampled by the ADC.  The twentieth is PTT2 on PB9,
 * a plain digital input.  The stock application decodes the same way -- it scans
 * six ADC channels, stores a per-key hold counter and posts the key's code at
 * the 4th in-window sample (0x08005724) -- and the windows below are its own
 * calibration, copied verbatim, so a reading can be compared with the stock
 * decode directly (see docs/ra89r_keypad.md).
 *
 * This module only *reads*: the ADC samples the ladder, nothing drives or pulls
 * those pins (that is what warmed the radio up while a pin probe held them up).
 */
#ifndef DRIVER_KEYPAD_H
#define DRIVER_KEYPAD_H

#include <stdint.h>
#include <stdbool.h>

/* The key codes are the K5V3 / F4HWN enumeration, identical to the port tree's
 * App/driver/keyboard.h, so the ported UI can use this reader unchanged.  The
 * one addition is KEY_PTT2, which this radio has and the K5V3 does not; it sits
 * before KEY_INVALID so tables sized [KEY_INVALID] still cover it. */
typedef enum {
    KEY_0 = 0,  // 0
    KEY_1,      // 1
    KEY_2,      // 2
    KEY_3,      // 3
    KEY_4,      // 4
    KEY_5,      // 5
    KEY_6,      // 6
    KEY_7,      // 7
    KEY_8,      // 8
    KEY_9,      // 9
    KEY_MENU,   // A
    KEY_UP,     // B
    KEY_DOWN,   // C
    KEY_EXIT,   // D
    KEY_STAR,   // *
    KEY_F,      // #
    KEY_PTT,    //
    KEY_SIDE2,  //
    KEY_SIDE1,  //
    KEY_PTT2,   // RA89R only: the second PTT, on PB9
    KEY_INVALID // no key, or a code not mapped yet
} KEY_Code_t;

/* The stock application's own numbering, which keypad_stock_code() reports so a
 * reading can be matched against the decode in docs/ra89r_findings.md: 10..19 are
 * digits 0..9 (digit = code - 10), 0x14..0x19 the function keys in their short
 * form, 4..9 the programmable keys, and 100 the key that pulls PA2 fully low. */
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

/* Power up the ADC (channels, sample time, calibration) and check that it
 * really converts.  Returns false if the self-test conversion did not finish --
 * worth printing, because that failure mode makes every read look like 0xFFFF. */
bool keypad_init(void);

/* Level semantics, like the port tree's KEYBOARD_Poll: the key currently held,
 * or KEY_INVALID.  Debounce, repeat and long-press belong on top -- the stock
 * application does the same, posting its short / held / long codes from a hold
 * counter rather than from the level alone.  PTT2 is reported as KEY_PTT2 while
 * it is held; keypad_ptt2_level() is there for callers that want it separately. */
KEY_Code_t keypad_poll(void);

/* The stock code of the key keypad_poll() returned, or KEYPAD_NONE.  PTT2 has
 * no stock code (it is not in the ADC table). */
int keypad_stock_code(void);

/* "0", "UP", "PTT", ... and "?" for a code that is not mapped yet. */
const char *keypad_name(KEY_Code_t key);

/* "digit 3", "PTT1", "code 0x14", ... -- the stock view, for the monitor. */
const char *keypad_stock_name(int code);

/* Last raw 12-bit sample of one line (for the console monitor). */
uint16_t keypad_raw(unsigned line);

/* The stock scans a sixth channel -- PB1, ADC channel 9 -- separately from the
 * keypad and uses it as the battery level (FUN_0800E514 / FUN_08007664).  It is
 * sampled into the same free-running DMA round; this returns its newest 12-bit
 * value, which is what the battery driver reports. */
uint16_t keypad_aux_raw(void);

const char *keypad_line_name(unsigned line);

/* The three codes a window can produce (short / held / long, 0xFF = none) for
 * the window that yielded this stock code; NULL if unknown. */
const uint8_t *keypad_variants(int code);

/* PTT2 on PB9: the level as read (low = pressed). */
bool keypad_ptt2_level(void);

#endif /* DRIVER_KEYPAD_H */
