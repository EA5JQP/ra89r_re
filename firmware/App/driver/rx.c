#include "driver/rx.h"

#include "app/chFrScanner.h"
#include "driver/audio_path.h"
#include "driver/bk4815.h"
#include "driver/bk4819.h"
#include "driver/bk4829.h"
#include "driver/pa.h"
#include "driver/rf_dual.h"
#include "driver/systick.h"
#include "driver/tx.h"
#include "driver/uart.h"
#include "driver/led.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"

static bool s_ready;
static bool s_squelch_open;
static int8_t s_tail_close_delay = -1;  /* console 'O': 0..3, -1 = the app's 3 */
static rx_squelch_t s_sq;       /* the stock's 0..10 counter; see rx.h */
/* Consecutive polls with the gate closed.  The LISTEN sweep waits for a few
 * before probing: one corrupted reading used to be enough to start it. */
static uint8_t s_closed_polls;
static bool s_fm_active;        /* the FM feature owns the audio; see rx_set_fm_active() */
static uint16_t s_rssi;
static uint32_t s_freq_10hz;    /* what the BK4829 was last tuned to */

/* Retune settling.  The first RSSI reads after a retune are the AGC/PLL
 * settling, not the new channel's level; without a hold-off they cross the
 * open mark and the squelch burps (a noise burst) on every frequency change --
 * the 2026-10-23 bench finding.  Five polls at the 10 ms service tick is 50 ms:
 * longer than the transient, short enough not to delay a real signal. */
#define RX_SETTLE_POLLS 5u
static uint8_t s_settle_polls;

/* Defined with the floor tracker below; needed by the retune paths above it. */
static void rx_floor_invalidate(void);
static void rx_gate_hold_closed(void);

void rx_init(uint32_t freq_10hz)
{
    /* The order is the one the bench was validated in, kept deliberately: the
     * K1 bring-up and receive first, then the second transceiver, the PA's PWM
     * and the audio path. */
    BK4819_SetAudioPathCallback(audio_path_drive);
    BK4819_Init();
    BK4819_SetFrequency(freq_10hz);
    s_freq_10hz = freq_10hz;
    BK4819_SetAF(BK4819_AF_FM);
    BK4819_RX_TurnOn();
    /* The stock's per-channel config writes `0x50 = 0x3B20` (FUN_08016DE8), and
     * the K1's `ExitTxMute` is the same word: it is the audio path's on value,
     * not a transmit-only unmute.  The port never wrote it for receive, so the
     * AF path kept the chip's reset word -- and `tx_stop()` then wrote the muted
     * `0xBB18`, which is why receive audio never came back after a PTT. */
    BK4819_WriteRegister(BK4819_REG_50, TX_REG50_UNMUTE);

    /* The BK4815 configuration comes first: it writes the stock's table,
     * including the band register 0x75, so the per-band value pa_select_band()
     * sets must land after it. */
    bk4815_configure();
    pa_select_band(freq_10hz);
    /* The BK4815's T/R state: the stock parks it (0xFFFB) at or below 134 MHz and
     * leaves it idle (0x0A03) above it (FUN_08016EE0 / FUN_08009CC4). */
    bk4815_write_reg(0x0C, pa_band_is_main() ? 0x0A03u : 0xFFFBu);
    tx_init();
    audio_path_init();

    BK4819_SetAF(BK4819_AF_MUTE);       /* start quiet; rx_poll() opens it */

    rx_floor_invalidate();      /* fresh estimates on every boot */
    s_ready = true;
    s_squelch_open = false;
    rx_squelch_reset(&s_sq);
}

void rx_set_frequency(uint32_t freq_10hz)
{
    if (freq_10hz != s_freq_10hz)
        rx_gate_hold_closed();

    BK4819_SetFrequency(freq_10hz);
    s_freq_10hz = freq_10hz;
    pa_select_band(freq_10hz);
}

bool rx_ready(void)
{
    return s_ready;
}

/* The FM broadcast feature (app/fm.c) runs the BK1080 on this board's shared
 * amplifier node.  While it is up, the BK4829 must not be polled or re-tuned:
 * `rx_poll()` would open the chip's AF on a carrier and drive that node with
 * the VFO's demodulated audio, and `rx_service()` would re-tune the RF chip to
 * the VFO, both of which the stock's own FM-on avoids (it idles the
 * transceivers, `FUN_08009CC4`).  `FM_Start()` sets this; `FM_TurnOff()`
 * clears it. */
void rx_set_fm_active(bool active)
{
    s_fm_active = active;
}

/* Which chip supplies the receive audio: the selected RX VFO's transceiver.
 * The stock switches the AF source by the same flag (`FUN_08015F48`: the
 * BK4829's 0x47 or the BK4815's 0x49 = 0x9A02), so only the selected VFO is
 * heard.  `s_audio_4815` is that choice. */
