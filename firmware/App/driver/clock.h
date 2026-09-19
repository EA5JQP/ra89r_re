/* Put the system clock into a state we can reason about.
 *
 * The stock bootloader runs at its own frequency (it has both 8 MHz and 24 MHz
 * constants and calibrates delays from a value it keeps in RAM) and it hands
 * that configuration over together with the application.  Everything in this
 * firmware depends on knowing the clock -- the UART divisor, SysTick, and the
 * panel's reset/power-up delays -- so the clock is forced back to the
 * datasheet's reset default (HSI 8 MHz, no PLL, no prescalers) instead of
 * assuming it.
 */
#ifndef DRIVER_CLOCK_H
#define DRIVER_CLOCK_H

#include <stdint.h>

/* Returns the RCC->CFGR value found *before* the change, for logging. */
uint32_t clock_init(void);

#endif /* DRIVER_CLOCK_H */
