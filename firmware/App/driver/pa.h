/* The transmit power amplifier and its band path, from the stock image.
 *
 * Three things live here, all taken from `docs/ra89r_rfpath.md`:
 *
 * 1. **The bias is a PWM.**  The stock's "Pow AdjData" path
 *    (`FUN_08016A2C` -> `FUN_0801830C` -> `FUN_0801BDE8` -> `FUN_08018A88` ->
 *    `FUN_080167B4` -> `FUN_0801306E`) programs **TIM1 channel 2**, whose pin
 *    `FUN_080131AC` configures as `GPIOB` mask `0x4000` with alternate function
 *    4 -- i.e. **`PB14`**.  The period is ARR 1439 (a 100 kHz PWM from the stock's
 *    144 MHz APB2), the compare is `power * ARR / 255` clamped to `ARR/2`, and 0
 *    in receive.  There is only one PWM: both config structs are TIM1 and every
 *    call uses channel 2, so it is a **common** bias line, not one per PA.
 *
 * 2. **The band/path select is the BK4829's `0x33`.**  `FUN_080137D4(mask,
 *    value)` maps mask bit `n` to output bit `0x40 >> n` (the K1's
 *    `BK4819_GPIO_PIN_t` pin).  The stock uses:
 *      * **pin 0 (`0x40`)** and **pin 1 (`0x20`)** -- the **TX** VHF / UHF PA
 *        paths, set by `FUN_0801BDE8` from the TX frequency's band index;
 *      * **pin 4 (`0x04`)** -- part of the **BK4815** receive branch: set by
 *        `FUN_08016CEC` (the > 134 MHz branch) and cleared by `FUN_08016DE8`
 *        (the BK4829 branch).  The stock picks the branch from the flag at
 *        `0x20000303` (1 = BK4829 at <= 134 MHz, 0 = BK4815 above it), so the
 *        BK4829's own receive state leaves pin 4 **clear**;
 *      * **pin 5 (`0x02`)** -- the **T/R** (PA enable), set by
 *        `FUN_08013A70(2)` and cleared by `(3)`.
 *    So the stock's transmit word is `0x40|0x02 = 0x42` on VHF and
 *    `0x20|0x02 = 0x22` on UHF.  The BK4829's receive state clears pin 4 and
 *    the TX pins; setting pin 4 costs ~16 dB on the BK4829 at VHF (measured).
 *
 * 3. **The amplifier enable is the chip's `0x36`** -- bit 7 (PA-CTL) with a bias
 *    in bits 15:8.  The stock's own transmit path never writes `0x36`; the K1
 *    sets it in `BK4819_SetupPowerAmplifier`, and our imported `BK4819_TxOn_Beep`
 *    wrote it to **0**, which is why the carrier was real and unamplified until
 *    `0x36` was set.
 *
 * The MCU band pins are `PA1 = 1, PA0 = 0` for both bands (`FUN_08013A70(2)` for
 * transmit and `(3)` for receive); PA0 is *not* the VHF/UHF selector here.  Values
 * measured on the radio: `0x36 = 0x8822`, PWM compare 128 -> voice heard on a
 * second receiver.
 */
#ifndef DRIVER_PA_H
#define DRIVER_PA_H

#include <stdint.h>
#include <stdbool.h>

/* PB14 / TIM1_CH2.  ARR 1439 at 100 kHz; the compare is clamped to ARR/2, which
 * is what the stock's FUN_080167B4 does. */
#define PA_PWM_ARR       1439u
#define PA_PWM_MAX_DUTY  (PA_PWM_ARR / 2u)

/* The chip's PA-CTL and bias word: bit 7 enables PA-CTL, bits 15:8 are the
 * bias, bits 5:0 the gain tuning.  0x8822 is the measured working value. */
#define PA_REG36_BIAS    0x8800u
#define PA_REG36_CTL     0x0080u
#define PA_REG36_GAIN    0x0022u
#define PA_REG36_ON      (PA_REG36_BIAS | PA_REG36_CTL | PA_REG36_GAIN)