static bool s_audio_4815;
static uint32_t s_4815_freq;    /* the tune last applied to the BK4815 (for its squelch marks) */
static bool s_force_4815_af;    /* console 'A' diagnostic */

/* Noise-floor tracker per (lane, band, modulation): the BK4829's floor is
 * 49-57 at UHF, 96-103 at VHF in FM and 63 at VHF in AM (the AM AGC profile
 * changes the scale), the BK4815's 10-18, so one number would be wrong
 * somewhere.  Keeping one per combination also means a tune or a VFO switch
 * does NOT throw the learned value away.  Index [lane][uhf | am << 1]. */
static rx_floor_t s_floor[2][4];

static uint16_t rx_floor_prior(uint8_t lane, bool uhf, bool am)
{
    if (lane == 1u)
        return RX_FLOOR_PRIOR_4815;
    if (uhf)
        return am ? RX_FLOOR_PRIOR_4829_AM_UHF : RX_FLOOR_PRIOR_4829_FM_UHF;
    return am ? RX_FLOOR_PRIOR_4829_AM_VHF : RX_FLOOR_PRIOR_4829_FM_VHF;
}

static bool rx_lane_am(void)
{
    return gRxVfo->Modulation == MODULATION_AM;
}

static uint16_t rx_floor_update(uint8_t lane, bool uhf, uint16_t rssi, bool closed)
{
    const bool am = rx_lane_am();
    const uint8_t slot = (uint8_t)((uhf ? 1u : 0u) | (am ? 2u : 0u));

    if (lane > 1u)
        lane = 0u;

    return rx_floor_step(&s_floor[lane][slot], lane == 1u,
                         rx_floor_prior(lane, uhf, am), rssi, closed);
}

/* True when the active lane is tuned above the 280 MHz band split. */
static bool rx_lane_uhf(void)
{
    const uint32_t f = s_audio_4815 ? s_4815_freq : s_freq_10hz;

    return (f != 0u) && (f >= PA_BAND_SPLIT);
}

static uint16_t rx_active_floor(void)
{
    const uint8_t lane = s_audio_4815 ? 1u : 0u;

    const bool    uhf  = rx_lane_uhf();
    const uint8_t slot = (uint8_t)((uhf ? 1u : 0u) | (rx_lane_am() ? 2u : 0u));

    return s_floor[lane][slot].valid ? s_floor[lane][slot].floor : 0u;
}

uint16_t rx_squelch_floor(void)
{
    return rx_active_floor();
}

uint16_t rx_squelch_open_mark_now(void)
{
    return rx_squelch_open_mark_from_floor(s_audio_4815, rx_lane_uhf(),
                                           rx_lane_am(),
                                           gEeprom.SQUELCH_LEVEL, rx_active_floor());
}

uint16_t rx_squelch_close_mark_now(void)
{
    return rx_squelch_close_mark_from_floor(s_audio_4815, rx_lane_uhf(),
                                            rx_lane_am(),
                                            gEeprom.SQUELCH_LEVEL, rx_active_floor());
}

static void rx_floor_invalidate(void)
{
    unsigned lane, band;

    for (lane = 0; lane < 2u; lane++)
        for (band = 0; band < 4u; band++)
            rx_floor_reset(&s_floor[lane][band]);
}

/* A retune is not a signal: close the gate, silence all three audio gates at
 * once, and ignore the RSSI for the settling window.  Called from every path
 * that moves the receiver (rx_set_frequency, the BK4815 tune, a lane switch). */
static void rx_gate_hold_closed(void)
{
    s_squelch_open  = false;
    s_closed_polls  = 0;
    s_settle_polls  = RX_SETTLE_POLLS;
    /* The hold-off is a fresh start: if the counter were left open, the first
     * settled sample would re-open the gate through its still-high count and
     * the retune burp would be back. */
    rx_squelch_reset(&s_sq);

    rx_apply_audio_gate();
}

/* A temporary scan-hit receive route (see rx.h).  DEFAULT follows the selected
 * VFO's own transceiver; the other values pin the hit's chip until cleared. */
static rx_scan_source_t s_scan_source = RX_SCAN_SOURCE_DEFAULT;

/* The override actually applied to the chips, so rx_service() retunes only on a
 * change instead of on every loop. */
static rx_scan_source_t s_scan_source_applied = RX_SCAN_SOURCE_DEFAULT;
static uint32_t         s_scan_source_freq;

void rx_set_scan_source_override(rx_scan_source_t source)
{
    s_scan_source = source;

    /* The scan lanes tune the chips themselves (chFrScanner.c's ScanBothProbe:
     * pa_select_band() + bk4815_set_frequency(probe)).  rf_dual_refresh() only
     * retunes on a change it knows about, so without this the part is left on
     * the last probe when the scan ends: the VFO reads its own frequency, the
     * chip is elsewhere, the squelch never opens on the real channel and a
     * manual retune was the only way back (radio 2026-10-18: VFO A on 145.500
     * but the BK4815 tuned to 146.25000, a scan probe).  Invalidate the
     * applied state on entry and exit so returning to the VFOs re-applies
     * both lanes. */
    rf_dual_reapply();
}

