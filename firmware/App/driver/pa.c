#include "driver/pa.h"

#include "board.h"
#include "board_pins.h"
#include "driver/bk4815.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"

static uint16_t s_reg36;
static uint16_t s_reg33 = 0x9000u;
static uint16_t s_compare;

/* The band state pa_select_band() applied.  s_uhf is the VHF/UHF band index (the
 * codeplug's 108-174 / 400-520 bands, split at 280 MHz); s_main is the stock's
 * >134 MHz transceiver split. */
static bool s_uhf;
static bool s_main;

/* The 'F' experiment: which chip-side receive path to apply.  AUTO is the K1
 * application's receive rule (and the one the radio received with); the other
 * modes let the radio settle the stock's own pin-4 activity on the bench.
 * Every mode goes through BK4819_ToggleGpioOut so 0x33's other bits survive. */
enum {
    PA_CHIP_PATH_LEAVE = 0,   /* as BK4819_Init() left it */
    PA_CHIP_PATH_VHF,         /* 0x33 bit 0x04 set, bit 0x08 clear */
    PA_CHIP_PATH_UHF,         /* 0x33 bit 0x08 set, bit 0x04 clear */
    PA_CHIP_PATH_NONE,        /* both cleared */
    PA_CHIP_PATH_AUTO,        /* K1/app rule: VHF pin 4 below 280 MHz, UHF pin 3 */
    PA_CHIP_PATH_MODES
};
static uint8_t s_chip_path = PA_CHIP_PATH_AUTO;

/* The 'B' experiment: the MCU band pin PA0.  The stock's value is LOW. */
static uint8_t s_band_pin_mode = PA_BAND_PIN_STOCK;

bool pa_is_uhf(uint32_t freq_10hz)  { return freq_10hz >= PA_BAND_SPLIT; }
bool pa_is_main(uint32_t freq_10hz) { return freq_10hz > PA_MAIN_SPLIT; }

bool pa_band_is_uhf(void)  { return s_uhf; }
bool pa_band_is_main(void) { return s_main; }

void pa_band_path(void)
{
    /* FUN_08013A70(2) for transmit and (3) for receive both leave PA1 high and
     * PA0 low; PA0 is not the VHF/UHF selector here (pa.h). */
    gpio_write(GPIOA, PA_BAND_PA1_PIN, 1);
    gpio_write(GPIOA, PA_BAND_PA0_PIN, pa_band_pa0_high() ? 1 : 0);
}

bool pa_band_pa0_high(void)
{
    switch (s_band_pin_mode) {
        case PA_BAND_PIN_HIGH: return true;
        case PA_BAND_PIN_AUTO: return s_uhf;
        default:               return false;   /* STOCK */
    }
}

uint8_t pa_band_pin_mode(void) { return s_band_pin_mode; }

void pa_set_band_pin_mode(uint8_t mode)
{
    if (mode < PA_BAND_PIN_MODES)
        s_band_pin_mode = mode;
}

uint8_t pa_chip_path_mode(void) { return s_chip_path; }

void pa_set_chip_path_mode(uint8_t mode)
{
    if (mode < PA_CHIP_PATH_MODES)
        s_chip_path = mode;
}

uint16_t pa_chip_path_reg(void) { return BK4819_ReadRegister(BK4819_REG_33); }

/* Apply the chip-side receive path.  AUTO is the K1 application's rule (and the
 * one the radio received with); the other modes are the 'F' experiment, which
 * walks the individual LNA pins so the radio can settle the stock's own pin-4
 * activity on the bench.  Every mode is a read-modify-write on `0x33`. */
static void pa_apply_chip_path(void)
{
    switch (s_chip_path) {
        case PA_CHIP_PATH_VHF:
            BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA, true);
            BK4819_ToggleGpioOut(BK4819_GPIO3_PIN31_UHF_LNA, false);
            break;
        case PA_CHIP_PATH_UHF:
            BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA, false);
            BK4819_ToggleGpioOut(BK4819_GPIO3_PIN31_UHF_LNA, true);
            break;
        case PA_CHIP_PATH_NONE:
            BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA, false);
            BK4819_ToggleGpioOut(BK4819_GPIO3_PIN31_UHF_LNA, false);
            break;
        case PA_CHIP_PATH_AUTO:
            /* Mirror the K1 application's own receive rule
             * (BK4819_PickRXFilterPathBasedOnFrequency, called by
             * RADIO_SetupRegisters): VHF LNA (pin 4) below 280 MHz, UHF LNA
             * (pin 3) at or above it.  BK4819_ToggleGpioOut is a
             * read-modify-write on the driver's output shadow, so the 0x9000
             * bits BK4819_Init() set survive.  The previous raw write here
             * (0x04 / 0x00) cleared them and stopped 0x67 following a carrier;
             * the stock's own FUN_080137D4 also reads 0x33 back before masking.
             * See docs/ra89r_port.md. */
            BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA, !s_uhf);
            BK4819_ToggleGpioOut(BK4819_GPIO3_PIN31_UHF_LNA, s_uhf);
            break;
        default:                /* LEAVE: touch nothing */
            break;
    }
}

void pa_select_band(uint32_t freq_10hz)
{
    s_uhf  = pa_is_uhf(freq_10hz);
    s_main = pa_is_main(freq_10hz);

    pa_band_path();

    /* The BK4815's band register 0x75, the stock's FUN_08013790: band 1 (0x11)
     * for VHF, band 2 (0x0A) for UHF. */
    bk4815_write_reg(0x75, s_uhf ? 0x0Au : 0x11u);

    pa_apply_chip_path();
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
    /* The stock's transmit word: the band pin (`0x40` VHF / `0x20` UHF, set by
     * FUN_0801BDE8 from the TX band index) plus the T/R pin (`0x02`, set by
     * FUN_08013A70(2)) -- 0x42 / 0x22.  The port used to write 0x0020 alone,
     * which is only the UHF band pin. */
    s_reg33 = (uint16_t)((s_uhf ? PA_REG33_BAND_UHF : PA_REG33_BAND_VHF) | PA_REG33_TR);
    BK4819_WriteRegister(BK4819_REG_33, s_reg33);
    s_reg36 = PA_REG36_ON;
    BK4819_WriteRegister(BK4819_REG_36, s_reg36);
}

void pa_rx_enable(void)
{
    /* Back to the receive path (`pa_apply_chip_path()`, AUTO by default), and
     * no PA-CTL. */
    pa_apply_chip_path();
    s_reg36 = 0x0000u;
    BK4819_WriteRegister(BK4819_REG_36, s_reg36);
    pa_power(0);
}

uint16_t pa_last_reg36(void) { return s_reg36; }
uint16_t pa_last_reg33(void) { return s_reg33; }
uint16_t pa_last_compare(void) { return s_compare; }
