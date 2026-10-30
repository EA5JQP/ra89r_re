#include "driver/pa.h"

#include "board.h"
#include "board_pins.h"
#include "driver/bk4815.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"

static uint16_t s_reg36;
static uint16_t s_reg33 = 0x9000u;
static uint16_t s_compare;
static bool s_bt_audio;

/* The band state pa_select_band() applied.  s_uhf is the VHF/UHF band index (the
 * codeplug's 108-174 / 400-520 bands, split at 280 MHz); s_main is the stock's
 * >134 MHz transceiver split. */
static bool s_uhf;
static bool s_main;

/* Which transceiver is the audio lane (see pa_set_rx_lane()).  The stock's
 * receive-path pin (0x33 bit 4) follows it. */
static bool s_rx_lane_4815;

/* The 'F' experiment: which chip-side receive path to apply.  AUTO is the
 * BK4829's own receive state (both LNA pins clear, the stock's BK4829 branch);
 * the other modes let the radio settle the pin-4/pin-3 choice on the bench.
 * Every mode goes through BK4819_ToggleGpioOut so 0x33's other bits survive. */
enum {
    PA_CHIP_PATH_LEAVE = 0,   /* as BK4819_Init() left it */
    PA_CHIP_PATH_VHF,         /* 0x33 bit 0x04 set, bit 0x08 clear */
    PA_CHIP_PATH_UHF,         /* 0x33 bit 0x08 set, bit 0x04 clear */
    PA_CHIP_PATH_NONE,        /* both cleared */
    PA_CHIP_PATH_AUTO,        /* the BK4829 state: both LNA pins clear */
    PA_CHIP_PATH_MODES
};
static uint8_t s_chip_path = PA_CHIP_PATH_AUTO;

/* The 'B' experiment: the MCU band pin PA0.  The stock's value is LOW. */
static uint8_t s_band_pin_mode = PA_BAND_PIN_STOCK;

/* The 'x' experiment: which chip band/path pin a transmission drives.  AUTO is
 * the stock's rule (the TX frequency's band). */
static uint8_t s_tx_path = PA_TX_PATH_AUTO;

uint8_t pa_tx_path_mode(void) { return s_tx_path; }

void pa_set_tx_path_mode(uint8_t mode)
{
    if (mode < PA_TX_PATH_MODES)
        s_tx_path = mode;
}

uint16_t pa_tx_path_bits(void)
{
    switch (s_tx_path) {
        case PA_TX_PATH_VHF:  return PA_REG33_BAND_VHF;
        case PA_TX_PATH_UHF:  return PA_REG33_BAND_UHF;
        case PA_TX_PATH_BOTH: return PA_REG33_BAND_VHF | PA_REG33_BAND_UHF;
        case PA_TX_PATH_NONE: return 0u;
        default:              return s_uhf ? PA_REG33_BAND_UHF : PA_REG33_BAND_VHF;
    }
}

const char *pa_tx_path_name(void)
{
    static const char *const names[PA_TX_PATH_MODES] = {
        "auto: the frequency's band (default)",
        "force VHF pin 0x40",
        "force UHF pin 0x20",
        "both band pins",
        "neither band pin (T/R only)",
    };

    return names[s_tx_path];
}

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

static void pa_apply_bt_audio(void)
{
    /* FUN_080137D4(mask=4, value=4|0) changes output bit 0x0010 and always
     * clears its paired configuration bit 0x1000.  Do not use the imported K1
     * ToggleGpioOut here: it intentionally leaves that paired bit alone. */
    s_reg33 = BK4819_ToggleGpioOutStock(0x04u, s_bt_audio ? 0x04u : 0u);
}

void pa_set_bt_audio(bool on)
{
    s_bt_audio = on;
    pa_apply_bt_audio();
}

bool pa_bt_audio(void) { return s_bt_audio; }

