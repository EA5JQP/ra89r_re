#include "board.h"
#include "board_pins.h"
#include "driver/systick.h"

/* core_cm4.h (CMSIS) provides SysTick_Config(); its handler is defined here
 * because the startup file's vector table expects SysTick_Handler. */

static volatile uint32_t s_millis;
static int s_systick_dead;

void SysTick_Handler(void)
{
    s_millis++;
}

void systick_init(void)
{
    /* 1 ms tick; SystemCoreClock is maintained by system_py32f403.c */
    if (SysTick_Config(SystemCoreClock / 1000u) != 0)
        s_systick_dead = 1;
}

/* Bring-up safety net: if SysTick could not be set up the interrupt never
 * fires, so the driver's delays would hang forever and the panel would stay
 * black with no clue why.  Fall back to a coarse busy loop. */
static void busy_delay_ms(uint32_t ms)
{
    volatile uint32_t n;

    while (ms--) {
        for (n = 0; n < (BOARD_SYSCLK_HZ / 1000u) / 4u; n++)
            __NOP();
        s_millis++;                 /* keep the uptime moving too */
    }
}

uint32_t systick_millis(void)
{
    return s_millis;
}

void systick_delay_ms(uint32_t ms)
{
    uint32_t start = s_millis;

    if (s_systick_dead) {
        busy_delay_ms(ms);
        return;
    }
    while ((uint32_t)(s_millis - start) < ms)
        __WFI();
}

/* The K1's SYSTICK_DelayUs, over this port's SysTick.  SysTick counts down from
 * LOAD to 0 every millisecond, so elapsed ticks are accumulated across wraps.
 * Only used for short (microsecond) waits, so the accumulation cannot overflow
 * a uint32_t before the target is reached. */
void SYSTICK_DelayUs(uint32_t us)
{
    const uint32_t ticks_per_us = SystemCoreClock / 1000000u;
    const uint32_t ticks = us * ticks_per_us;
    const uint32_t load  = SysTick->LOAD;
    uint32_t prev;
    uint32_t elapsed = 0;

    if (ticks == 0u)
        return;

    prev = SysTick->VAL;
    while (elapsed < ticks) {
        const uint32_t now = SysTick->VAL;

        if (now < prev)
            elapsed += prev - now;                 /* counting down */
        else if (now > prev)
            elapsed += prev + (load - now);        /* wrapped 0 -> LOAD */
        prev = now;
    }
}