void rx_clear_scan_source_override(void)
{
    s_scan_source = RX_SCAN_SOURCE_DEFAULT;
    /* A scan writes the chips under the poller, so after one stops the floor
     * learned before it may not describe this channel: start over. */
    rf_dual_reapply();      /* see rx_set_scan_source_override() */
}

rx_scan_source_t rx_scan_source_override(void)
{
    return s_scan_source;
}

void rx_force_bk4815_af(bool on)
{
    s_force_4815_af = on;
}

bool rx_force_bk4815_af_on(void)
{
    return s_force_4815_af;
}

static void rx_set_audio_source(bool use_4815)
{
    /* The receive-path pin belongs to the lane, not to a VFO assignment: the
     * stock sets it for its BK4815 entry and clears it for the BK4829's, and a
     * wrongly set pin costs the BK4829 ~16 dB at VHF (measured).  Re-assert it
     * on every pass, because the app's own radio setup and the scan probes write
     * 0x33 too. */
    pa_set_rx_lane(use_4815);

    if (use_4815 == s_audio_4815)
        return;

    s_audio_4815  = use_4815;
    s_closed_polls = 0u;
    s_settle_polls = RX_SETTLE_POLLS;   /* a lane switch retunes the audio path */
    /* Mute both and let the squelch open the selected one.  The BK4815's audio
     * shares the BK4829's AF node (measured: it is only audible while the
     * BK4829's AF is open, e.g. with the K1 squelch at 0), so the BK4815 source
     * must open the BK4829's AF as well as its own 0x49. */
    BK4819_SetAF(BK4819_AF_MUTE);
    bk4815_set_af(false);
    s_squelch_open = false;
    rx_squelch_reset(&s_sq);
}

/* Drive the three audio gates from the current squelch state.  Split out of
 * rx_poll() because the K1's own receive paths open the amplifier and unmute
 * the chip's AF behind the gate's back: `RADIO_SetupRegisters()` ends with
 * `RADIO_SetModulation()` (AF = FM) and `APP_StartListening()` does
 * `AUDIO_AudioPathOn()` + `gEnableSpeaker = true`.  Re-applying the gate right
 * there closes the retune/entry transient instead of leaving it to the next
 * 10 ms poll -- the audible squelch burp on every frequency change
 * (2026-10-23). */
void rx_apply_audio_gate(void)
{
    if (!s_ready)
        return;

    if (s_audio_4815) {
        bk4815_set_af(s_squelch_open);

        /* While a scan runs on the other chip, keep the BK4829's AF output
         * muted with the AF DAC alive (the stock TX-entry mute word). */
        if (gScanStateDir != SCAN_OFF)
            BK4819_WriteRegister(BK4819_REG_47, 0x6040u);
        else
            BK4819_SetAF(s_squelch_open ? BK4819_AF_FM : BK4819_AF_MUTE);
    } else {
        bk4815_set_af(false);
        BK4819_SetAF(s_squelch_open ? BK4819_AF_FM : BK4819_AF_MUTE);
    }

    audio_path_drive(s_squelch_open);
}

