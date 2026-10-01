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
    __IO uint32_t AHB2ENR;
    __IO uint32_t APB2ENR;
    __IO uint32_t AHB1ENR;      /* the names the drivers use (keypad, backlight) */
    __IO uint32_t APB1ENR;
    __IO uint32_t CSR;
} RCC_TypeDef;

typedef struct {
    __IO uint32_t CR1;
    __IO uint32_t CR2;
    __IO uint32_t SMCR;
    __IO uint32_t DIER;
    __IO uint32_t SR;
    __IO uint32_t EGR;
    __IO uint32_t CCMR1;
    __IO uint32_t CCMR2;
    __IO uint32_t CCER;
    __IO uint32_t CNT;
    __IO uint32_t PSC;
    __IO uint32_t ARR;
    __IO uint32_t RCR;
    __IO uint32_t CCR1;
    __IO uint32_t CCR2;
    __IO uint32_t CCR3;
    __IO uint32_t CCR4;
    __IO uint32_t BDTR;
} TIM_TypeDef;

typedef struct {
    __IO uint32_t ISR;
    __IO uint32_t IFCR;
} DMA_TypeDef;

typedef struct {
    __IO uint32_t CCR;
    __IO uint32_t CNDTR;
    __IO uint32_t CPAR;
    __IO uint32_t CMAR;
} DMA_Channel_TypeDef;

typedef struct {
    __IO uint32_t CFGR[5];
} SYSCFG_TypeDef;

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

/* `driver/pa.c` names TIM1; a preview never calls `pa_init()`, so the timer
 * only has to exist for the reference to link. */
extern TIM_TypeDef host_tim_scratch;
#define TIM1 (&host_tim_scratch)

/* `driver/backlight.c` reaches TIM7, DMA1 channel 2 and SYSCFG, and the preview
 * *does* call `BACKLIGHT_InitHardware()` (its logic is host-tested), so these
 * point at scratch too.  RCC likewise: the backlight enables its clocks through
 * it, so it can no longer be the null that only faulted on an accidental
 * access. */
extern TIM_TypeDef        host_tim7_scratch;
extern DMA_TypeDef        host_dma_scratch;
extern DMA_Channel_TypeDef host_dma_ch2_scratch;
extern SYSCFG_TypeDef     host_syscfg_scratch;
extern RCC_TypeDef        host_rcc_scratch;

#define TIM7          (&host_tim7_scratch)
#define DMA1          (&host_dma_scratch)
#define DMA1_Channel2 (&host_dma_ch2_scratch)
#define SYSCFG        (&host_syscfg_scratch)

/* The register bits the backlight driver uses (standard values; see the real
 * py32f403xB.h). */
#define DMA_CCR_EN           (1u << 0)
#define DMA_CCR_DIR          (1u << 4)      /* 1 = memory -> peripheral */
#define DMA_CCR_CIRC         (1u << 5)
#define DMA_CCR_MINC         (1u << 7)
#define DMA_CCR_PSIZE_1      (1u << 9)
#define DMA_CCR_MSIZE_1      (1u << 11)
#define DMA_CCR_PL           (2u << 12)
#define DMA_IFCR_CGIF2       (1u << 4)
#define TIM_CR1_CEN          (1u << 0)
#define TIM_CR1_ARPE         (1u << 7)
#define TIM_DIER_UDE         (1u << 8)
#define TIM_EGR_UG           (1u << 0)
#define RCC_APB1ENR_TIM7EN   (1u << 5)
#define RCC_AHB1ENR_DMA1EN   (1u << 0)
#define RCC_APB2ENR_SYSCFGEN (1u << 0)

#define ADC1  ((ADC_TypeDef *)0u)
#define USART1 ((USART_TypeDef *)0u)
#define USART2 ((USART_TypeDef *)0u)
#define RCC   (&host_rcc_scratch)

/* The one CMSIS call the imported application makes (menu "reset" items). */
void host_nvic_system_reset(void);
#define NVIC_SystemReset() host_nvic_system_reset()

extern uint32_t SystemCoreClock;

#endif /* HOST_PY32F4XX_H */
