#include "driver/rx.h"

#include "driver/audio_path.h"
#include "driver/bk4815.h"
#include "driver/bk4819.h"
#include "driver/bk4829.h"
#include "driver/pa.h"
#include "driver/rf_dual.h"
#include "driver/systick.h"
#include "driver/tx.h"
#include "driver/led.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"

static bool s_ready;
static bool s_squelch_open;
static bool s_fm_active;        /* the FM feature owns the audio; see rx_set_fm_active() */
static uint16_t s_rssi;
static uint32_t s_freq_10hz;    /* what the BK4829 was last tuned to */
static bool s_af_4815_written;  /* the BK4815 AF gate's applied value... */
static bool s_af_4815_valid;    /* ...and whether that shadow can be trusted */
static uint16_t s_band_4815_written;   /* the BK4815 band register 0x75's last written value */

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

    s_ready = true;
    s_squelch_open = false;
}

void rx_set_frequency(uint32_t freq_10hz)
{
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
static bool s_force_4815_af;
static bool s_bridge_4815 = true;   /* open the BK4829 AF with the 4815's squelch */

void rx_set_4815_bridge(bool on)
{
    s_bridge_4815 = on;
    if (!on)
        BK4819_SetAF(BK4819_AF_MUTE);
}

bool rx_4815_bridge(void)
{
    return s_bridge_4815;
}    /* console 'A' diagnostic */

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
}

void rx_clear_scan_source_override(void)
{
    s_scan_source = RX_SCAN_SOURCE_DEFAULT;
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
    if (use_4815 == s_audio_4815)
        return;

    s_audio_4815 = use_4815;
    /* Mute both and let the squelch open the selected one.  The BK4815's audio
     * shares the BK4829's AF node (measured: it is only audible while the
     * BK4829's AF is open, e.g. with the K1 squelch at 0), so the BK4815 source
     * must open the BK4829's AF as well as its own 0x49. */
    BK4819_SetAF(BK4819_AF_MUTE);
    bk4815_set_af(false);
    s_squelch_open = false;
}

/* Applying the BK4815's receive set (bk4815_enter_rx) nudges the part: its RSSI
 * spikes for a moment, and acting on that would open the audio gates on the
 * demodulator's relock transient -- the burst a squelch opening used to make.
 * The gates stay shut for this long after the set, then the squelch judges the
 * settled RSSI. */
#define RX4815_SETTLE_MS 150u

static uint32_t s_settle_until;         /* 0 = not settling */

