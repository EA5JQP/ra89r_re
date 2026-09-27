/* The transmit power amplifier and its band path, as measured on this board.
 *
 * Two halves, and neither of them is in the stock's own RX/TX register set:
 *
 * 1. **The bias is a PWM.**  The stock's "Pow AdjData" path
 *    (`FUN_08016A2C` -> `FUN_0801830C` -> `FUN_0801BDE8` -> `FUN_08018A88` ->
 *    `FUN_080167B4` -> `FUN_0801306E`) programs **TIM1 channel 2**, whose pin
 *    `FUN_080131AC` configures as `GPIOB` mask `0x4000` with alternate function
 *    4 -- i.e. **`PB14`**.  The period comes from the boot argument 100 through
 *    `1.44e8 / 100 / 1000 = 1440`, so PSC 0, ARR 1439: a 100 kHz PWM from the
 *    144 MHz APB2 clock.  The compare is the codeplug's power value, clamped to
 *    `ARR/2`, and 0 in receive.  `PB14` is AF-configured, not driven through
 *    `BSRR`, which is why no GPIO-write sweep ever found it.
 *
 * 2. **The amplifier enable is the chip's register `0x36`** -- bit 7 (PA-CTL)
 *    with a bias in bits 15:8 -- together with the chip's GPIO pin 1 in `0x33`.
 *    The stock's own transmit path writes only the `0x33` pin
 *    (`FUN_08013A70(2)` -> `FUN_080137D4(0x20, 0x20)`, which also clears every
 *    other output and each driven pin's paired bit); it never writes `0x36` at
 *    all.  The K1's `BK4819_SetupPowerAmplifier` does, and our port had imported
 *    a `BK4819_TxOn_Beep` that wrote it to **0** -- which is why the carrier was
 *    real, on frequency and completely unamplified until `0x36` was set.
 *
 * The band-path pins `PA1`/`PA0` are the same in both directions here
 * (`FUN_08013A70(2)` for transmit and `(3)` for receive both leave `PA1 = 1,
 * PA0 = 0`); only the chip's PA enable and `0x36` differ.
 *
 * Values measured on the radio: `0x36 = 0x8822`, PWM compare 128 -> voice heard
 * on a second receiver.  See `docs/ra89r_rfpath.md`.
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

/* Bring up the PWM pin and timer and park the band-path pins; compare 0, so the
 * PA is unbiased until a transmission asks for power. */
void pa_init(void);

/* The bias PWM compare, 0 .. PA_PWM_MAX_DUTY.  Higher is more bias. */
void pa_power(uint16_t compare);

/* Park the band-path pins at the VHF setting (see pa_select_band). */
void pa_band_path(void);

/* Select the front end for a frequency (10 Hz units): the MCU band pins here,
 * the chip's own path bits through BK4819_PickRXFilterPathBasedOnFrequency().
 *
 * The RA89R has separate front-end paths for VHF and UHF and they were never
 * switched: the measured bring-up chain set no path at all, so both bands ran
 * with whatever the chip's GPIO register happened to hold.  The split (280 MHz)
 * is the K1's own and lands between this radio's codeplug bands, 108-174 and
 * 400-520 MHz.
 *
 * The chip side is the stock's too: register 0x33 bit 0x08 is the UHF path and
 * bit 0x04 the VHF one -- the stock's FUN_0800D35C drives 0x08 for its UHF band
 * and 0x08005692 drives 0x04.  The MCU side is PA0, which the stock sets from
 * its band state in FUN_08013A70 (its modes 2/3 leave PA0 low, 0/1 high).  PA0's
 * UHF level here is inferred from the validated VHF state: the radio tuned and
 * transmitted at 145.75 MHz with PA0 low, so UHF is taken as high.  If UHF turns
 * out deaf or the PA misbehaves, that one line is the thing to flip. */
bool pa_is_uhf(uint32_t freq_10hz);
void pa_select_band(uint32_t freq_10hz);

/* The front end currently selected (the state pa_select_band applied), for the
 * console. */
bool pa_band_is_uhf(void);

/* Which chip-side front-end selection is applied, and how to change it.  The
 * console 'f' command cycles these to find the setting this board actually
 * receives with; the register is read back for the log.  See pa.c for why the
 * default is "leave the register alone". */
uint8_t  pa_chip_path_mode(void);
void     pa_set_chip_path_mode(uint8_t mode);
uint16_t pa_chip_path_reg(void);

/* Which chip-side front-end selection is applied, and how to change it: the
 * console 'f' command cycles these to find the one this board actually receives
 * with (see pa.c).  The register's value is read back for the log. */
uint8_t  pa_chip_path_mode(void);
void     pa_set_chip_path_mode(uint8_t mode);
uint16_t pa_chip_path_reg(void);

/* Chip side, transmit: PA enable on (`0x33 = 0x0020`, the stock's clean
 * transmit select) and `0x36 = PA_REG36_ON`. */
void pa_tx_enable(void);

/* Chip side, receive: back to the state the K1 init leaves (`0x33 = 0x9000`),
 * `0x36 = 0`, compare 0. */
void pa_rx_enable(void);

/* What the driver last wrote, for a host test or the console. */
uint16_t pa_last_reg36(void);
uint16_t pa_last_reg33(void);
uint16_t pa_last_compare(void);

#endif /* DRIVER_PA_H */
