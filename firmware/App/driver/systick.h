/* SysTick millisecond time base. */
#ifndef DRIVER_SYSTICK_H
#define DRIVER_SYSTICK_H

#include <stdint.h>

void systick_init(void);
uint32_t systick_millis(void);
void systick_delay_ms(uint32_t ms);

#endif /* DRIVER_SYSTICK_H */
