/* LCD backlight.
 *
 * RA89R: GPIOA pin 1, a plain push-pull output, level 1 = on.  The stock
 * bootloader blinks exactly this pin when it enters update mode (block at
 * 0x08000582: mask 0x2 on GPIOA, off -> 100 ms -> on -> 100 ms, three times)
 * and leaves it high while it shows the "Update..." screen, so high is the
 * steady "lamp on" state.
 *
 * This build also drives GPIOA pin 5 (BACKLIGHT_AUX_PIN): the bootloader blinks
 * that pin five times just before the pin-1 blink (0x08000AD2), and the panel
 * stayed dark on the radio although pin 1 was driven high.  Driving both is the
 * test; see board_pins.h.
 *
 * The API mirrors App/driver/backlight.{c,h} of the UV-K1/K5V3 port tree, where
 * the same signal is dimmed with a TIM+DMA PWM; this radio is wired for plain
 * on/off, so only the level is driven here (no brightness ramp yet).
 */
#ifndef DRIVER_BACKLIGHT_H
#define DRIVER_BACKLIGHT_H

#include <stdbool.h>

void BACKLIGHT_Init(void);
void BACKLIGHT_TurnOn(void);
void BACKLIGHT_TurnOff(void);
bool BACKLIGHT_IsOn(void);

#endif /* DRIVER_BACKLIGHT_H */
