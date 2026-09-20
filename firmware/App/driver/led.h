/* Status LED -- the radio's red/green indicator.
 *
 * GPIOA pins 0 and 1 -- and **nothing visual happens** when they are driven, at
 * either level, on this radio.  So the premise this driver was written on ("the
 * stock configures GPIOA mask 3 as outputs and the firmware has a Led Type menu
 * entry, therefore these are the status LED") is *not* supported by the hardware.
 *
 * What is known: the stock's pin setup (FUN_080138FC) *does* configure them as
 * outputs, and the bootloader blinks PA1 three times (right after it blinks PA5,
 * which is the backlight).  What that blink is *for* on a board with no visible
 * LED is unknown -- a status LED wired to the companion chip, or a lamp line this
 * variant does not fit.
 *
 * Until that is settled this driver only drives the pins on request, and the
 * console reports which combination it just drove.  Level 1 = lit is assumed.
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

/* Raw test entry: drive exactly these GPIOA pins high and the rest low. */
void led_drive_pins(uint32_t mask);

/* "off", "red", "green", "both" -- for the console and the boot log. */
const char *led_name(led_colour_t colour);

#endif /* DRIVER_LED_H */
