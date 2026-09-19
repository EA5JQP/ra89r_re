/* LCD backlight.
 *
 * RA89R: GPIOA pin 1, driven as a plain push-pull output -- the stock bootloader
 * blinks exactly this pin when it enters update mode (block at 0x08000582: mask
 * 0x2 on GPIOA, off -> 100 ms -> on -> 100 ms, three times), and the stock
 * application drives the same pin heavily while offering "Back Light"/"Rx.Light"
 * menu items, i.e. it is the user-visible lamp.  See ra89r_findings.md.
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