void rx_poll(void)
{
    if (!s_ready)
        return;

    /* A retune's first readings are the receiver settling (the same reason the
     * K1's squelch tick waits): hold the gate closed and the audio muted for
     * RX_SETTLE_POLLS instead of letting a transient open the squelch. */
    if (s_settle_polls != 0u) {
        s_settle_polls--;
        s_squelch_open = false;
        rx_apply_audio_gate();
        return;
    }

    if (s_audio_4815) {
        /* Squelch 0 is the K1's "always open": RADIO_ConfigureSquelchAndOutputPower()
         * forces the BK4829's chip-squelch thresholds to 0 for it.  This
         * software gate must honour it too, or a BK4815 VFO stays muted below
         * the fixed mark even with the user's squelch turned off. */
        const bool     squelch_off = (gEeprom.SQUELCH_LEVEL == 0u);
        const bool     was_open    = s_squelch_open;

        s_rssi = bk4815_read_rssi();

        /* Learn the lane's floor and take the marks from it: a fixed mark cannot
         * work on this board (see rx.h).  Not while a scan runs: it retunes
         * this very chip under the poller, and the samples it reads there
         * dragged the floor to its minimum and left the gate unable to open on
         * a real signal (radio 2026-10-21).
         *
         * While the gate is OPEN the floor is frozen *except* for a weak level
         * (within two margins of it): that is noise that happened to trip the
         * gate, and freezing on it would hold the squelch open forever.  A real
         * signal is further above and keeps the floor where it is. */
        {
            const uint16_t open_now  = rx_squelch_open_mark_now();
            const bool     weak_open = s_squelch_open &&
                s_rssi < (uint16_t)(open_now + rx_squelch_margin(true, rx_lane_am(), gEeprom.SQUELCH_LEVEL));

            if (gScanStateDir == SCAN_OFF && (!s_squelch_open || weak_open))
                (void)rx_floor_update(1u, rx_lane_uhf(), s_rssi, true);
        }

        {
            const uint16_t open_mark  = rx_squelch_open_mark_now();
            const uint16_t close_mark = rx_squelch_close_mark_now();
            const bool     present    = (s_rssi >= open_mark);
            const bool     absent     = (s_rssi < close_mark);
            const bool     force      = s_force_4815_af || squelch_off;

            rx_squelch_step(&s_sq, present, absent, force);
            s_squelch_open = s_sq.open;
        }

        /* The K1's power save sleeps the BK4829 when idle (BK4819_Sleep():
         * 0x30 = 0, 0x37 = 0x1D00) and its own receive path wakes it through
         * BK4819_Conditional_RX_TurnOn().  The BK4815's audio rides that
         * chip's RX/AF chain, so the first reception after an idle period
         * opens the gates onto a sleeping chip: the shared path is dead and
         * the reception is noise until the app's wake runs at its end (radio
         * 2026-10-19: first PTT noise, second one voice).  Wake it here, on
         * the opening edge, so the first reception is like the rest. */
        if (s_squelch_open && !was_open)
            BK4819_RX_TurnOn();

        if (s_squelch_open)
            s_closed_polls = 0u;
        else if (s_closed_polls < 255u)
            s_closed_polls++;

        /* Drive the three gates every poll, not only on a transition.  The K1's
         * own receive setup (RADIO_SetupRegisters -> RADIO_SetModulation, and
         * AUDIO_AudioPathOff) writes the shared AF node and the amplifier behind
         * our back, so a one-shot open at the transition leaves the audio muted
         * from then on.  The BK4815's own AF (0x49), the BK4829's AF (0x47, the
         * node the BK4815's audio rides) and the amplifier (PC13) must all be
         * open together for a BK4815 receive to be heard. */
        rx_apply_audio_gate();
        return;
    }

    /* A reading below the lane's plausible floor is a bus artefact, not a
     * signal: the scanner drives the same bit-banged bus, so a poll that
     * collides with a probe transaction returns 0 or garbage.  Classifying that
     * as "squelch closed" let the LISTEN sweep start, which collided further and
     * turned the audio into noise (radio 2026-10-22).  Hold the previous value
     * and the gate state instead. */
    {
        const uint16_t raw = BK4819_GetRSSI();

        if (raw != 0u)
            s_rssi = raw;
    }

    {
        const bool     squelch_off = (gEeprom.SQUELCH_LEVEL == 0u);

        {
            const uint16_t open_now  = rx_squelch_open_mark_now();
            const bool     weak_open = s_squelch_open &&
                s_rssi < (uint16_t)(open_now + rx_squelch_margin(false, rx_lane_am(), gEeprom.SQUELCH_LEVEL));

            /* See the BK4815 branch: a weak open level (within two margins of
             * the floor) is noise and is learned, a real signal is not. */
            if (gScanStateDir == SCAN_OFF && (!s_squelch_open || weak_open))
                (void)rx_floor_update(0u, rx_lane_uhf(), s_rssi, true);
        }

        {
            /* The BK4829 lane is decided by the chip's own squelch comparator
             * (0x4F/0x78/0x4D/0x4E, programmed by
             * RADIO_ConfigureSquelchAndOutputPower()).  REG_0C<1> is its
             * result: 1 = link/open, 0 = loss/closed.  The chip does not gate
             * the AF on this board (measured 2026-10-24: the AF hisses through
             * sqlFound with 0x3B20 and 0x3B18), so the MCU still drives the
             * audio gate -- the K1 decision, the stock's mute.
             *
             * The RSSI floor/marks above are kept for the scanner and the
             * S-meter, but they no longer decide this gate. */
            if (squelch_off)
                s_squelch_open = true;
            else
                s_squelch_open = (BK4819_ReadRegister(BK4819_REG_0C) & 0x0002u) != 0u;

            if (s_squelch_open)
                s_closed_polls = 0u;
            else if (s_closed_polls < 255u)
                s_closed_polls++;

            /* Drive the AF and the amplifier every poll, not only on a
             * transition.  The app's own receive setup runs on every scan start,
             * VFO reconfiguration and scan hit and begins with
             * AUDIO_AudioPathOff() (the K1's AUDIO_AudioPathOff ->
             * GPIO_DisableAudioPath -> PC13), so a transition-only drive left
             * the audio shut whenever the gate was already open: radio
             * 2026-10-22, LISTEN with a -20 dBm carrier, squelch OPEN, RSSI 204,
             * and no tone at all.  The BK4815 branch already drove its gates
             * every poll for the same reason. */
            rx_apply_audio_gate();
        }
    }
}

bool rx_squelch_open(void)
{
    return s_squelch_open;
}