void pa_set_rx_lane(bool use_4815)
{
    s_rx_lane_4815 = use_4815;

    /* The 'F' experiment modes own the pins themselves; only AUTO re-applies. */
    if (s_chip_path != PA_CHIP_PATH_AUTO)
        return;

    /* Checked on every pass, because the app's radio setup and the scan probes
     * write 0x33 too; written only on a difference (the driver's output shadow
     * is what every 0x33 write goes through), so a settled lane costs no bus
     * traffic.
     *
     * The stock's receive entry clears the band pins 0/1 (`FUN_080093EC` ->
     * `FUN_080137d4(3, 0)`) on *either* lane and selects the receive branch
     * with pin 4.  Pin 0 (0x40) is the VHF *transmit* band path
     * (`FUN_0801BDE8`, pa_tx_enable()), not the K1's "RX_ENABLE": set during
     * receive it costs the BK4829 ~57 dB at 145.5 MHz and ~45 dB at 120.1 MHz
     * (0x67 111 vs 226 and 122 vs 211 with a -40 dBm carrier) and nothing at
     * 446 MHz (152 either way) -- radio 2026-10-30, tinySA.  So it is cleared
     * on both lanes; transmit sets it itself. */
    if (BK4819_IsGpioOutSet(BK4819_GPIO0_PIN28_RX_ENABLE))
        BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);
    if (BK4819_IsGpioOutSet(BK4819_GPIO4_PIN32_VHF_LNA) != use_4815)
        BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA, use_4815);
}

bool pa_rx_lane_is_4815(void) { return s_rx_lane_4815; }

/* Bench toggles (console 'y'/'z'), receive-path lines only.  PA1 is what the
 * stock's receive entry drives low (`FUN_080093EC` -> `FUN_08011b74(GPIOA, 2,
 * 0)`), while this port leaves it high; `0x33` pin 2 (output bit 0x10) is set
 * or cleared by `FUN_08015D44` from a config/codeplug pair the port never
 * writes.  Both are candidates for the antenna path the BK4815 is missing
 * (radio 2026-10-21), so they can be swept without a rebuild. */
void pa_set_pa1(bool high)
{
    gpio_write(GPIOA, PA_BAND_PA1_PIN, high ? 1 : 0);
}

void pa_toggle_pin2(void)
{
    const bool set = (BK4819_ReadRegister(BK4819_REG_33) & 0x10u) != 0u;

    BK4819_ToggleGpioOut((BK4819_GPIO_PIN_t)2, !set);
}

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
            /* The stock's per-lane receive path, re-asserted on every re-entry:
             * pin 4 belongs to the BK4815 receive branch (`FUN_08016CEC` sets
             * it) and is cleared for the BK4829 (`FUN_08016DE8`), and it costs
             * ~16 dB on the BK4829 at VHF when wrongly set -- measured on the
             * radio with console 'F' (pin 4 gave 0x67 = 199, clearing it 232).
             * The lane, not "a VFO names the BK4815", decides: s_rx_lane_4815
             * is set from the selected RX VFO's transceiver by rx_set_audio_
             * source().  BK4819_ToggleGpioOut read-modify-writes the driver's
             * output shadow, so the 0x9000 bits BK4819_Init() set survive, and
             * pin 0 is cleared on either lane: it is the VHF transmit band
             * path, and set during receive it costs the BK4829 ~45-57 dB at
             * VHF (pa_set_rx_lane()).  The stock's BT pin 2 is applied by
             * pa_apply_bt_audio() after this switch.  See docs/ra89r_rfpath.md. */
            BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);
            BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA, s_rx_lane_4815);
            BK4819_ToggleGpioOut(BK4819_GPIO3_PIN31_UHF_LNA, false);
            break;
        default:                /* LEAVE: touch nothing */
            break;
    }
    /* The K1 LNA writes use a different GPIO helper; reapply the stock BT pin
     * and paired-bit state after those shadow writes. */
    pa_apply_bt_audio();
}

