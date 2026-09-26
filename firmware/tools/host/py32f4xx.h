/* Host device-header test double (NOT part of the firmware build).
 *
 * The PC previews compile the ported K1 application sources, which include
 * "board.h"/"driver/gpio.h"/"py32f0xx.h" and therefore the vendor device
 * header.  That header is ARM-only: CMSIS's __DSB()/__ISB() are inline
 * assembly, so a TU that emits them cannot be assembled for the host.
 *
 * For host builds the include path puts this directory *before* the SDK, so
 * this file answers instead of the real py32f4xx.h.  It provides just the
 * identifiers those sources mention, with the peripherals pointing at address
 * 0: nothing in a preview executes a driver, and any accidental access is a
 * clean crash in the test tool rather than a silent wrong answer.  The target
 * build never sees this file (see firmware/CMakeLists.txt).
 */
#ifndef HOST_PY32F4XX_H
#define HOST_PY32F4XX_H

#include <stddef.h>
#include <stdint.h>

#define __IO volatile

typedef struct {
    __IO uint32_t MODER;
    __IO uint32_t OTYPER;
    __IO uint32_t OSPEEDR;
    __IO uint32_t PUPDR;
    __IO uint32_t IDR;
    __IO uint32_t ODR;
    __IO uint32_t BSRR;
    __IO uint32_t BRR;
    __IO uint32_t LCKR;
    __IO uint32_t AFR[2];
} GPIO_TypeDef;

typedef struct {
    __IO uint32_t ISR;
    __IO uint32_t ICR;
    __IO uint32_t IER;
    __IO uint32_t CR;
    __IO uint32_t CFGR1;
    __IO uint32_t CFGR2;
    __IO uint32_t SMPR;
    __IO uint32_t DR;
    __IO uint32_t CFGR;
} ADC_TypeDef;

typedef struct {
    __IO uint32_t CR1;
    __IO uint32_t CR2;
    __IO uint32_t CR3;
    __IO uint32_t BRR;
    __IO uint32_t GTPR;
    __IO uint32_t RTOR;
    __IO uint32_t RQR;
    __IO uint32_t ISR;
    __IO uint32_t ICR;
    __IO uint32_t RDR;
    __IO uint32_t TDR;
} USART_TypeDef;

typedef struct {
    __IO uint32_t CR;
    __IO uint32_t ICSCR;
    __IO uint32_t CFGR;
    __IO uint32_t PLLCFGR;
    __IO uint32_t CIER;
    __IO uint32_t CIFR;
    __IO uint32_t CICR;
    __IO uint32_t IOPRSTR;
    __IO uint32_t AHBRSTR;
    __IO uint32_t APBRSTR1;
    __IO uint32_t APBRSTR2;
    __IO uint32_t CIER2;
    __IO uint32_t IOPENR;
    __IO uint32_t AHBENR;
    __IO uint32_t APBENR1;
    __IO uint32_t APBENR2;
    __IO uint32_t CSR;
} RCC_TypeDef;

#define GPIOA ((GPIO_TypeDef *)&host_gpio_scratch[0])
#define GPIOB ((GPIO_TypeDef *)&host_gpio_scratch[1])
#define GPIOC ((GPIO_TypeDef *)&host_gpio_scratch[2])
#define GPIOD ((GPIO_TypeDef *)&host_gpio_scratch[3])
#define GPIOF ((GPIO_TypeDef *)&host_gpio_scratch[5])

/* The peripherals point at a scratch struct instead of address 0: the imported
 * sources do reach some register-level helpers -- `driver/backlight.c` writes
 * its pin through the inline `gpio_write` -- and a preview should run through
 * them rather than fault.  Nothing reads a value back. */
extern GPIO_TypeDef host_gpio_scratch[6];

#define ADC1  ((ADC_TypeDef *)0u)
#define USART1 ((USART_TypeDef *)0u)
#define USART2 ((USART_TypeDef *)0u)
#define RCC   ((RCC_TypeDef *)0u)

/* The one CMSIS call the imported application makes (menu "reset" items). */
void host_nvic_system_reset(void);
#define NVIC_SystemReset() host_nvic_system_reset()

extern uint32_t SystemCoreClock;

#endif /* HOST_PY32F4XX_H */