bool rx_squelch_quiet(uint8_t polls)
{
    return !s_squelch_open && s_closed_polls >= polls;
}

bool rx_audio_is_4815(void)
{
    return s_audio_4815;
}

/* Put the bench VFO back to the normal UHF receive after the forced 145.5
 * setup.  The VFO unit is 10 Hz. */
static void rx_tail_restore_vfo(void)
{
    gRxVfo->freq_config_RX.Frequency = 44600625u;
    gRxVfo->freq_config_TX.Frequency = 44600625u;
    gRxVfo->Modulation              = MODULATION_FM;
    RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
    RADIO_SetupRegisters(false);
    rx_set_frequency(44600625u);
    uart_puts("tail: VFO restored to 446.00625 MHz\n");
}

/* Console '@': bounded electrical tail trace for the BK4829 hybrid.
 *
 * The app loop is blocked, so the trace owns the audio: AF FM, amplifier on,
 * the app's thresholds.  It reads 0x67 and REG_0C<1> every ~2 ms and records
 * the RSSI fall, the chip's close and the audio-gate call.  The deltas are the
 * electrical tail components; the speaker decay is not visible here (the ear
 * is the acoustic acceptance).  No scope, no case opening, no marker pin.
 *
 * t_close - t_fall: chip comparator + AGC/quieting + the 2 ms sample loop.
 * t_gate - t_close: the MCU mute (0x47 + PC13), a couple of bus writes.
 */
void rx_tail_set_close_delay(int delay)
{
    if (delay < 0 || delay > 3)
        delay = 3;
    s_tail_close_delay = (int8_t)delay;
}