void pa_select_band(uint32_t freq_10hz)
{
    s_uhf  = pa_is_uhf(freq_10hz);
    s_main = pa_is_main(freq_10hz);

    pa_band_path();

    /* The BK4815's band register 0x75, the stock's FUN_08013790: band 1 (0x11)
     * for VHF, band 2 (0x0A) for UHF. */
    bk4815_set_band(s_uhf ? 0x0Au : 0x11u);

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
    /* PB14 is **TIM1_CH2N** (datasheet AF4), the *complementary* output, so the
     * channel is enabled with **CC2NE** (CCER bit 6, 0x40) -- which is exactly
     * what the stock does: `FUN_08012f26` -> `FUN_0801de58(TIM1, 4, 4)` sets
     * CC2NE, then BDTR MOE and CR1 CEN.  CC2E (bit 4) does not drive PB14 at
     * all.  The old `CCER &= ~0x30u` cleared the enable bit and the PWM never
     * reached the PA (PB14 sits on a pull-down, so the bias stayed 0). */
    TIM1->CCER = (TIM1->CCER & ~(0x20u | 0x80u)) | 0x40u;  /* CC2NE on, CC2P/CC2NP clear */
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

void pa_tx_enable(uint8_t power)
{
    /* The stock's transmit word: the band pin (`0x40` VHF / `0x20` UHF, set by
     * FUN_0801BDE8 from the TX band index) plus the T/R pin (`0x02`, set by
     * FUN_08013A70(2)) -- 0x42 / 0x22.  The band pin comes from the 'x'
     * override (`pa_tx_path_bits()`), AUTO being the frequency's band. */
    const uint8_t gain = s_uhf ? PA_REG36_GAIN_UHF : PA_REG36_GAIN_VHF;

    /* The stock uses masked GPIO writes, not a replacement 0x33 word, so the
     * paired-bit rule and the other outputs survive.  The 'x' override's band
     * bits (0x40 VHF = pin 0, 0x20 UHF = pin 1), the T/R pin (pin 5) and the
     * BT audio pin (pin 2) are applied through the stock helper. */
    {
        const uint16_t path = pa_tx_path_bits();
        uint8_t band = 0u;

        if ((path & PA_REG33_BAND_VHF) != 0u)
            band |= 0x01u;                  /* pin 0 */
        if ((path & PA_REG33_BAND_UHF) != 0u)
            band |= 0x02u;                  /* pin 1 */

        s_reg33 = BK4819_ToggleGpioOutStock(0x03u, band);
        s_reg33 = BK4819_ToggleGpioOutStock(0x20u, 0x20u);
        s_reg33 = BK4819_ToggleGpioOutStock(0x04u, s_bt_audio ? 0x04u : 0u);
    }

    /* The K1's BK4819_SetupPowerAmplifier: 0x36 = (bias << 8) | PA-CTL | gain.
     * The RA89R's own power knob is the PB14 bias PWM, so the same setting
     * drives it with the stock's arithmetic (`power * ARR / 255`, clamped). */
    s_reg36 = (uint16_t)(((uint16_t)power << 8) | PA_REG36_CTL | gain);
    BK4819_WriteRegister(BK4819_REG_36, s_reg36);

    pa_power((uint16_t)(((uint32_t)power * PA_PWM_ARR) / 255u));
}

void pa_rx_enable(void)
{
    /* The stock clears both TX band pins and T/R before restoring RX. */
    s_reg33 = BK4819_ToggleGpioOutStock(0x03u, 0u);
    s_reg33 = BK4819_ToggleGpioOutStock(0x20u, 0u);
    /* Back to the receive path (AUTO by default), and no PA-CTL. */
    pa_apply_chip_path();
    s_reg36 = 0x0000u;
    BK4819_WriteRegister(BK4819_REG_36, s_reg36);
    pa_power(0);
}

uint16_t pa_last_reg36(void) { return s_reg36; }
uint16_t pa_last_reg33(void) { return s_reg33; }
uint16_t pa_last_compare(void) { return s_compare; }
