/* The RF front-end board facts of the RA89R, trimmed to the single-BK4829
 * layout of the fresh port.
 *
 * Source of truth: the validated `firmware/App/driver/pa.c` of the old port
 * (docs/ra89r_rfpath.md) -- the band pins, the receive-path GPIO pins and the
 * PA bias PWM are radio-measured facts.  Everything dual-lane (the BK4815
 * lane, the 'F' bench experiment modes) died with the BK4815 (user decision
 * 2026-10-26); the AUTO rule below is what the old port's own code ends up
 * doing with lane = BK4829, which is the only lane this port has.
 *
 * Timers: PA bias PWM facts are unchanged.  External pins: PA0/PA1 band path
 * (both left PA1 high / PA0 low by the stock's own TX and RX entries), PB14 =
 * TIM1_CH2N (AF4) bias PWM.
 */
#include "driver/pa.h"

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "board_pins.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"

static bool s_uhf;
static bool s_main;

bool pa_band_is_main(void) { return s_main; }

/* The stock's RX entries leave PA1 high and PA0 low, so the band pin is the
 * only one that follows the frequency (see old `app/tx.c` and docs). */
void pa_band_path(void)
{
    gpio_write(GPIOA, PA_BAND_PA1_PIN, 1);
    gpio_write(GPIOA, PA_BAND_PA0_PIN, s_uhf ? 1 : 0);
}

/* The old port's single-lane rule, with the lane welded to the BK4829.  The
 * board's receive-path facts: chip GPIO0 = the BK4829 path, chip GPIO4 = the
 * (dead) BK4815 one, so the fresh port sets the pair once per band change via
 * the	K1's `PickRXFilter` hook and never again until something reconfigures
 * the path. */
#define PA_CHIP_PATH_AUTO 0u
#define PA_CHIP_PATH_MODES 1u

static uint8_t s_chip_path = PA_CHIP_PATH_AUTO;
static bool    s_rx_lane_4815 = false;

void pa_set_rx_lane(bool use_4815)
{
    (void)use_4815;   /* only one lane in the fresh port */

    if (s_chip_path != PA_CHIP_PATH_AUTO)
        return;

    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
    BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA, false);
}

bool pa_rx_lane_is_4815(void) { return false; }

/* The BK4815's band register 0x75 died with the BK4815 (single chip). */
void pa_apply_chip_path(void)
{
    if (s_chip_path != PA_CHIP_PATH_AUTO)
        return;

    /* The fresh port's own receive entry sets GPIO0 and clears GPIO4 through
     * `pa_set_rx_lane()`; the band pins follow the frequency below. */
    pa_band_path();
}

void pa_select_band(uint32_t freq_10hz)
{
    s_uhf  = (freq_10hz >= PA_BAND_SPLIT);
    s_main = s_uhf;     /* both bands above 280 MHz */

    pa_apply_chip_path();
}

/* --- the PA bias PWM (TIM1 channel 2N on PB14), the validated old facts --- */

void pa_init(void)
{
    gpio_port_clock(GPIOB);
    RCC->APB2ENR |= (1u << 11);                 /* TIM1 */
    (void)RCC->APB2ENR;

    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(0xFu << 24)) | (PA_PWM_AF << 24);
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 28)) | (2u << 28);      /* AF */
    GPIOB->OSPEEDR = (GPIOB->OSPEEDR & ~(3u << 28)) | (1u << 28);
    GPIOB->PUPDR = (GPIOB->PUPDR & ~(3u << 28)) | (2u << 28);      /* pull-down */

    TIM1->PSC = 0;
    TIM1->ARR = PA_PWM_ARR;
    TIM1->CCMR1 = (TIM1->CCMR1 & ~0xFF00u) | 0x6000u;   /* OC2M = PWM mode 1 */
    /* PB14 is TIM1_CH2N (AF4): the channel is CC2NE (CCER bit 6). */
    TIM1->CCER = (TIM1->CCER & ~(0x20u | 0x80u)) | 0x40u;
    TIM1->CCR2 = 0;

    TIM1->BDTR |= (1u << 15);                   /* MOE */
    TIM1->CR1 |= 1u;                            /* CEN */
}
