/* Status LED -- the radio's red/green indicator.
 *
 * GPIOA pins 0 and 1, plain push-pull outputs.  The stock's pin setup
 * (FUN_080138FC) configures GPIOA mask 3 as outputs, next to PC15 and PD0, and
 * the firmware carries a "Led Type" menu entry (string 0x08026F8C) with the CPS
 * exposing an "LED Mode" setting -- so this is the radio's status LED.
 *
 * It is what the old "backlight" driver was really driving: PA1 alone never lit
 * the panel, and PA5 is DAC_OUT2 (the beeper).  The panel's own backlight is a
 * different pin and is still unidentified.
 *
 * Which pin is red and which is green has not been measured yet; the assignment
 * in led.c is provisional and the console cycles every state so the radio can
 * settle it.  Same for the polarity: level 1 = lit is assumed.
 */
#ifndef DRIVER_LED_H
#define DRIVER_LED_H

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

/* "off", "red", "green", "both" -- for the console and the boot log. */
const char *led_name(led_colour_t colour);

#endif /* DRIVER_LED_H */
