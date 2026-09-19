#include "driver/clock.h"

#include "py32f4xx.h"      /* SystemCoreClock */
#include "board_pins.h"

/* Raw registers on purpose: this must not depend on SDK macro spellings.
 * RCC: CR 0x00, CFGR 0x04.  CFGR fields (ST-compatible layout):
 *   SW[1:0] source    SWS[3:2] status    HPRE[7:4]  PPRE1[10:8]  PPRE2[13:11]
 * All-zero prescaler fields mean "not divided". */
#define RCC_CR    (*(volatile uint32_t *)0x40021000u)
#define RCC_CFGR  (*(volatile uint32_t *)0x40021004u)

#define CR_HSION   (1u << 0)
#define CR_HSIRDY  (1u << 1)
#define CR_PLLON   (1u << 24)

uint32_t clock_init(void)
{
    uint32_t before = RCC_CFGR;

    RCC_CR |= CR_HSION;                       /* HSI on (it is, after reset) */
    while (!(RCC_CR & CR_HSIRDY))
        ;

    /* switch to HSI and clear every divider: HPRE, PPRE1, PPRE2 */
    RCC_CFGR &= ~(0x3u << 0);                 /* SW = HSI */
    RCC_CFGR &= ~(0xFu << 4);                 /* HPRE = /1 */
    RCC_CFGR &= ~(0x7u << 8);                 /* PPRE1 = /1 (APB1) */
    RCC_CFGR &= ~(0x7u << 11);                /* PPRE2 = /1 (APB2) */
    while ((RCC_CFGR & (0x3u << 2)) != 0)     /* wait until SWS says HSI */
        ;

    RCC_CR &= ~CR_PLLON;                      /* PLL off if the bootloader left it on */

    SystemCoreClock = BOARD_SYSCLK_HZ;        /* HSI = 8 MHz (datasheet) */
    return before;
}
