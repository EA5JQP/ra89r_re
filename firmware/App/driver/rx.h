/* The receive chain, as measured on this board.
 *
 * `rx_init()` brings the radio up to a receiving state in one call:
 *
 *   1. `BK4819_Init()` -- the K1 driver's own register sequences on this board's
 *      BK4829 (chip select PB8), which is what makes the part's id answer and
 *      what receive was validated with;
 *   2. the **second transceiver**: the stock configures it on every bring-up
 *      (`FUN_08016788` -> `FUN_08006A0C`, a 36-byte block into registers 2..19
 *      plus 33 writes) and parks it in `0x0C = 0x0A03`; leaving it at its
 *      power-on defaults is not a neutral state, it shares the RF path;
 *   3. the **PA**: `tx_init()` brings the PB14/TIM1_CH2 bias PWM up with a
 *      compare of 0, which is what the stock does from boot;
 *   4. the **audio path**: `audio_path_init()`, registered as the K1 driver's
 *      audio-path callback;
 *   5. the tune, `BK4819_SetAF(BK4819_AF_FM)` and `BK4819_RX_TurnOn()`.
 *
 * `rx_poll()` is the squelch: the caller paces it (the bench uses 50 ms), it
 * reads `0x67` and mutes or unmutes the chip's AF at the **stock's own marks**
 * -- open at `0xCF`, close below `0xB4` -- with `rx_squelch_open()` reporting
 * the state.  The mute is chip-side, as in the stock: nothing on this board is
 * switched for it.
 */
#ifndef DRIVER_RX_H
#define DRIVER_RX_H

#include <stdint.h>
#include <stdbool.h>

/* The stock's squelch marks on register 0x67 (see FUN_080052B8). */
#define RX_SQUELCH_OPEN_MARK   0xCFu
#define RX_SQUELCH_CLOSE_MARK  0xB4u

/* Squelch marks on the BK4815's register 0x44 (7-bit RSSI), used when the
 * selected VFO is on the BK4815.  Measured on the radio with console 'J' at
 * 145.500 with the receive RF-path pin 4 set (as the stock's BK4815 branch
 * does): idle ~20, a keyed carrier ~47, so the marks sit between. */
#define RX4815_SQUELCH_OPEN_MARK   0x1Eu   /* 30 */
#define RX4815_SQUELCH_CLOSE_MARK  0x18u   /* 24 */

/* Bring the RF up and start receiving on `freq_10hz` (10 Hz units, the same
 * convention as the codeplug).  Idempotent enough to be called again to retune. */
void rx_init(uint32_t freq_10hz);

/* Retune without touching anything else -- for a channel change. */
void rx_set_frequency(uint32_t freq_10hz);

/* True once rx_init() has run: without it the part is untuned and not in RX, so
 * 0x67 does not follow a carrier. */
bool rx_ready(void);

/* One squelch step.  Paced by the caller. */
void rx_poll(void);

bool rx_squelch_open(void);
uint16_t rx_rssi(void);

/* True when the receive audio is currently taken from the BK4815 (the selected
 * RX VFO's transceiver), for the console diagnostic. */
bool rx_audio_is_4815(void);

/* Diagnostic (console 'A'): force the BK4815's AF open, bypassing the squelch,
 * so the audio path can be judged separately from the squelch. */
void rx_force_bk4815_af(bool on);
bool rx_force_bk4815_af_on(void);

/* The frequency the BK4829 was last tuned to, in 10 Hz units, for diagnostics. */
uint32_t rx_rx_frequency(void);

/* The app loop's receive service: the squelch poll, the retune when the K1's
 * selected VFO moves, and the g_SquelchLost publication.  Does nothing while
 * the FM feature is up (rx_set_fm_active(true)). */
void rx_service(void);

/* Hand the receiver over to the FM broadcast feature (or take it back).  While
 * active, rx_service() is a no-op so the BK4829 cannot drive the shared
 * amplifier node out from under the BK1080. */
void rx_set_fm_active(bool active);

/* Keep both transceivers configured for their VFOs (rf_dual.c): resolve the
 * per-VFO transceiver setting, pick the primary/secondary roles and tune the
 * BK4815 secondary when one is active.  Cheap and safe to call from the receive
 * service; it only retunes when the role or the secondary frequency changes. */
void rf_dual_refresh(void);

/* Force the next rf_dual_refresh() to re-apply both transceivers' receive
 * frequencies.  Called after a transmit, which retunes the chips to the TX
 * frequency. */
void rf_dual_reapply(void);

/* A temporary scan-hit receive route.  While set, the receiver follows the scan
 * hit's chip and frequency instead of the selected VFO's transceiver; clearing
 * it restores the saved per-VFO route.  Used by the dual-transceiver scan while
 * a hit is paused.  Never persisted. */
typedef enum {
    RX_SCAN_SOURCE_DEFAULT = 0,   /* the selected VFO's own transceiver */
    RX_SCAN_SOURCE_BK4829  = 1,
    RX_SCAN_SOURCE_BK4815  = 2
} rx_scan_source_t;

void             rx_set_scan_source_override(rx_scan_source_t source);
void             rx_clear_scan_source_override(void);
rx_scan_source_t rx_scan_source_override(void);

#endif /* DRIVER_RX_H */
