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
 * does): idle ~20, a keyed carrier ~47, so the marks sit between.
 *
 * The part's RSSI scale is not the same on both bands: the same carrier that
 * reads 47-58 on 446.00625 reads only 32-33 on 145.5 (radio 2026-10-19), so
 * the UHF marks left the VHF squelch barely opening and dropping out on
 * fades.  Split them by the band of the tune, with the VHF pair sitting under
 * that band's carrier level and over its floor (measured 0-13). */
#define RX4815_SQUELCH_OPEN_MARK   0x1Eu   /* 30, UHF */
#define RX4815_SQUELCH_CLOSE_MARK  0x18u   /* 24, UHF */
#define RX4815_SQUELCH_OPEN_MARK_VHF   0x14u   /* 20, VHF */
#define RX4815_SQUELCH_CLOSE_MARK_VHF  0x0Eu   /* 14, VHF */

/* The BK4815's RSSI floor moves between sessions and bands (17 at 446 MHz on
 * 2026-10-21, 0-13 at 145.5 on 2026-10-19), so the level mark alone can sit
 * *below* the noise: at SQL 1 the VHF mark was 6 while the floor was 17, and
 * the lane was permanently open (and the dual scan stopped on noise every
 * batch).  The gate therefore keeps a slow floor estimate and never opens
 * inside `FLOOR_MARGIN` of it; the level mark still decides where the floor
 * allows it to matter. */
#define RX4815_FLOOR_MARGIN  4u

/* The marks scale with the user's squelch setting and with the lane's *own*
 * noise floor, because a fixed mark cannot work on this board: on 2026-10-21
 * the BK4829's floor measured 96-103 at 145.5 and 76-79 at 446, while the
 * BK4815's sat at 10-18, and the stock-like fixed pairs (0xCF/0xB4) then hid
 * every signal below full strength.  The stock itself compares against a
 * threshold just above its floor (its squelch page values are 23-37, on a
 * lower-noise AGC setting), so a floor + margin rule is the faithful shape.
 *
 * `level` is gEeprom.SQUELCH_LEVEL (0 = off, handled by the caller):
 * margin = base + step * level, hysteresis below the opening mark. */
#define RX_SQUELCH_MARGIN_BASE_4829   6u
#define RX_SQUELCH_MARGIN_STEP_4829   6u
#define RX_SQUELCH_MARGIN_BASE_4815   2u
#define RX_SQUELCH_MARGIN_STEP_4815   2u
#define RX_SQUELCH_HYST_4829          12u
#define RX_SQUELCH_HYST_4815          4u

/* The margin above the learned floor, monotone in the squelch level. */
static inline uint16_t rx_squelch_margin(bool use_4815, uint8_t level)
{
    const uint8_t lv = (level > 9u) ? 9u : level;

    return (uint16_t)(use_4815
                          ? (RX_SQUELCH_MARGIN_BASE_4815 + RX_SQUELCH_MARGIN_STEP_4815 * lv)
                          : (RX_SQUELCH_MARGIN_BASE_4829 + RX_SQUELCH_MARGIN_STEP_4829 * lv));
}

/* The opening mark for a lane whose floor is `floor`.  The old fixed pairs are
 * used as a *lower* bound only when the floor is not known yet (floor 0).
 */
static inline uint16_t rx_squelch_open_mark_from_floor(bool use_4815, bool uhf,
                                                       uint8_t level, uint16_t floor)
{
    if (floor == 0u)
        return use_4815 ? (uhf ? RX4815_SQUELCH_OPEN_MARK : RX4815_SQUELCH_OPEN_MARK_VHF)
                        : RX_SQUELCH_OPEN_MARK;

    return (uint16_t)(floor + rx_squelch_margin(use_4815, level));
}

static inline uint16_t rx_squelch_close_mark_from_floor(bool use_4815, bool uhf,
                                                        uint8_t level, uint16_t floor)
{
    const uint16_t open = rx_squelch_open_mark_from_floor(use_4815, uhf, level, floor);
    const uint16_t hyst = use_4815 ? RX_SQUELCH_HYST_4815 : RX_SQUELCH_HYST_4829;

    return (uint16_t)((open > hyst) ? (uint16_t)(open - hyst) : 0u);
}

