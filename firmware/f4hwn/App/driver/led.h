/* Status LED -- the radio's red/green indicator, on GPIOA 13/14.
 *
 * Measured on the radio, by stepping the pair through all four combinations and
 * reading the colour after each (see `board_pins.h` for the table):
 *
 *   PA13 = red die, PA14 = green die, **both active HIGH**.
 *
 * The stock drives this same pair itself, and its two routines are the two
 * halves of the indicator:
 *
 *   - `PA14` (green) follows the squelch -- `FUN_08004C84` drives it high while
 *     the squelch is open, `FUN_0801D458` drives it low when it closes -- which
 *     is the `Rx.Light` behaviour;
 *   - `PA13` (red) is the transmit side: `FUN_08018AB8` (TX entry) raises it,
 *     `FUN_08017340` (RX entry) clears it.
 *
 * The active level comes from bit 2 of codeplug settings byte 2, which is what
 * the CPS's `Led Type` selects; this radio's block has it clear, i.e. active
 * high.  `FUN_08018A10` and `FUN_08020028` are the two level setters, and both
 * pins are configured as push-pull outputs together by the boot GPIO init
 * `FUN_08013C74` (GPIOA mask `0x6000`).
 *
 * `PA13`/`PA14` are the Cortex-M `SWDIO`/`SWCLK` pads: the stock gives the debug
 * port up for the indicator, so this driver has to configure them as GPIO before
 * it can drive them.  `docs/ra89r_led.md` has the whole search, including the PA0/PA1
 * attempt that produced nothing visible.
 */
#ifndef DRIVER_LED_H
#define DRIVER_LED_H

#include <stdint.h>

typedef enum {
    LED_OFF = 0,
    LED_RED,
    LED_GREEN,
    LED_BOTH,
    LED_STATE_COUNT
} led_colour_t;

void led_init(void);
void led_set(led_colour_t colour);
led_colour_t led_get(void);

/* "off", "red", "green", "red+green" -- for the console and the boot log. */
const char *led_name(led_colour_t colour);

#endif /* DRIVER_LED_H */
