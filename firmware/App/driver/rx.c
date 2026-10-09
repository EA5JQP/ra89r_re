#include "driver/rx.h"

#include "driver/audio_path.h"
#include "driver/bk4815.h"
#include "driver/bk4819.h"
#include "driver/bk4829.h"
#include "driver/pa.h"
#include "driver/rf_dual.h"
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
static bool s_force_4815_af;    /* console 'A' diagnostic */

/* A temporary scan-hit receive route (see rx.h).  DEFAULT follows the selected
 * VFO's own transceiver; the other values pin the hit's chip until cleared. */
static rx_scan_source_t s_scan_source = RX_SCAN_SOURCE_DEFAULT;

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

void rx_poll(void)
{
    if (!s_ready)
        return;

    if (s_audio_4815) {
        s_rssi = bk4815_read_rssi();

        if (s_force_4815_af || (!s_squelch_open && s_rssi >= RX4815_SQUELCH_OPEN_MARK))
            s_squelch_open = true;
        else if (s_squelch_open && s_rssi < RX4815_SQUELCH_CLOSE_MARK)
            s_squelch_open = false;

        /* Drive the three gates every poll, not only on a transition.  The K1's
         * own receive setup (RADIO_SetupRegisters -> RADIO_SetModulation, and
         * AUDIO_AudioPathOff) writes the shared AF node and the amplifier behind
         * our back, so a one-shot open at the transition leaves the audio muted
         * from then on.  The BK4815's own AF (0x49), the BK4829's AF (0x47, the
         * node the BK4815's audio rides) and the amplifier (PC13) must all be
         * open together for a BK4815 receive to be heard. */
        bk4815_set_af(s_squelch_open);
        BK4819_SetAF(s_squelch_open ? BK4819_AF_FM : BK4819_AF_MUTE);
        audio_path_drive(s_squelch_open);
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
        rf_dual_refresh();

        /* The receive audio follows the selected VFO's transceiver, so a VFO on
         * the BK4815 is audible through that chip's own AF. */
        rx_set_audio_source(SETTINGS_GetVfoTransceiver(gEeprom.RX_VFO) == RF_XCVR_BK4815);
    } else {
        /* A paused scan hit: follow the chip that found it, not the saved VFO
         * assignment.  The K1 receive setup drives the BK4829, so a BK4815 hit
         * has to be pointed at the hit frequency on the BK4815 itself. */
        const bool use_4815 = (s_scan_source == RX_SCAN_SOURCE_BK4815);

        if (use_4815) {
            const uint32_t f = gRxVfo->freq_config_RX.Frequency;

            bk4815_set_frequency(f, false);
            bk4815_write_reg(0x75u, (f >= 28000000u) ? 0x0Au : 0x11u);
        }
        rx_set_audio_source(use_4815);
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

void rf_dual_reapply(void)
{
    s_dual_pri = -2;
    s_dual_sec = -2;
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
