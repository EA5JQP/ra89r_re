/* The beeper -- the DAC tone generator (see driver/beeper.h).
 *
 * The tone math and the sine table live in the header so they can be tested on
 * a PC; this file is the hardware half: the DAC clock and pin, TIM4 as the
 * sample clock, and the update ISR that writes each sample.
 *
 * The stock's own path is TIM4 plus a waveform generator on every update
 * (docs/ra89r_beeper.md); this keeps that shape and drops the stock's
 * note-length table for a plain sine at BOARD_BEEPER_SAMPLE_HZ.  Where the
 * stock's description could not settle the pin (PA4 vs PA5), the driver follows
 * board_pins.h and uses PA4 = DAC_OUT1.
 */
#include "driver/beeper.h"

#include "board.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/systick.h"

/* The DAC is 12-bit and centred so a resting output is 0 V (through the amp's
 * coupling) rather than a DC step at the start and end of a beep. */
#define BEEPER_MIDPOINT 2048u

static uint32_t beeper_phase;
static uint32_t beeper_step;
static bool     beeper_ready;

void beeper_init(void)
{
    if (beeper_ready)
        return;

    /* The DAC is an APB1 peripheral on this part.  (The doc's "RCC_AHB2ENR bit
     * 2" does not match the vendor header, which names RCC_APB1ENR_DACEN; the
     * header wins.) */
    RCC->APB1ENR |= RCC_APB1ENR_DACEN | RCC_APB1ENR_TIM4EN;

    /* PA4 = DAC_OUT1: analog, no drive, no pull. */
    gpio_config_analog(GPIOA, BEEPER_DAC_PIN);

    /* TIM4: one update = one DAC sample, off APB1. */
    TIM4->PSC = 0u;
    TIM4->ARR = (BOARD_APB1_HZ / BOARD_BEEPER_SAMPLE_HZ) - 1u;
    TIM4->CR1 = TIM_CR1_ARPE;
    TIM4->DIER = TIM_DIER_UIE;

    DAC1->CR = DAC_CR_EN1;              /* channel 1, output buffer on (BOFF1 = 0) */
    DAC1->DHR12R1 = BEEPER_MIDPOINT;

    NVIC_EnableIRQ(TIM4_IRQn);
    beeper_ready = true;
}

void beeper_stop(void)
{
    TIM4->CR1 &= ~TIM_CR1_CEN;
    DAC1->DHR12R1 = BEEPER_MIDPOINT;    /* no DC step across the beep */
}

void beeper_play(uint16_t freq_hz, uint16_t duration_ms)
{
    if (!beeper_ready || freq_hz == 0u || duration_ms == 0u)
        return;

    beeper_step  = beeper_osc_step(freq_hz, BOARD_BEEPER_SAMPLE_HZ);
    beeper_phase = 0u;

    TIM4->CNT = 0u;
    TIM4->EGR = TIM_EGR_UG;             /* latch PSC/ARR now */
    TIM4->SR  = 0u;
    TIM4->CR1 |= TIM_CR1_CEN;

    systick_delay_ms(duration_ms);

    beeper_stop();
}

void TIM4_IRQHandler(void)
{
    if (TIM4->SR & TIM_SR_UIF) {
        TIM4->SR = 0u;                  /* write 0 clears the flags */
        DAC1->DHR12R1 = BEEPER_MIDPOINT +
                        (uint32_t)(int32_t)beeper_osc_next(&beeper_phase, beeper_step);
    }
}