/* Backwards-compatible wrappers for callers that only know the level (the
 * scanner's 4815 lane, tests): the fixed level-8 anchor plus the level steps.
 */
#define RX_SQUELCH_STEP_4829   6u    /* per level on the 9-bit 0x67 scale */
#define RX_SQUELCH_STEP_4815   2u    /* per level on the 7-bit 0x44 scale */
#define RX_SQUELCH_HYST_4829_FIXED   27u   /* the stock pair's hysteresis */
#define RX_SQUELCH_HYST_4815_FIXED   6u

/* Signed level offset from the level-8 anchor: -7 at level 1, 0 at 8, +1 at 9. */
static inline int rx_squelch_level_offset(uint8_t level)
{
    const uint8_t lv = (level > 9u) ? 9u : level;

    return (int)lv - 8;
}

/* The opening / closing mark for a lane.  `use_4815` picks the chip's scale,
 * `uhf` its band split (the BK4815's RSSI reads lower on VHF), and `level` is
 * gEeprom.SQUELCH_LEVEL.  A return of 0 for the open mark means "always open"
 * and is the caller's level-0 case. */
static inline uint16_t rx_squelch_open_mark(bool use_4815, bool uhf, uint8_t level)
{
    const int off = rx_squelch_level_offset(level);

    if (use_4815) {
        const int base = (int)(uhf ? RX4815_SQUELCH_OPEN_MARK : RX4815_SQUELCH_OPEN_MARK_VHF);
        const int v    = base + off * (int)RX_SQUELCH_STEP_4815;

        return (uint16_t)((v > 1) ? v : 1);
    }

    return (uint16_t)((int)RX_SQUELCH_OPEN_MARK + off * (int)RX_SQUELCH_STEP_4829);
}

static inline uint16_t rx_squelch_close_mark(bool use_4815, bool uhf, uint8_t level)
{
    const uint16_t open = rx_squelch_open_mark(use_4815, uhf, level);
    const uint16_t hyst = use_4815 ? RX_SQUELCH_HYST_4815_FIXED
                                  : RX_SQUELCH_HYST_4829_FIXED;

    return (uint16_t)((open > hyst) ? (uint16_t)(open - hyst) : 0u);
}

/* ---- the noise-floor tracker (pure, host-tested) --------------------------
 *
 * The gate's threshold is this floor plus the level margin, so the floor has to
 * be *noise* and nothing else.  Its rules, each one learned the hard way on the
 * radio (2026-10-21/22):
 *
 *  - seed from the minimum of RX_FLOOR_SEED_SAMPLES samples, because the first
 *    sample after a retune can be a transient (a 193 seed sat above every
 *    signal and the gate never opened again);
 *  - a single sample far *below* the floor is a transient too (a mid-retune
 *    read of 1 held the gate open on noise), but a genuinely quieter channel
 *    must be followed: a small fall is immediate, a large one after
 *    RX_FLOOR_DROP_SAMPLES consecutive samples;
 *  - readings more than RX_FLOOR_RISE_BAND above the floor are signals: the
 *    floor never learns them (a -20 dBm carrier walked the floor from 49 to
 *    268), and while the squelch is open it is frozen anyway;
 *  - per-lane bounds from the measured baselines, applied at seed and after
 *    every step.
 */
#define RX_FLOOR_MIN_4829     40u   /* measured 54-61 UHF, 96-103 VHF */
#define RX_FLOOR_MIN_4815      8u   /* measured 11-18 */
#define RX_FLOOR_MAX_4829    200u
#define RX_FLOOR_MAX_4815     60u
#define RX_FLOOR_MAX_DROP      8u   /* an immediate one-sample fall */
#define RX_FLOOR_DROP_SAMPLES  4u   /* consecutive samples for a larger fall */
#define RX_FLOOR_RISE_BAND     6u   /* above this it is a signal */
#define RX_FLOOR_SEED_SAMPLES  3u
/* Seed ceilings, so a carrier present when a (lane, band) is first learned
 * cannot push the mark above itself -- radio 2026-10-22: switching to a VFO
 * with a -20 dBm carrier seeded the BK4829's floor at 199 and the mark at 229,
 * so the gate stayed closed on a 204 reading. */