/* The stock's two frequency splits, in the codec's 10 Hz units. */
#define PA_MAIN_SPLIT    13400000u  /* 134.0 MHz: <=134 the BK4829 branch, >134 the BK4815 */
#define PA_BAND_SPLIT    28000000u  /* 280 MHz: between the codeplug bands 174/400 */

/* BK4829 register 0x33 output bits (mask bit n -> output 0x40 >> n). */
#define PA_REG33_BAND_VHF 0x0040u   /* pin 0: the VHF TX path (FUN_0801BDE8) */
#define PA_REG33_BAND_UHF 0x0020u   /* pin 1: the UHF TX path */
#define PA_REG33_RX_MAIN  0x0004u   /* pin 4: the BK4815 receive branch's bit
                                     * (FUN_08016CEC sets it, FUN_08016DE8
                                     * clears it); the BK4829 leaves it clear */
#define PA_REG33_TR       0x0002u   /* pin 5: T/R, the PA enable (FUN_08013A70) */

/* Bring up the PWM pin and timer and park the band-path pins; compare 0, so the
 * PA is unbiased until a transmission asks for power. */
void pa_init(void);

/* The bias PWM compare, 0 .. PA_PWM_MAX_DUTY.  Higher is more bias. */
void pa_power(uint16_t compare);

/* Park the band-path pins at the stock's value (PA1 = 1, PA0 = 0). */
void pa_band_path(void);

/* True when the frequency is in the main band (> 134.0 MHz): the BK4815 branch
 * of the stock's T/R path. */
bool pa_is_main(uint32_t freq_10hz);

/* True when the frequency is UHF (>= 280 MHz), i.e. the codeplug's band 2. */
bool pa_is_uhf(uint32_t freq_10hz);

/* Select the front end for a frequency (10 Hz units): the MCU band pins, the
 * BK4815 band register `0x75`, and the chip's RX path bit.  Called for both
 * directions; `pa_tx_enable()` then applies the TX band pin. */
void pa_select_band(uint32_t freq_10hz);

/* The band currently selected, for the console. */
bool pa_band_is_uhf(void);
bool pa_band_is_main(void);

/* The actual level PA0 is driven to, after the band-pin mode is applied. */
bool pa_band_pa0_high(void);

/* Which chip-side front-end selection is applied ('F').  AUTO is the BK4829's
 * own receive state -- both LNA pins clear, the stock's BK4829 branch -- and is
 * a read-modify-write, so register `0x33`'s `0x9000` bits survive. */
uint8_t  pa_chip_path_mode(void);
void     pa_set_chip_path_mode(uint8_t mode);
uint16_t pa_chip_path_reg(void);

/* Which level the MCU band pin PA0 is driven to ('B').  The stock takes PA0 from
 * its config, not the frequency, and on this codeplug that is PA0 = 0 for both
 * bands; the other modes are the experiment that settles it on the radio. */
enum {
    PA_BAND_PIN_STOCK = 0,  /* PA0 low whatever the band (the stock's value) */
    PA_BAND_PIN_HIGH,       /* PA0 high whatever the band */
    PA_BAND_PIN_AUTO,       /* PA0 high for UHF, low for VHF (an inference) */
    PA_BAND_PIN_MODES
};
uint8_t pa_band_pin_mode(void);
void    pa_set_band_pin_mode(uint8_t mode);

/* Chip side, transmit: the band pin (`0x40` VHF / `0x20` UHF) plus the T/R pin
 * (`0x02`), and `0x36 = PA_REG36_ON`. */
void pa_tx_enable(void);

/* Chip side, receive: re-apply `pa_apply_chip_path()` (AUTO = the K1/app LNA
 * rule), `0x36 = 0`, compare 0. */
void pa_rx_enable(void);

/* What the driver last wrote, for a host test or the console. */
uint16_t pa_last_reg36(void);
uint16_t pa_last_reg33(void);
uint16_t pa_last_compare(void);

#endif /* DRIVER_PA_H */
