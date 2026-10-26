/* Put the system clock into a state we can reason about, at a usable speed.
 *
 * The stock bootloader runs at its own frequency (it has both 8 MHz and 24 MHz
 * constants and calibrates delays from a value it keeps in RAM) and it hands
 * that configuration over together with the application.  Everything in this
 * firmware depends on knowing the clock -- the UART divisor, SysTick, the
 * panel's reset delays and every bit-banged bus -- so this configures it
 * outright: HSI (8 MHz) through the PLL to BOARD_SYSCLK_HZ, no prescalers.
 * See clock.c for why the part is not simply left at its 8 MHz reset default.
 */
#ifndef DRIVER_CLOCK_H
#define DRIVER_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Returns the RCC->CFGR value found *before* the change, for logging. */
uint32_t clock_init(void);

/* False when the PLL did not lock and the MCU is still on HSI (8 MHz).  The
 * radio works either way, just slowly, so the boot log says which happened. */
extern bool gClockPllRunning;

#endif /* DRIVER_CLOCK_H */