void rx_tail_trace(void)
{
    enum {
        TAIL_SAMPLE_US   = 2000u,
        TAIL_DURATION_MS = 30000u
    };
    uint32_t t0, deadline, next, close_at = 0u, last_diag = 0u;
    uint32_t t_fall = 0u, t_close = 0u, t_gate = 0u;
    uint16_t open_mark;
    uint16_t rssi;
    uint16_t reg0c;
    bool was_open;
    bool first_fall  = false;
    bool first_close = false;
    unsigned transitions = 0u;

    if (!s_ready) {
        uart_puts("\ntail: receiver not ready\n");
        return;
    }
    if (s_audio_4815) {
        uart_puts("\ntail: selected lane is BK4815; this trace measures the BK4829 hybrid\n");
        return;
    }

    /* Pin the bench frequency and its matching threshold table.  The app's VFO
     * state has wandered across the CAT tests; an adjacent-channel carrier
     * would silently invalidate the trace. 145.500 is the low-output tinySA
     * setting used for the tail.  The codeplug's unit is 10 Hz. */
    gRxVfo->freq_config_RX.Frequency = 14550000u;
    gRxVfo->freq_config_TX.Frequency = 14550000u;
    gRxVfo->Modulation              = MODULATION_FM;
    RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
    RADIO_SetupRegisters(false);
    if (s_tail_close_delay >= 0) {
        uint16_t r4e = BK4819_ReadRegister(BK4819_REG_4E);

        r4e = (uint16_t)((r4e & ~(3u << 9)) |
                         ((uint16_t)s_tail_close_delay << 9));
        BK4819_WriteRegister(BK4819_REG_4E, r4e);
    }
    rx_set_frequency(14550000u);
    systick_delay_ms(60);

    bk4815_set_af(false);
    BK4819_SetAF(BK4819_AF_FM);
    audio_path_drive(1);

    /* Use the chip's own RSSI-open threshold, not the app's floor: the floor
     * tracker can be poisoned by a carrier that was present while the gate was
     * closed.  0x78<15:8> is the open threshold, the same 0.5 dB/step scale as
     * 0x67. */
    open_mark = (uint16_t)((BK4819_ReadRegister(BK4819_REG_78) >> 8) & 0xFFu);
    if (open_mark == 0u)
        open_mark = rx_squelch_open_mark_now();

    uart_printf("tail: thresholds 4D=%04X 4E=%04X 4F=%04X 78=%04X "
                "63=%u 65=%u 67=%u\n",
                BK4819_ReadRegister(BK4819_REG_4D),
                BK4819_ReadRegister(BK4819_REG_4E),
                BK4819_ReadRegister(BK4819_REG_4F),
                BK4819_ReadRegister(BK4819_REG_78),
                BK4819_GetGlitchIndicator(),
                BK4819_GetExNoiceIndicator(),
                BK4819_GetRSSI());

    /* The squelch open delay (0x4E<13:11> = 5) can take longer than the 60 ms
     * settle; wait up to 500 ms for the chip to report link. */
    {
        unsigned i;

        for (i = 0u; i < 50u; i++) {
            rssi  = BK4819_GetRSSI();
            reg0c = BK4819_ReadRegister(BK4819_REG_0C);
            if ((reg0c & 0x0002u) != 0u && rssi >= open_mark)
                break;
            systick_delay_ms(10u);
        }
    }

    was_open = (reg0c & 0x0002u) != 0u;

    if (!was_open || rssi < open_mark) {
        uart_printf("\ntail: no carrier at start (rssi %u, mark %u, reg0c 0x%04X); "
                    "key the tone and retry\n",
                    rssi, open_mark, reg0c);
        rx_tail_restore_vfo();
        return;
    }

    uart_printf("\ntail trace: 145.500 MHz, open mark %u, start rssi %u, "
                "reg0c 0x%04X; turn the tinySA output off now (up to %u ms)\n",
                open_mark, rssi, reg0c, (unsigned)TAIL_DURATION_MS);

    t0       = systick_micros();
    deadline = t0 + TAIL_DURATION_MS * 1000u;
    next     = t0;

    while ((int32_t)(systick_micros() - deadline) < 0) {
        rssi  = BK4819_GetRSSI();
        reg0c = BK4819_ReadRegister(BK4819_REG_0C);

        {
            const uint32_t now  = systick_micros();
            const bool     open = (reg0c & 0x0002u) != 0u;

            if (!first_fall && rssi < open_mark) {
                first_fall = true;
                t_fall     = now - t0;
                last_diag  = now;
                uart_printf("tail: fall +%u ms rssi %u noise %u glitch %u "
                            "reg0c 0x%04X\n",
                            (unsigned)(t_fall / 1000u), rssi,
                            BK4819_GetExNoiceIndicator(),
                            BK4819_GetGlitchIndicator(), reg0c);
            }

            if (was_open && !open) {
                transitions++;
                if (!first_close) {
                    first_close   = true;
                    t_close       = now - t0;
                    close_at      = now;
                    s_squelch_open = false;
                    rx_apply_audio_gate();
                    t_gate        = systick_micros() - t0;
                    uart_printf("tail: close +%u ms rssi %u noise %u "
                                "glitch %u reg0c 0x%04X\n",
                                (unsigned)(t_close / 1000u), rssi,
                                BK4819_GetExNoiceIndicator(),
                                BK4819_GetGlitchIndicator(), reg0c);
                }
            }
            was_open = open;

            /* Why the chip waited: print the meters every 200 ms between the
             * RSSI fall and the close.  The 0x4E close delay, the RSSI close
             * threshold and the Ex-noise filter are the three candidates. */
            if (first_fall && !first_close &&
                (int32_t)(now - last_diag) >= 200000) {
                last_diag = now;
                uart_printf("tail: +%u ms rssi %u noise %u glitch %u "
                            "reg0c 0x%04X\n",
                            (unsigned)((now - t0) / 1000u), rssi,
                            BK4819_GetExNoiceIndicator(),
                            BK4819_GetGlitchIndicator(), reg0c);
            }
            if (first_close &&
                (int32_t)(systick_micros() - close_at) > 200000) {
                break;      /* 200 ms after the close is enough for chatter */
            }
        }

        next += TAIL_SAMPLE_US;
        while ((int32_t)(systick_micros() - next) < 0) {
        }
    }

    uart_printf("tail: fall %s%u us, close %s%u us, gate %s%u us; "
                "close-fall %d us, gate-close %d us; transitions %u\n",
                first_fall ? "" : "(none) ", (unsigned)t_fall,
                first_close ? "" : "(none) ", (unsigned)t_close,
                first_close ? "" : "(none) ", (unsigned)t_gate,
                (int)((int32_t)t_close - (int32_t)t_fall),
                (int)((int32_t)t_gate - (int32_t)t_close),
                transitions);
    uart_puts("tail: the app gate resumes on the next rx poll\n");
    rx_tail_restore_vfo();
}

uint16_t rx_rssi(void)
{
    return s_rssi;
}

uint32_t rx_rx_frequency(void)
{
    return s_freq_10hz;
}

/* The app loop's receive service, moved here from the old GUI shim.
 *
 * It polls the squelch, retunes when the K1's selected VFO frequency moves
 * (this is the only place that tracks it, so the configured RX frequency stays
 * authoritative even on a path that skips RADIO_SetupRegisters), and publishes
 * `g_SquelchLost`.  The K1 app's receive path keys off that flag --
 * CheckForIncoming() promotes FOREGROUND to INCOMING on it -- but the chip's
 * squelch interrupt is never armed here, so the polled result is what keeps the
 * screen, status line and green LED in step. */
