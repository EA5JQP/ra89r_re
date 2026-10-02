/* The beeper -- the DAC tone generator, played by DMA (see driver/beeper.h).
 *
 * The tone math and the sine table live in the header so they can be tested on
 * a PC; this file is the hardware half.  TIM4 is the sample clock: its update
 * event is TRGO, the DAC converts on that trigger, and DMA1 channel 3 feeds the
 * DAC the next table entry -- so there is no per-sample interrupt, and the CPU
 * only sleeps (`WFI`) for the beep's duration.
 *
 * The pieces are the vendor's own DAC+DMA pattern
 * (Projects/PY32F403-STK/Example/DAC/DAC_TIMTrigger_DMA): the DAC's trigger
 * select is TIM4 TRGO, the DAC raises the DMA request, and the channel is
 * mapped to DAC1 through SYSCFG.  That map is a crossbar, so the DAC does not
 * have to share: DMA1 channel 1 is the keypad's ADC and channel 2 the
 * backlight's TIM7, and the DAC takes channel 3.
 */
#include "driver/beeper.h"

#include "board.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/systick.h"

#define BEEPER_MIDPOINT 2048u

/* The DMA source: the 12-bit DAC words, 2048 +/- the sine.  32-bit words, as
 * the vendor's example uses (WORD memory and peripheral alignment). */
static uint32_t s_dma_buf[BEEPER_SINE_LEN];

static bool beeper_ready;

/* DMA1 channel 3 -> DAC1.  SYSCFG->CFGR[2] holds one 7-bit request field per
 * DMA1 channel: channel 1 is bits[6:0] (ADC1, the keypad) and channel 2 is
 * bits[14:8] (TIM7, the backlight), so channel 3 is bits[22:16].  The request
 * number is 3 (DMA_CHANNEL_MAP_DAC1 in the SDK). */
#define BEEPER_DMA_MAP_SHIFT 16u
#define BEEPER_DMA_MAP_DAC1  3u

void beeper_init(void)
{
    unsigned i;

    if (beeper_ready)
        return;

    for (i = 0; i < BEEPER_SINE_LEN; i++)
        s_dma_buf[i] = BEEPER_MIDPOINT + (uint32_t)(int32_t)beeper_sine[i];

    /* Clocks: DAC and TIM4 on APB1, DMA1 on AHB1, SYSCFG for the request map. */
    RCC->APB1ENR |= RCC_APB1ENR_DACEN | RCC_APB1ENR_TIM4EN;
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;

    /* PA4 = DAC_OUT1: analog, no drive, no pull. */
    gpio_config_analog(GPIOA, BEEPER_DAC_PIN);

    /* TIM4: one update = one sample, TRGO = update; no interrupt. */
    TIM4->PSC  = 0u;
    TIM4->ARR  = beeper_arr(1000u, BOARD_APB1_HZ);      /* a sane resting tone */
    TIM4->CR1  = TIM_CR1_ARPE;
    TIM4->CR2  = TIM_CR2_MMS_1;                         /* MMS = update */
    TIM4->DIER = 0u;

    /* DMA1 channel 3, memory -> DAC1->DHR12R1, circular, 32-bit both sides. */
    SYSCFG->CFGR[2] = (SYSCFG->CFGR[2] & ~(0x7Fu << BEEPER_DMA_MAP_SHIFT)) |
                      ((uint32_t)BEEPER_DMA_MAP_DAC1 << BEEPER_DMA_MAP_SHIFT);

    DMA1_Channel3->CCR   = 0u;
    DMA1_Channel3->CPAR  = (uint32_t)&DAC1->DHR12R1;
    DMA1_Channel3->CMAR  = (uint32_t)s_dma_buf;
    DMA1_Channel3->CNDTR = BEEPER_SINE_LEN;
    DMA1_Channel3->CCR   = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_CIRC |
                           DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1 | DMA_CCR_PL;

    /* DAC: channel 1, output buffer on, TIM4 TRGO trigger, DMA request. */
    DAC1->CR = DAC_CR_EN1 | DAC_CR_TEN1 | DAC_CR_TSEL1_2 | DAC_CR_TSEL1_0 |
               DAC_CR_DMAEN1;
    DAC1->DHR12R1 = BEEPER_MIDPOINT;

    beeper_ready = true;
}

void beeper_stop(void)
{
    TIM4->CR1 &= ~TIM_CR1_CEN;
    DMA1_Channel3->CCR &= ~DMA_CCR_EN;
    DAC1->DHR12R1 = BEEPER_MIDPOINT;    /* no DC step across the beep */
}

void beeper_play(uint16_t freq_hz, uint16_t duration_ms)
{
    if (!beeper_ready || freq_hz == 0u || duration_ms == 0u)
        return;

    /* The tone comes from the clock: DMA plays the whole table once per timer
     * period, so the frequency is the update rate divided by the table length. */
    TIM4->ARR = beeper_arr(freq_hz, BOARD_APB1_HZ);

    /* Restart the DMA at the top of the table, then start the clock. */
    DMA1_Channel3->CCR  &= ~DMA_CCR_EN;
    DMA1_Channel3->CNDTR = BEEPER_SINE_LEN;
    DMA1_Channel3->CMAR  = (uint32_t)s_dma_buf;
    DMA1_Channel3->CCR  |= DMA_CCR_EN;

    TIM4->CNT = 0u;
    TIM4->EGR = TIM_EGR_UG;             /* latch PSC/ARR now */
    TIM4->SR  = 0u;
    TIM4->CR1 |= TIM_CR1_CEN;

    systick_delay_ms(duration_ms);

    beeper_stop();
}
