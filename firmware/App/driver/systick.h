/* SysTick millisecond time base. */
#ifndef DRIVER_SYSTICK_H
#define DRIVER_SYSTICK_H

#include <stdint.h>

void systick_init(void);
uint32_t systick_millis(void);
void systick_delay_ms(uint32_t ms);

/* Busy-wait `us` microseconds over the SysTick counter.  The imported F4HWN
 * fast scan (app/chFrScanner.c) and the K1's bit-banged buses use it; it is the
 * K1's `SYSTICK_DelayUs` with this port's 1 ms SysTick reload. */
void SYSTICK_DelayUs(uint32_t us);

#endif /* DRIVER_SYSTICK_H */