void rx_service(void)
{
    static bool squelch_open;

    /* The FM feature owns the receiver and the audio while it is up (see
     * rx_set_fm_active()): leave the BK4829 alone so it cannot drive the
     * amplifier node out from under the BK1080. */
    if (s_fm_active)
        return;

    /* Keep both transceivers on their own VFO.  This is now the only place the
     * BK4829 is retuned from: the primary VFO (the BK4829 one) drives it, so
     * with one VFO on each chip the BK4829 does not follow the receiver onto
     * the BK4815's VFO. */
    if (s_scan_source == RX_SCAN_SOURCE_DEFAULT) {
        s_scan_source_applied = RX_SCAN_SOURCE_DEFAULT;

        /* While a scan runs, the scanner owns the scan lane's chip and walks
         * gRxVfo's frequency as its cursor.  Retuning/mirroring/repairing here
         * would fight the probes (and, in LISTEN mode, move the listening
         * lane), so the coordinator yields for the duration and re-applies
         * when the scan stops (CHFRSCANNER_Stop clears the override). */
        if (gScanStateDir == SCAN_OFF) {
            rf_dual_refresh();

            /* The receive audio follows the selected VFO's transceiver, so a
             * VFO on the BK4815 is audible through that chip's own AF. */
            rx_set_audio_source(SETTINGS_GetVfoTransceiver(gEeprom.RX_VFO) == RF_XCVR_BK4815);
        }
    } else {
        /* A paused scan hit: follow the chip that found it, not the saved VFO
         * assignment.  The K1 receive setup only tunes the BK4829 when the
         * selected VFO is assigned to it, so each hit's chip is retuned here
         * (a BK4815 hit on the BK4815, a BK4829 hit on the BK4829). */
        const bool     use_4815 = (s_scan_source == RX_SCAN_SOURCE_BK4815);
        const uint32_t f        = gRxVfo->freq_config_RX.Frequency;

        if (s_scan_source_applied != s_scan_source || s_scan_source_freq != f) {
            if (use_4815) {
                bk4815_set_frequency(f, false);
                bk4815_set_band((f >= PA_BAND_SPLIT) ? 0x0Au : 0x11u);
            } else {
                rx_set_frequency(f);
            }
            s_scan_source_applied = s_scan_source;
            s_scan_source_freq    = f;
        }
        rx_set_audio_source(use_4815);
    }

    /* The airband's stock receive path (0x33 pin 0 clear) changes the RSSI
     * scale by ~37 dB.  A floor learned on the other path would leave the gate
     * open on noise, so drop the estimates on entering or leaving 108-137 MHz
     * and let them re-learn from the new scale. */
    {
        static bool was_airband;
        const uint32_t f = s_audio_4815 ? s_4815_freq : s_freq_10hz;
        const bool     airband = (f >= 10800000u && f < 13700000u);

        if (airband != was_airband) {
            was_airband = airband;
            rx_floor_invalidate();
        }
    }

    if (rx_ready())
        rx_poll();

    if (rx_ready() && rx_squelch_open() != squelch_open) {
        squelch_open = rx_squelch_open();

        g_SquelchLost = squelch_open;
        if (!tx_active())
            led_set(squelch_open ? LED_GREEN : LED_OFF);
        if (squelch_open) {
            #ifdef ENABLE_FEAT_F4HWN_RX_TX_TIMER
                gRxTimerCountdown_500ms = 7200;
            #endif
        }
        gUpdateDisplay = true;
        gUpdateStatus  = true;
    }
}

/* The gEeprom glue the pure coordinator (rf_dual.c) does not carry: resolve the
 * per-VFO transceiver setting, choose the roles and tune the secondary.  Called
 * from rx_service() and once at boot. */
/* The last applied roles/frequencies, so rf_dual_refresh only retunes on a
 * change.  -2 = "never applied". */
static int      s_dual_pri   = -2;
static int      s_dual_sec   = -2;
static uint32_t s_dual_pri_f = 0u;
static uint32_t s_dual_sec_f = 0u;
static uint8_t  s_dual_rx_vfo = 0xFFu;   /* the RX_VFO the state was applied for */
static uint32_t s_4829_mirror_f;         /* unassigned BK4829: the BK4815 tune mirrored onto it */

void rf_dual_reapply(void)
{
    s_dual_pri = -2;
    s_dual_sec = -2;
    s_dual_rx_vfo = 0xFFu;
    /* Force the mirror to be re-applied as well (the TX paths retune the 4829). */
    s_4829_mirror_f = 0u;
}

