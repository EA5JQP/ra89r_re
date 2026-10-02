/* LCD backlight -- GPIOA pin 5, active high.
 *
 * The K1/F4HWN driver on this board: a TIM7 update event drives DMA1 channel 2
 * to write duty words into `GPIOA->BSRR`, so the pin is dimmed by a 4 kHz, 32
 * level software PWM.  `value[11]` is the K1's brightness staircase, and
 * `BACKLIGHT_MIN`/`BACKLIGHT_MAX` choose between its steps; the `BL*` menu items
 * drive them.  The K1 API is kept so the imported application links unchanged.
 *
 * Evidence, the register choices and what is deliberately not ported are in
 * docs/ra89r_led.md and
 * docs/superpowers/specs/2026-10-01-backlight-k1-port-design.md.
 *
 * `BACKLIGHT_Init()`/`BACKLIGHT_InitHardware()` bring the timer and DMA up with
 * the light *off*; call `BACKLIGHT_TurnOn()` after the settings are loaded (as
 * the K1 does at its welcome screen), or the panel stays dark.
 */
#ifndef DRIVER_BACKLIGHT_H
#define DRIVER_BACKLIGHT_H

#include <stdbool.h>
#include <stdint.h>

/* K1-compatible state. */
extern uint16_t gBacklightCountdown_500ms;
extern uint8_t  gBacklightBrightness;
extern uint8_t  gBacklightBrightnessOld;
extern const uint8_t value[11];

void BACKLIGHT_Init(void);
void BACKLIGHT_TurnOn(void);
void BACKLIGHT_TurnOff(void);
bool BACKLIGHT_IsOn(void);

/* K1 names. */
void BACKLIGHT_InitHardware(void);
void BACKLIGHT_UpdateTickless(void);
void BACKLIGHT_SetBrightness(uint8_t brightness);
uint8_t BACKLIGHT_GetBrightness(void);
void BACKLIGHT_Update(void);

/* Diagnostic: how many duty words are currently ON (the PWM's level). */
unsigned BACKLIGHT_DutyOnCount(void);

#endif /* DRIVER_BACKLIGHT_H */