void rx_poll(void)
{
    if (!s_ready)
        return;

    if (s_audio_4815) {
        /* Squelch 0 is the K1's "always open": RADIO_ConfigureSquelchAndOutputPower()
         * forces the BK4829's chip-squelch thresholds to 0 for it.  This
         * software gate must honour it too, or a BK4815 VFO stays muted below
         * the fixed mark even with the user's squelch turned off. */
        const bool squelch_off = (gEeprom.SQUELCH_LEVEL == 0u);

        const bool was_open = s_squelch_open;
        const bool settling = (s_settle_until != 0u && systick_millis() < s_settle_until);

        s_rssi = bk4815_read_rssi();

        if (settling) {
            /* Just applied the receive set: the RSSI is not trustworthy yet,
             * so keep the squelch latch as it is and judge again when the
             * settle window has passed. */
        } else {
            s_settle_until = 0u;

            if (s_force_4815_af || squelch_off ||
                (!s_squelch_open && s_rssi >= RX4815_SQUELCH_OPEN_MARK))
                s_squelch_open = true;
            else if (s_squelch_open && !squelch_off && s_rssi < RX4815_SQUELCH_CLOSE_MARK)
                s_squelch_open = false;
        }

        /* Re-apply the stock's receive setup when a carrier appears: a
         * transmission (or the K1's own setup) can leave the part tuned but
         * with the demodulator back in its power-on state, which sounds like
         * noise.  Once per squelch opening, not every poll -- the squelch stays
         * latched open but the gates below stay shut until the set's own RSSI
         * nudge has settled, so the transient is never heard.  The restart
         * pair can also revert the part's AF gate (0x49) to its unmuted boot
         * value, so the gate shadow below is invalidated here. */
        if (s_squelch_open && !was_open) {
            bk4815_enter_rx();
            s_af_4815_valid = false;
            s_settle_until = systick_millis() + RX4815_SETTLE_MS;
        }

        {
            /* Recompute, not reuse: the transition above just armed the
             * settle window, and the pass that did it must not open the
             * gates on the transient (the pre-arming `settling` read 0
             * there -- the log caught the gate open in the same
             * millisecond as the set). */
            const bool settling_now = (s_settle_until != 0u &&
                                       systick_millis() < s_settle_until);
            const bool gates = s_squelch_open && !settling_now;

            /* The BK4815's AF gate has no outside writer: write it on a
             * change, like the stock's applier, not at 100 Hz -- but always
             * once after the receive set, whose restart pair can revert it.
             * The BK4829's 0x47 is different -- the K1 app writes it behind
             * our back, so the bridge keeps re-driving it every poll. */
            if (!s_af_4815_valid || gates != s_af_4815_written) {
                bk4815_set_af(gates);
                s_af_4815_written = gates;
                s_af_4815_valid = true;
            }
            if (s_bridge_4815)
                BK4819_SetAF(gates ? BK4819_AF_FM : BK4819_AF_MUTE);
            else
                BK4819_SetAF(BK4819_AF_MUTE);   /* hard mute, whatever the app writes */
            /* Leave the audio-path line alone during a duplex transmission: the
             * transmit side owns it then (console 'g'), and driving it here every
             * 10 ms would override that. */
            if (!tx_active())
                audio_path_drive(gates);
        }

        /* The K1 owns the BK4829's chip outputs, and its own power-save, wake
         * and RX-off paths clear them behind the port's back: RX_ENABLE
         * (GPIO0, `functions.c`'s FUNCTION_PowerSave and the RX-off helpers)
         * and -- through the RF-setup path -- the VHF-LNA pin (GPIO4).  The
         * BK4815's receive path rides those pins (the stock's BK4815 branch
         * sets pin 4 on the 4829), and rf_dual_refresh() only re-drives them
         * when the roles change, so a VFO switch used to leave the BK4815
         * receiving with the path pins down: RSSI still present but the audio
         * noise, and a frequency edit (which forces rf_dual_apply) "fixed" it.
         * Radio 2026-10-18: the noisy state read `0x33 = 0x9004` (RX_ENABLE
         * clear), a good one `0x9044`.  Re-assert them while the BK4815 is the
         * receive source; the register write happens only on a change. */
        if (!BK4819_IsGpioOutSet(BK4819_GPIO0_PIN28_RX_ENABLE))
            BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
        if (!BK4819_IsGpioOutSet(BK4819_GPIO4_PIN32_VHF_LNA))
            BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA, true);

        /* The BK4815's band register belongs to the tune the part is on, but
         * pa_select_band() writes it from whatever frequency it is given --
         * the scan's probes and the TX path use the BK4829's.  Keep it on the
         * BK4815's own tune (rf_dual_apply() sets the same value). */
        {
            const uint32_t f    = rf_dual_bk4815_frequency();
            const uint16_t band = (f >= PA_BAND_SPLIT) ? 0x0Au : 0x11u;

            if (f != 0u && band != s_band_4815_written) {
                bk4815_write_reg(0x75u, band);
                s_band_4815_written = band;
            }
        }
        return;
    }

    s_rssi = BK4819_GetRSSI();

    if (!s_squelch_open && s_rssi >= RX_SQUELCH_OPEN_MARK) {
        s_squelch_open = true;
        BK4819_SetAF(BK4819_AF_FM);
    } else if (s_squelch_open && s_rssi < RX_SQUELCH_CLOSE_MARK) {
        s_squelch_open = false;
        BK4819_SetAF(BK4819_AF_MUTE);
    }
}

/* Applying the BK4815 receive set outside the poller (the TX teardown paths in
 * driver/tx.c): the set nudges the part's RSSI, so arm the same settle window
 * rx_poll() uses for its own application of it.  The set's restart pair can
 * also revert the part's AF gate (0x49), so the poller's gate shadow is
 * invalidated. */
void rx_4815_set_applied(void)
{
    s_settle_until = systick_millis() + RX4815_SETTLE_MS;
    s_af_4815_valid = false;
}

bool rx_squelch_open(void)
{
    return s_squelch_open;
}

