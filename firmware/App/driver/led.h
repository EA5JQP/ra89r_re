/* Status LED -- the radio's red/green indicator.
 *
 * GPIOA pins 0 and 1 -- and **nothing visual happens** when they are driven, at
 * either level, on this radio.  So the premise this driver was written on ("the
 * stock configures GPIOA mask 3 as outputs and the firmware has a Led Type menu
 * entry, therefore these are the status LED") is *not* supported by the hardware.
 *
 * What the LED actually is: a receive/transmit indicator (green receiving, red
 * transmitting -- the owner's description), which the settings corroborate: the
 * CPS lists "Rx.Light", "Light", "LED Mode" and "Led Type", and the firmware has
 * RX.LIGHT and LIGHT menu strings of its own.
 *
 * It is *not* on a pin this driver could reach.  The stock's TX/RX path
 * (FUN_08016228) writes only RF transceiver registers -- 0x6042/0x6142/0x6740 in
 * register 0x47, 0xbff1 in 0x30, and FUN_08022082(0x203, 0xc) for receive -- and
 * touches no MCU GPIO, so the indicator is almost certainly a register bit on the
 * RF chip.  That is why driving PA0/PA1 does nothing and why the bootloader's PA1
 * blink is invisible on this radio.
 *
 * So this driver is parked: it drives the pins on request, and the LED itself
 * belongs with the RF bring-up.
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
