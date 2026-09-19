#include "driver/early.h"

#include <stdint.h>

/* Register-level only, no other driver dependencies: this runs before
 * SystemInit() and before .data/.bss are initialised, so it must not touch
 * initialised globals (string constants in flash are fine) and must not rely
 * on interrupts.
 *
 * GPIO register word indices (vendor GPIO_TypeDef):
 *   0 MODER  1 OTYPER  2 OSPEEDR  3 PUPDR  4 IDR  5 ODR
 *   6 BSRR   7 LCKR    8 AFRL     9 AFRH  10 BRR
 */
#define EARLY_GPIOB      ((volatile uint32_t *)0x48000400u)
#define EARLY_RCC_AHB2EN ((volatile uint32_t *)0x40021034u)   /* AHB2ENR: IOPBEN = bit 3 */
#define EARLY_RCC_APB2EN ((volatile uint32_t *)0x40021018u)   /* APB2ENR: USART1EN = bit 14 */
#define EARLY_USART1     ((volatile uint32_t *)0x40013800u)   /* SR, DR, BRR, CR1 */
#define EARLY_RCC_CR     ((volatile uint32_t *)0x40021000u)   /* CR: HSION 0, HSIRDY 1, PLLON 24 */
#define EARLY_RCC_CFGR   ((volatile uint32_t *)0x40021004u)   /* SW/SWS/HPRE/PPRE1/PPRE2 */

#define MODER 0
#define OSPEEDR 2
#define PUPDR 3
#define AFRL 8

static void early_putc(char c)
{
    while (!(EARLY_USART1[0] & 0x80u))            /* SR.TXE */
        ;
    EARLY_USART1[1] = (uint32_t)(unsigned char)c; /* DR */
}

static void early_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            early_putc('\r');
        early_putc(*s++);
    }
}

void early_boot_banner(void)
{
    /* Force the clock to the reset default (HSI 8 MHz, no PLL, no prescalers):
     * the bootloader hands its own frequency over with the application, so the
     * UART divisor below would otherwise be wrong. */
    uint32_t before = *EARLY_RCC_CFGR;

    *EARLY_RCC_CR |= (1u << 0);
    while (!(*EARLY_RCC_CR & (1u << 1)))
        ;
    *EARLY_RCC_CFGR &= ~0x3u;                      /* SW = HSI */
    *EARLY_RCC_CFGR &= ~(0xFu << 4);               /* HPRE /1 */
    *EARLY_RCC_CFGR &= ~(0x7u << 8);               /* PPRE1 /1 */
    *EARLY_RCC_CFGR &= ~(0x7u << 11);              /* PPRE2 /1 */
    while ((*EARLY_RCC_CFGR & (0x3u << 2)) != 0)
        ;
    *EARLY_RCC_CR &= ~(1u << 24);                  /* PLL off */

    *EARLY_RCC_AHB2EN |= (1u << 3);                /* GPIOB clock */
    *EARLY_RCC_APB2EN |= (1u << 14);               /* USART1 clock */
    (void)*EARLY_RCC_APB2EN;

    /* PB6 = USART1_TX, PB7 = USART1_RX: AF mode, high speed, pull-up on RX */
    EARLY_GPIOB[MODER] = (EARLY_GPIOB[MODER] & ~(3u << 12)) | (2u << 12);
    EARLY_GPIOB[MODER] = (EARLY_GPIOB[MODER] & ~(3u << 14)) | (2u << 14);
    EARLY_GPIOB[OSPEEDR] = (EARLY_GPIOB[OSPEEDR] & ~(3u << 12)) | (3u << 12);
    EARLY_GPIOB[OSPEEDR] = (EARLY_GPIOB[OSPEEDR] & ~(3u << 14)) | (3u << 14);
    EARLY_GPIOB[PUPDR] = (EARLY_GPIOB[PUPDR] & ~(3u << 12)) | (1u << 12);
    EARLY_GPIOB[PUPDR] = (EARLY_GPIOB[PUPDR] & ~(3u << 14)) | (1u << 14);
    /* AFRL holds pins 0-7, one nibble each: pins 6 and 7 are bits 24-31 */
    EARLY_GPIOB[AFRL] = (EARLY_GPIOB[AFRL] & ~0xFF000000u) | 0x22000000u;

    EARLY_USART1[2] = 8000000u / 115200u;         /* BRR, reset-default HSI 8 MHz */
    EARLY_USART1[3] = 0x200Cu;                    /* CR1 = UE | TE | RE */
    early_puts("\n[early] reset handler reached; clock forced to HSI 8 MHz.\n");
    early_puts("[early] the bootloader had left CFGR=");
    {
        /* four hex digits, most significant first -- no printf here */
        int shift;
        const char *digits = "0123456789ABCDEF";
        for (shift = 28; shift >= 0; shift -= 4)
            early_putc(digits[(before >> shift) & 0xFu]);
    }
    early_puts("\n");
}
