/* Put the system clock into a state we can reason about, and make it fast.
 *
 * Two things force this file to exist.  The stock bootloader leaves its own
 * clock configuration behind (it has both 8 MHz and 24 MHz constants and
 * calibrates delays from a value it keeps in RAM), and everything here depends
 * on knowing the clock: the UART divisor, SysTick, every `SYSTEM_DelayMs` and
 * the panel's reset delays.  And the port used to stay at the HSI reset default
 * -- 8 MHz with the PLL off, "no PLL bring-up risk" -- which made every
 * bit-banged bus, every busy-wait and every display write six to twelve times
 * slower than the part allows.  The PY32F403 is a Cortex-M4F rated to 155 MHz;
 * the UV-K1 firmware this was ported from runs its smaller PY32F071 at 48 MHz.
 *
 * So: HSI (8 MHz, no crystal needed) through the PLL to BOARD_SYSCLK_HZ, with
 * the flash wait states the vendor's own table asks for:
 *
 *   0 WS <= 28 MHz | 1 WS 28..60 | 3 WS 60..90 | 4 WS 90..120 | 5 WS 120..140
 *
 * No prescalers: HCLK = PCLK1 = PCLK2 = SYSCLK, which is what BOARD_APB2_HZ
 * says and what the UART divisor and SysTick are built from.
 */
#include "driver/clock.h"

#include "py32f4xx.h"      /* SystemCoreClock */
#include "board_pins.h"

/* Raw registers on purpose: this must not depend on SDK macro spellings.
 * RCC: CR 0x00, CFGR 0x04; FLASH: ACR 0x00.  CFGR fields (ST-compatible
 * layout): SW[1:0] source, SWS[3:2] status, HPRE[7:4], PPRE1[10:8],
 * PPRE2[13:11], PLLSRC 16, PLLXTPRE 17, PLLMULL[21:18] + 29.
 * All-zero prescaler fields mean "not divided". */
#define RCC_CR    (*(volatile uint32_t *)0x40021000u)
#define RCC_CFGR  (*(volatile uint32_t *)0x40021004u)
#define FLASH_ACR (*(volatile uint32_t *)0x40022000u)

#define CR_HSION   (1u << 0)
#define CR_HSIRDY  (1u << 1)
#define CR_PLLON   (1u << 24)
#define CR_PLLRDY  (1u << 25)

#define CFGR_SW       (0x3u << 0)
#define CFGR_SWS      (0x3u << 2)
#define CFGR_HPRE     (0xFu << 4)
#define CFGR_PPRE1    (0x7u << 8)
#define CFGR_PPRE2    (0x7u << 11)
#define CFGR_PLLSRC   (1u << 16)
#define CFGR_PLLXTPRE (1u << 17)
#define CFGR_PLLMULL  (0xFu << 18)
#define CFGR_PLLMULL4 (1u << 29)

#define ACR_LATENCY   (0xFu)

#define SW_HSI   0u
#define SW_PLL   2u
#define SWS_HSI  0u
#define SWS_PLL  8u

/* The field above covers the multiplier up to x33: from x34 the SDK's own
 * defines also set bit 30, a fifth bit this code does not claim to understand
 * (and there is no reason to go near it -- x33 from HSI is 264 MHz). */
_Static_assert(BOARD_PLL_MUL >= 2u && BOARD_PLL_MUL <= 33u,
               "BOARD_PLL_MUL outside the multiplier field this code encodes");

/* A PLL that never locks must not hang the radio with the console dead: every
 * wait here is bounded, and the failure path leaves the MCU on HSI at 8 MHz --
 * slow, but alive and able to say so. */
#define WAIT_LOOPS 200000u

static int wait_for(volatile uint32_t *reg, uint32_t mask, uint32_t want)
{
    uint32_t i;

    for (i = 0; i < WAIT_LOOPS; i++) {
        if ((*reg & mask) == want)
            return 0;
    }
    return -1;
}

/* Set by clock_init: whether SYSCLK is really the PLL. */
bool gClockPllRunning;

uint32_t clock_init(void)
{
    const uint32_t before = RCC_CFGR;
    uint32_t cfgr;
    uint32_t mul;

    gClockPllRunning = false;

    /* HSI on and selected first: the PLL is then programmed from a source we
     * know, whatever the bootloader left running. */
    RCC_CR |= CR_HSION;
    (void)wait_for(&RCC_CR, CR_HSIRDY, CR_HSIRDY);

    /* Switch to HSI and clear every divider: SW, HPRE, PPRE1, PPRE2.  ADCPRE
     * is left alone -- keypad.c sets it, and it is not ours to guess. */
    RCC_CFGR = (RCC_CFGR & ~(CFGR_SW | CFGR_HPRE | CFGR_PPRE1 | CFGR_PPRE2)) | SW_HSI;
    (void)wait_for(&RCC_CFGR, CFGR_SWS, SWS_HSI);

    /* PLL off while it is reprogrammed. */
    RCC_CR &= ~CR_PLLON;
    (void)wait_for(&RCC_CR, CR_PLLRDY, 0u);

    /* Flash wait states for the target, set *before* the clock goes up. */
    FLASH_ACR = (FLASH_ACR & ~ACR_LATENCY) | BOARD_FLASH_WS;

    /* PLL = HSI x BOARD_PLL_MUL.  The field is the multiplier minus two, four
     * bits of it in PLLMULL[3:0] and the fifth in bit 29. */
    mul = (BOARD_PLL_MUL - 2u) & 0x1Fu;
    cfgr = RCC_CFGR & ~(CFGR_PLLSRC | CFGR_PLLXTPRE | CFGR_PLLMULL | CFGR_PLLMULL4);
    cfgr |= (mul & 0x0Fu) << 18;
    if (mul & 0x10u)
        cfgr |= CFGR_PLLMULL4;
    RCC_CFGR = cfgr;

    RCC_CR |= CR_PLLON;
    if (wait_for(&RCC_CR, CR_PLLRDY, CR_PLLRDY) == 0) {
        /* Switch SYSCLK to the PLL and wait for the status field to agree. */
        RCC_CFGR = (RCC_CFGR & ~CFGR_SW) | SW_PLL;
        if (wait_for(&RCC_CFGR, CFGR_SWS, SWS_PLL) == 0)
            gClockPllRunning = true;
        else
            RCC_CFGR = (RCC_CFGR & ~CFGR_SW) | SW_HSI;   /* back to HSI */
    }

    SystemCoreClock = gClockPllRunning ? BOARD_SYSCLK_HZ : 8000000u;
    return before;
}
