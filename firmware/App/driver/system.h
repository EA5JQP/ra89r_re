/* The K1's system driver, on this board (see NOTICE).
 *
 * The K1's version is two calls: a millisecond delay over its SysTick and an
 * empty clock configuration (its clocks are set up by the vendor SystemInit).
 * Here the delay is this repo's SysTick and the clock configuration is the
 * bring-up's, which already runs from main().
 */
#ifndef DRIVER_SYSTEM_H
#define DRIVER_SYSTEM_H

#include <stdint.h>

void SYSTEM_DelayMs(uint32_t Delay);

/* Kept so the imported sources compile; the RA89R's clocks are configured once
 * in the bring-up (driver/clock.c) and nothing switches them at runtime. */
void SYSTEM_ConfigureClocks(void);

#endif /* DRIVER_SYSTEM_H */
