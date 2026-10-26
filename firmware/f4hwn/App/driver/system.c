#include "driver/system.h"

#include "driver/systick.h"

void SYSTEM_DelayMs(uint32_t Delay)
{
    systick_delay_ms(Delay);
}

void SYSTEM_ConfigureClocks(void)
{
}
