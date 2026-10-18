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

/* The 'w' sweep: extra stock TX register candidates, applied on top of the
 * measured chain, so the radio can settle which one the PA actually needs.  The
 * stock's own TX (`FUN_08016228`) writes `0x30 = 0xBFF1`, `0x47 = 0x6142`,
 * `0x13 = 0x03FF`, clears `0x31` bit 2 and raises `PC13`; the K1 chain sends
 * none of those.  See docs/ra89r_rfpath.md. */
uint8_t     tx_variant(void);
void        tx_variant_next(void);
void        tx_variant_apply(void);
const char *tx_variant_name(void);

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

/* ---- Duplex test mode (console 'U') -------------------------------------
 *
 * Satellite-style full duplex: the BK4815 receives on its VFO while the
 * BK4829 transmits on the other VFO, with the stock's per-chip register split
 * (docs/ra89r_fullduplex.md, "What the repeat entry does to the audio
 * registers").  During a duplex transmission nothing on the BK4815 is written:
 * no frequency, no band register, no T/R, no AF.  `rx_on_4815`/`tx_on_4829`
 * are the caller's resolved VFO assignments; enabling is refused unless the
 * receive VFO is bound to the BK4815 and the transmit VFO to the BK4829 (the
 * only transmit chain measured on this board). */
bool tx_set_duplex(bool on, bool rx_on_4815, bool tx_on_4829);
bool tx_duplex_active(void);

/* Live adjustments while duplex is on:
 *   AF mode 0 = the stock's TX-entry mute (0x47 = 0x6040; default)
 *           1 = leave the BK4829's AF at Normal (0x6142) -- this radio's
 *               measured path lets the BK4815's audio through the BK4829 node
 *           2 = the K1 mute the driver already applied (0x6042)
 *   path mode 0 = the full transmit band path (PA0/PA1 written)
 *             1 = leave PA0/PA1 as the receive path left them (TX may lose
 *                 power; tells whether the pins are what desenses the RX) */
void        tx_duplex_next_af(void);
void        tx_duplex_next_path(void);
const char *tx_duplex_af_name(void);
const char *tx_duplex_path_name(void);

/* Direct mode setters for the self-test ('E'), and the fourth variant: also
 * write the BK4815's T/R register (0x0C = 0x0203) at transmit entry -- the
 * stock's above-134 MHz transmit path does, and the receive lane may or may not
 * survive it. */
void tx_duplex_set_af(uint8_t mode);
void tx_duplex_set_path(uint8_t mode);
void tx_duplex_set_bk4815_tr(bool on);
bool tx_duplex_bk4815_tr(void);
const char *tx_duplex_tr_name(void);

/* Keep the BK4829's AF DAC (0x30 bit 9) alive during a duplex transmit: the
 * normal TX word (0xC1FE) turns it off, which silences the AF node the BK4815's
 * audio rides.  0xC3FE (TX + AF DAC) is the stock's own value. */
void tx_duplex_set_af_dac(bool on);
bool tx_duplex_af_dac(void);
uint16_t tx_duplex_reg30_value(void);
const char *tx_duplex_af_dac_name(void);

/* PC13 (the audio path line) during a duplex TX: 0 leave as RX had it, 1 drive
 * low (the stock's T/R), 2 drive high. */
void tx_duplex_set_pc13(uint8_t mode);
uint8_t tx_duplex_pc13(void);
const char *tx_duplex_pc13_name(void);

/* What the duplex transmit actually wrote: the BK4829's register reads are not
 * reliable while it transmits (they return values that do not match the writes
 * and vary between samples), so the console prints the written values instead. */
uint16_t tx_duplex_af_value(void);
uint32_t tx_last_frequency(void);

/* ---- Route probe (console 'o'): does the BK4829's microphone path carry the
 * BK4815's receive audio? ------------------------------------------------
 *
 * The one board-level fact satellite mode still depends on.  The BK4815's
 * `EARO` reaches the speaker either summed with the BK4829's `EARO` at the
 * amplifier input, or through the BK4829's microphone input; only the second
 * makes a BK4829 transmission relay the downlink (docs/ra89r_fullduplex.md,
 * "V52 static follow-up").
 *
 * The probe opens the BK4815's own AF output (its `0x49` gate is shut while the
 * receive squelch is closed, and the console blocks the poller that opens it),
 * mutes the BK4829's AF so the speaker cannot be picked up acoustically, powers
 * the BK4829's microphone path but never the PA, and exposes the chip's VOX
 * amplitude (`0x64`, the stock's `FUN_08017618` reader), which is the
 * microphone signal level.  The microphone path has two candidate power
 * settings -- the transmit entry's (`0x30 = 0xC1FE`) and the stock's VOX word
 * (`0xBDF1` with `0x31` bit 2, `FUN_0801754A`) -- so the console first proves
 * the reading with the operator's own voice and only then compares the level
 * with and without a downlink carrier. */
bool     tx_route_probe_begin(void);
void     tx_route_probe_mic_mode(uint8_t mode);   /* 0 = TX mic, 1 = stock VOX */
uint8_t  tx_route_probe_mic_mode_get(void);
uint16_t tx_route_probe_vox_amp(void);
void     tx_route_probe_end(void);
bool     tx_route_probe_active(void);

/* Swap mode ('y'): receive on the BK4829, transmit on the BK4815 (the stock's
 * repeat arrangement).  The 4829 is never written during the transmission, so
 * its receive audio path stays valid. */
void tx_set_swap(bool on);
bool tx_swap(void);

#endif /* DRIVER_TX_H */
