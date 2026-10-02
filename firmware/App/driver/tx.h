/* The transmit chain, as measured on this board.
 *
 * The stock's own transmit path covers the chip's TX registers and the band
 * path; what it does not cover is the PA enable (`0x36`) and the unmute
 * (`0x50 = 0x3B20`), and both of those are where our imported driver was wrong.
 * This is the sequence that a second receiver confirms, in order:
 *
 *   pa_select_band()                PA0/PA1, the BK4815 0x75 band, the RX path bit
 *   pa_tx_enable(power)             0x33 = 0x42 VHF / 0x22 UHF (band pin + T/R);
 *                                   0x36 = (power << 8) | PA-CTL | gain (the K1's
 *                                   SetupPowerAmplifier); the PB14 PWM compare =
 *                                   power * ARR / 255 (the stock's arithmetic)
 *   BK4815 0x0C = 0x0203            the T/R path's other-branch state
 *   BK4819_SetFrequency()           0x38/0x39
 *   0x7D = 0xE958                   the stock's power/bias value for this codeplug
 *   BK4819_PrepareTransmit()        0x37 = 0x9D1F, 0x30 = 0xC1FE (mic ADC + TX DSP)
 *   0x47 = 0x6042                   AF muted; the AF DAC is not the modulation source
 *   0x50 = 0x3B20                   the TX unmute -- without it the carrier is silent
 *   0x40 = 0x3700 (microphone)      the gain field is a byte in bits 11:4
 *
 * `power` is the K1's `TXP_CalculatedSetting` (0..255, from the channel's
 * OUTPUT_POWER), so the menu's power ladder reaches this radio.  Its calibration
 * is provisional for now -- see SETTINGS_GetTxCalibration and
 * docs/ra89r_rfpath.md -- to be replaced by a measured sweep.
 *
 * See `docs/ra89r_rfpath.md` for the evidence and the register-by-register table.
 */
#ifndef DRIVER_TX_H
#define DRIVER_TX_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    TX_SOURCE_TONE = 0,     /* the chip's own DTMF tone: deterministic */
    TX_SOURCE_MIC           /* the microphone */
} tx_source_t;

/* Measured values.  The tone source ignores the microphone gain and vice versa. */
#define TX_REG7D_POWER    0xE958u   /* stock's power/bias for this codeplug */
#define TX_REG50_UNMUTE   0x3B20u   /* stock's TX unmute (our import sent 0x3B18) */
#define TX_MIC_GAIN       0x70u     /* audible on a second receiver */
#define TX_POWER_COMPARE  128u      /* bench PB14 compare default ('Y' steps it) */

/* Bring up the PA: the PB14 PWM and the band-path pins, no bias yet. */
void tx_init(void);

/* Tune, enable the PA, unmute and modulate.  `freq_10hz` is the channel in
 * 10 Hz units, the same convention as the codeplug; `power` is the K1's
 * `TXP_CalculatedSetting` (0..255). */
void tx_start(uint32_t freq_10hz, uint8_t power, tx_source_t source);

/* Back to receive: PA off, the second transceiver idle, RX on and muted. */
void tx_stop(void);

bool tx_active(void);
tx_source_t tx_source(void);

/* Read PTT from the keypad and drive the measured transmit chain; called by the
 * app loop (the K1's CheckKeys() leaves PTT to this port). */
void tx_poll_ptt(void);

#endif /* DRIVER_TX_H */
