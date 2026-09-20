/* LCD backlight -- GPIOA pin 5.
 *
 * Confirmed on the radio by eye: driving this pin lights the panel.  The
 * bootloader blinks the same pin on entering update mode (0x08000AD2: low for
 * 100 ms, then high, five times, ending lit), and the stock application also
 * configures it as DAC_OUT2 -- so the stock is probably dimming the backlight
 * through the DAC.  Plain on/off is what this radio needs today; a brightness
 * ramp through the DAC would be the refinement.
 *
 * The API keeps the shape of the UV-K1/K5V3 port tree's backlight driver.
 */
#ifndef DRIVER_BACKLIGHT_H
#define DRIVER_BACKLIGHT_H

#include <stdbool.h>

void BACKLIGHT_Init(void);
void BACKLIGHT_TurnOn(void);
void BACKLIGHT_TurnOff(void);
bool BACKLIGHT_IsOn(void);

#endif /* DRIVER_BACKLIGHT_H */