void rf_dual_refresh(void)
{
    const bool      a_4829 = SETTINGS_GetVfoTransceiver(0u) != RF_XCVR_BK4815;
    const bool      b_4829 = SETTINGS_GetVfoTransceiver(1u) != RF_XCVR_BK4815;
    const rf_dual_roles_t roles = rf_dual_choose(a_4829, b_4829, gEeprom.RX_VFO);
    uint32_t        f4829 = 0u;
    uint32_t        f4815 = 0u;

    /* Switching VFOs has to re-enter the selected lane's receive
     * configuration, the way the stock does: its receive entry (FUN_08016cec)
     * tunes the part and applies the demodulator set, the band register and
     * the receive-path pin *together*, every time a lane is (re)configured --
     * a VFO switch included.  The port split those steps and ran the tune only
     * when the BK4815's role or frequency changed, so "switch to a BK4815 VFO"
     * left the part un-retuned while the K1's own radio setup had moved its
     * band/path state: the RSSI still read the carrier (the squelch opened)
     * but the demodulator produced noise, and editing the frequency and back
     * -- which forces the re-apply -- was the workaround (radio 2026-10-18).
     * Selecting another VFO now invalidates the applied state so both lanes
     * re-apply, exactly what a frequency change does. */
    if (gEeprom.RX_VFO != s_dual_rx_vfo) {
        s_dual_rx_vfo = gEeprom.RX_VFO;
        s_dual_pri    = -2;
        s_dual_sec    = -2;
    }

    if (roles.bk4829_vfo >= 0)
        f4829 = gEeprom.VfoInfo[roles.bk4829_vfo].freq_config_RX.Frequency;
    if (roles.bk4815_vfo >= 0)
        f4815 = gEeprom.VfoInfo[roles.bk4815_vfo].freq_config_RX.Frequency;
    s_4815_freq = f4815;

    /* An unassigned BK4829 -- both VFOs on the BK4815, the usual setup here --
     * is left receiving on whatever frequency it had last.  Its RX chain is
     * the audio path the BK4815's audio rides (radio 2026-10-18: parking its
     * demodulator silenced the receiver completely, and muting its AF output
     * did the same), and while it sits on another frequency its demodulated
     * noise sums into that path -- which is the "noise, squelch open" fault
     * chased for so long.  The one state that works is the 4829 on the *same*
     * channel as the BK4815 (the good capture had exactly that, RSSI 281).
     * So mirror the BK4815's tune onto the unassigned 4829: its demod then
     * sees the same carrier, its noise is the carrier's, and the shared audio
     * path stays alive.  A VFO that names the chip tunes it itself. */
    if (roles.bk4829_vfo < 0) {
        if (f4815 != 0u && f4815 != s_4829_mirror_f) {
            BK4819_SetFrequency(f4815);
            s_4829_mirror_f = f4815;
        }
    } else {
        s_4829_mirror_f = 0u;
    }

    /* The BK4815's own state -- its band register 0x75 and its receive-path
     * pin -- belongs to the tune the BK4815 is on, but two other paths write
     * it as a side effect:
     *   - pa_select_band() writes 0x75 from *whatever frequency it is handed*,
     *     and the BK4829's retune path and the TX path hand it the BK4829's;
     *     it also clears the receive-path pin through pa_apply_chip_path().
     *   - the K1's own radio setup clears the pin through
     *     BK4819_PickRXFilterPathBasedOnFrequency() every time it configures
     *     a BK4829-bound VFO -- i.e. on every VFO switch and end-of-reception
     *     reconfigure.
     * The repair used to run only when the BK4815's role or frequency
     * changed, so the sequence "switch to a BK4829 VFO, switch back" left the
     * pin cleared and the band stale: the RSSI still read the carrier (the
     * squelch opened) but the demodulator produced noise, and a frequency
     * edit-and-back -- which forces the re-apply -- was the workaround.
     * Own both here instead: assert/clear them after every pass's retunes
     * (which are the clobberers), writing the register only when the value
     * actually changes. */

    /* The BK4829 is retuned to its VFO here, not from the receiver's VFO: the
     * receiver may be following the BK4815 one. */
    if (f4829 != 0u && (roles.bk4829_vfo != s_dual_pri || f4829 != s_dual_pri_f)) {
        rx_set_frequency(f4829);
        s_dual_pri   = roles.bk4829_vfo;
        s_dual_pri_f = f4829;
    }

    if (roles.bk4815_vfo != s_dual_sec || f4815 != s_dual_sec_f) {
        rx_gate_hold_closed();      /* the BK4815 retune below is a frequency change */
        rf_dual_apply(&roles, f4815);
        /* The rest of the stock's per-VFO receive entry (FUN_08016CEC): the
         * demod/baseband/AF block and the channel-spacing bit in 0x01, taken
         * from this VFO's own bandwidth.  It runs on the (re)configuration, not
         * on every pass, and never from the scanner's probes. */
        if (roles.bk4815_vfo >= 0)
            bk4815_receive_entry(gEeprom.VfoInfo[roles.bk4815_vfo].CHANNEL_BANDWIDTH
                                 == BANDWIDTH_WIDE);
        s_dual_sec   = roles.bk4815_vfo;
        s_dual_sec_f = f4815;
    }

    if (roles.bk4815_vfo >= 0) {
        static uint16_t s_band_written;

        {
            const uint16_t band = (f4815 >= PA_BAND_SPLIT) ? 0x0Au : 0x11u;

            if (band != s_band_written) {
                bk4815_set_band(band);
                s_band_written = band;
            }
        }
    }

    /* The 0x33 receive-path pin is the *lane's*, set by rx_set_audio_source()
     * through pa_set_rx_lane(): a VFO merely naming the BK4815 no longer sets
     * it, because that costs the BK4829 ~16 dB at VHF when it is the lane being
     * heard (measured, docs/ra89r_rfpath.md). */
}