#define RX_FLOOR_SEED_MAX_4829_UHF  80u   /* measured noise 49-61 */
#define RX_FLOOR_SEED_MAX_4829_VHF 120u   /* measured noise 96-103 */
#define RX_FLOOR_SEED_MAX_4815_UHF  30u   /* measured noise 10-18 */
#define RX_FLOOR_SEED_MAX_4815_VHF  30u

typedef struct {
    uint16_t floor;
    uint16_t seed_min;
    uint8_t  seed_n;
    uint8_t  low_cnt;
    bool     valid;
} rx_floor_t;

static inline void rx_floor_reset(rx_floor_t *f)
{
    f->floor    = 0u;
    f->seed_min = 0xFFFFu;
    f->seed_n   = 0u;
    f->low_cnt  = 0u;
    f->valid    = false;
}

static inline uint16_t rx_floor_clamp(uint16_t v, bool use_4815)
{
    const uint16_t min = use_4815 ? RX_FLOOR_MIN_4815 : RX_FLOOR_MIN_4829;
    const uint16_t max = use_4815 ? RX_FLOOR_MAX_4815 : RX_FLOOR_MAX_4829;

    if (v < min)
        return min;
    if (v > max)
        return max;
    return v;
}

/* One poll: `rssi` is the reading, `closed` the gate state before this poll.
 * Returns the floor in force (0 while still seeding). */
static inline uint16_t rx_floor_step(rx_floor_t *f, bool use_4815, uint16_t seed_max,
                                     uint16_t rssi, bool closed)
{
    if (!f->valid) {
        if (f->seed_min == 0xFFFFu || rssi < f->seed_min)
            f->seed_min = rssi;
        if (++f->seed_n >= RX_FLOOR_SEED_SAMPLES) {
            const uint16_t seed = (f->seed_min > seed_max) ? seed_max : f->seed_min;

            f->floor = rx_floor_clamp(seed, use_4815);
            f->valid = true;
        }
        return f->valid ? f->floor : 0u;
    }

    if (!closed)
        return f->floor;

    if (rssi < f->floor) {
        if ((uint16_t)(f->floor - rssi) <= RX_FLOOR_MAX_DROP) {
            f->floor   = rssi;
            f->low_cnt = 0u;
        } else if (++f->low_cnt >= RX_FLOOR_DROP_SAMPLES) {
            f->floor   = rssi;
            f->low_cnt = 0u;
        }
    } else {
        f->low_cnt = 0u;
        if (rssi <= (uint16_t)(f->floor + RX_FLOOR_RISE_BAND))
            f->floor = (uint16_t)(f->floor + ((rssi - f->floor) >> 4));
    }

    f->floor = rx_floor_clamp(f->floor, use_4815);
    return f->floor;
}

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

/* The gate has been closed for at least `polls` consecutive polls.  The LISTEN
 * sweep waits for this (docs/ra89r_scan.md): a single corrupted 0x67 read used
 * to be enough to start a probe batch, which then collided with the poller and
 * turned the audio into noise. */
bool rx_squelch_quiet(uint8_t polls);
uint16_t rx_rssi(void);

/* The marks actually in force right now: the active lane's learned floor plus
 * the level-derived margin (rx_squelch_open_mark_from_floor above).  The
 * scanner uses the opening mark so the sweep and the audio agree. */
uint16_t rx_squelch_open_mark_now(void);
uint16_t rx_squelch_close_mark_now(void);

/* The active lane's floor estimate (0 while it is still unknown). */
uint16_t rx_squelch_floor(void);

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
