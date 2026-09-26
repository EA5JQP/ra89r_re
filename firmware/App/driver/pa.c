#include "driver/pa.h"

#include "board.h"
#include "board_pins.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"

static uint16_t s_reg36;
static uint16_t s_reg33 = 0x9000u;
static uint16_t s_compare;

/* Selected front end.  The RA89R switches its VHF and UHF paths separately and
 * nothing here used to touch them at all; see pa.h for the evidence. */
#define PA_VHF_UHF_SPLIT  28000000u   /* 280 MHz, in 10 Hz units */

static bool s_uhf;

bool pa_is_uhf(uint32_t freq_10hz)
{
    return freq_10hz >= PA_VHF_UHF_SPLIT;
}

void pa_band_path(void)
{
    /* FUN_08013A70(2) for transmit and (3) for receive both leave PA1 high; PA0
     * is the band, and low is the VHF state this board was validated in. */
    gpio_write(GPIOA, PA_BAND_PA1_PIN, 1);
    gpio_write(GPIOA, PA_BAND_PA0_PIN, s_uhf ? 1 : 0);
}

bool pa_band_is_uhf(void)
{
    return s_uhf;
}

void pa_select_band(uint32_t freq_10hz)
{
    s_uhf = pa_is_uhf(freq_10hz);
    pa_band_path();

    /* The chip's own front-end path bits -- 0x33 bit 0x08 for UHF, bit 0x04 for
     * VHF.  The K1's helper drives exactly those two from the frequency. */
    BK4819_PickRXFilterPathBasedOnFrequency(freq_10hz);
}

void pa_init(void)
{
    /* The port and timer clocks, exactly as FUN_080131AC enables them: bit 3 of
     * RCC_AHB2ENR is IOPBEN and bit 11 of RCC_APB2ENR is TIM1EN. */
    gpio_port_clock(GPIOB);
    RCC->APB2ENR |= (1u << 11);                 /* TIM1 */
    (void)RCC->APB2ENR;

    /* PB14, alternate function 4, as FUN_080131AC configures it. */
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(0xFu << 24)) | (PA_PWM_AF << 24);
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 28)) | (2u << 28);      /* AF   */
    GPIOB->OSPEEDR = (GPIOB->OSPEEDR & ~(3u << 28)) | (1u << 28);
    GPIOB->PUPDR = (GPIOB->PUPDR & ~(3u << 28)) | (2u << 28);      /* pull-down */

    /* 144 MHz / (0+1) / (1439+1) = 100 kHz, PWM mode 1 on channel 2.  TIM1 is an
     * advanced timer, so its outputs stay off until BDTR's MOE is set. */
    TIM1->PSC = 0;
    TIM1->ARR = PA_PWM_ARR;
    TIM1->CCMR1 = (TIM1->CCMR1 & ~0xFF00u) | 0x6000u;   /* OC2M = PWM mode 1 */
    TIM1->CCER &= ~0x30u;                                /* CC2P/CC2NE = 0  */
    TIM1->CCR2 = 0;
    TIM1->BDTR |= 0x8000u;                               /* MOE: TIM1 needs it */
    TIM1->EGR = 1u;                                      /* UG */
    TIM1->CR1 |= 1u;                                     /* CEN */

    s_compare = 0;

    gpio_config_output(GPIOA, PA_BAND_PA1_PIN | PA_BAND_PA0_PIN);
    pa_band_path();
}

void pa_power(uint16_t compare)
{
    if (compare > PA_PWM_MAX_DUTY)
        compare = PA_PWM_MAX_DUTY;
    s_compare = compare;
    TIM1->CCR2 = compare;
}

void pa_tx_enable(void)
{
    /* The stock's transmit select: every chip GPIO output cleared and only
     * pin 1 (PA enable) set -- FUN_08013A70(2) -> FUN_080137D4(0x20, 0x20). */
    s_reg33 = 0x0020u;
    BK4819_WriteRegister(BK4819_REG_33, s_reg33);
    s_reg36 = PA_REG36_ON;
    BK4819_WriteRegister(BK4819_REG_36, s_reg36);
}

void pa_rx_enable(void)
{
    /* Whatever the K1 init left, which is what receive has been validated
     * with, and no PA-CTL. */
    s_reg33 = 0x9000u;
    BK4819_WriteRegister(BK4819_REG_33, s_reg33);
    s_reg36 = 0x0000u;
    BK4819_WriteRegister(BK4819_REG_36, s_reg36);
    pa_power(0);
}

uint16_t pa_last_reg36(void) { return s_reg36; }
uint16_t pa_last_reg33(void) { return s_reg33; }
uint16_t pa_last_compare(void) { return s_compare; }