bool rx_audio_is_4815(void)
{
    return s_audio_4815;
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
    static uint32_t s_applied_4815_freq;

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
        rf_dual_refresh();

        /* The receive audio follows the selected VFO's transceiver, so a VFO on
         * the BK4815 is audible through that chip's own AF. */
        rx_set_audio_source(SETTINGS_GetVfoTransceiver(gEeprom.RX_VFO) == RF_XCVR_BK4815);
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
                bk4815_write_reg(0x75u, (f >= PA_BAND_SPLIT) ? 0x0Au : 0x11u);
            } else {
                rx_set_frequency(f);
            }
            s_scan_source_applied = s_scan_source;
            s_scan_source_freq    = f;
        }
        rx_set_audio_source(use_4815);
    }

    /* Pair the BK4815's receive set with its tune, as the stock does: its RX
     * entry (`FUN_08016CEC`) programs the frequency *and* the demodulator, and
     * a part that is tuned but never given the set demodulates noise (RSSI
     * follows the carrier, the audio does not).  The port used to apply the
     * set only on a squelch opening, so a VFO switch or a frequency edit onto
     * the BK4815 left it unset -- "noise after switching VFOs, fixed by
     * editing the frequency".  Apply it whenever the coordinator has (re)tuned
     * the part, with the settle window holding the audio gates through the
     * set's own transient. */
    {
        const uint32_t f4815 = rf_dual_bk4815_frequency();

        if (f4815 != 0u && f4815 != s_applied_4815_freq) {
            s_applied_4815_freq = f4815;
            bk4815_enter_rx();
            rx_4815_set_applied();
        }
    }

    if (rx_ready())
        rx_poll();

    if (rx_ready() && rx_squelch_open() != squelch_open) {
        squelch_open = rx_squelch_open();

        g_SquelchLost = squelch_open;
        if (!tx_active())
            led_set(squelch_open ? LED_GREEN : LED_OFF);
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

/* Whether the unassigned BK4829's demodulator is parked (see rf_dual_refresh). */
static bool s_4829_parked;

void rf_dual_reapply(void)
{
    s_dual_pri = -2;
    s_dual_sec = -2;
    /* Re-assert the parked BK4829 word on the next pass: the TX paths rewrite
     * register 0x30 themselves, so the shadow cannot be trusted after one. */
    s_4829_parked = false;
}

void rf_dual_refresh(void)
{
    const bool      a_4829 = SETTINGS_GetVfoTransceiver(0u) != RF_XCVR_BK4815;
    const bool      b_4829 = SETTINGS_GetVfoTransceiver(1u) != RF_XCVR_BK4815;
    const rf_dual_roles_t roles = rf_dual_choose(a_4829, b_4829, gEeprom.RX_VFO);
    uint32_t        f4829 = 0u;
    uint32_t        f4815 = 0u;

    if (roles.bk4829_vfo >= 0)
        f4829 = gEeprom.VfoInfo[roles.bk4829_vfo].freq_config_RX.Frequency;
    if (roles.bk4815_vfo >= 0)
        f4815 = gEeprom.VfoInfo[roles.bk4815_vfo].freq_config_RX.Frequency;

    /* An unassigned BK4829 is not idle.  Left RX-enabled on a stale frequency
     * it keeps demodulating, and the bridge (this same poller) sums its output
     * into the shared audio node the BK4815's audio rides -- with both VFOs on
     * the BK4815 that is the steady state, and the stale chip is a noise
     * source nothing mutes (its 0x47 mute does not stop the demod).  Park its
     * demodulator: drop the RX link, the demod DSP and the disc mode from
     * register 0x30, keep the AF DAC (the BK4815's audio is only audible
     * through that output stage) and the PLL/VCO.  Restore the full RX word
     * the moment a VFO names the chip again. */
    if (roles.bk4829_vfo < 0) {
        if (!s_4829_parked) {
            BK4819_WriteRegister(BK4819_REG_30, 0x82F0u);
            s_4829_parked = true;
        }
    } else if (s_4829_parked) {
        BK4819_RX_TurnOn();
        s_4829_parked = false;
    }

    /* The BK4829 is retuned to its VFO here, not from the receiver's VFO: the
     * receiver may be following the BK4815 one. */
    if (f4829 != 0u && (roles.bk4829_vfo != s_dual_pri || f4829 != s_dual_pri_f)) {
        rx_set_frequency(f4829);
        s_dual_pri   = roles.bk4829_vfo;
        s_dual_pri_f = f4829;
    }

    if (roles.bk4815_vfo != s_dual_sec || f4815 != s_dual_sec_f) {
        rf_dual_apply(&roles, f4815);
        s_dual_sec   = roles.bk4815_vfo;
        s_dual_sec_f = f4815;

        /* The receive RF-path pin follows the transceiver: the stock's BK4815
         * branch sets the BK4829's 0x33 pin 4 (`FUN_08016CEC` ->
         * `FUN_080137D4(0x10,0x10)`), the BK4829 branch clears it.  The port's
         * pa_apply_chip_path() clears both, so set pin 4 for a BK4815 VFO. */
        BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA,
                             roles.bk4815_vfo >= 0);
    }
}
