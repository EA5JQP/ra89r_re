#include "driver/tx.h"

#include "driver/audio_path.h"
#include "driver/bk4815.h"
#include "driver/bk4819.h"
#include "driver/led.h"
#include "driver/pa.h"
#include "driver/rx.h"
#include "driver/keyboard.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"

static bool s_active;
static tx_source_t s_source;

/* The 'w' sweep: one extra stock TX register on top of the K1 chain, cycled
 * while PTT is held so the radio can settle which one the PA needs. */
static uint8_t s_variant;

uint8_t tx_variant(void) { return s_variant; }

void tx_variant_next(void) { s_variant = (uint8_t)((s_variant + 1u) % 6u); }

/* ---- Swap mode ('y'): receive on the BK4829, transmit on the BK4815 -------
 *
 * The stock's repeat entry programs the BK4815 for transmit (`FUN_080171d0`,
 * including 0x6d = 0xE718, the datasheet's transmit PA value) and leaves the
 * receiving chip untouched -- so the receiving chip's AF stays valid.  That is
 * the only arrangement consistent with the stock hearing its downlink while
 * transmitting.  The port's normal chain is the opposite (TX on the 4829), so
 * this mode uses the 4815's own transmit set and never writes the 4829. */
static bool s_swap;

void tx_set_swap(bool on) { s_swap = on; }
bool tx_swap(void) { return s_swap; }

static bool    s_duplex;
static bool    s_duplex_rx_is_bk4815;
/* Default: Normal AF on the transmit chip.  On this board the BK4815's audio
 * leaves through the BK4829's AF node (measured), so muting the 4829 at TX
 * entry (the stock's 0x6040) silences the downlink even though the 4815 still
 * receives it.  Mode 0 is the stock behaviour, kept selectable with 'I'. */
static uint8_t s_duplex_af = 1;  /* 0 stock mute, 1 normal AF, 2 K1 mute */
/* Keep the BK4829's AF DAC (0x30 bit 9) enabled during a duplex transmission.
 * Default ON here to recover the "beep while TX+RX" state: with the AF DAC
 * alive the speaker node stays powered during a transmission, which is the one
 * state where receive audio could coexist with TX.  'H' toggles it. */
static bool    s_duplex_af_dac = true;
/* PC13 (the audio path line) during a duplex transmission: 0 = leave it as the
 * receive path left it, 1 = drive low (the stock's T/R state), 2 = drive high.
 * rx_poll does not touch the line while tx_active(), so this is authoritative. */
static uint8_t s_duplex_pc13;
static uint8_t s_duplex_path;    /* 0 full band path, 1 leave PA0/PA1 */
static bool    s_duplex_tr;      /* write the BK4815's 0x0C at TX entry */
static uint32_t s_last_tx_freq;  /* the frequency the last tx_start() programmed */

uint32_t tx_last_frequency(void) { return s_last_tx_freq; }

uint16_t tx_duplex_af_value(void)
{
    switch (s_duplex_af) {
        case 1:  return 0x6142u;
        case 2:  return 0x6042u;
        default: return 0x6040u;
    }
}

bool tx_duplex_active(void) { return s_duplex; }

bool tx_set_duplex(bool on, bool rx_on_4815, bool tx_on_4829)
{
    if (!on) {
        s_duplex = false;
        s_duplex_rx_is_bk4815 = false;
        return true;
    }
    if (!rx_on_4815 || !tx_on_4829)
        return false;

    s_duplex              = true;
    s_duplex_rx_is_bk4815 = true;
    return true;
}

void tx_duplex_next_af(void)
{
    s_duplex_af = (uint8_t)((s_duplex_af + 1u) % 3u);
}

void tx_duplex_next_path(void)
{
    s_duplex_path = (uint8_t)((s_duplex_path + 1u) % 2u);
}

void tx_duplex_set_af(uint8_t mode)
{
    if (mode < 3u)
        s_duplex_af = mode;
}

void tx_duplex_set_path(uint8_t mode)
{
    if (mode < 2u)
        s_duplex_path = mode;
}

void tx_duplex_set_bk4815_tr(bool on)
{
    s_duplex_tr = on;
}

bool tx_duplex_bk4815_tr(void)
{
    return s_duplex_tr;
}

const char *tx_duplex_tr_name(void)
{
    return s_duplex_tr ? "BK4815 0x0C = 0x0203 at TX (the stock's T/R)"
                       : "BK4815 0x0C untouched at TX";
}

void tx_duplex_set_af_dac(bool on)
{
    s_duplex_af_dac = on;
}

bool tx_duplex_af_dac(void)
{
    return s_duplex_af_dac;
}

uint16_t tx_duplex_reg30_value(void)
{
    /* 0xC1FE = TX DSP + mic ADC; +0x0200 (AF DAC) is the stock's 0xC3FE. */
    return s_duplex_af_dac ? 0xC3FEu : 0xC1FEu;
}

const char *tx_duplex_af_dac_name(void)
{
    return s_duplex_af_dac ? "TX 0x30 = 0xC3FE (AF DAC kept: the RX audio node stays alive)"
                           : "TX 0x30 = 0xC1FE (AF DAC off, as the K1 TX chain leaves it)";
}

void tx_duplex_set_pc13(uint8_t mode)
{
    if (mode < 3u)
        s_duplex_pc13 = mode;
}

uint8_t tx_duplex_pc13(void)
{
    return s_duplex_pc13;
}

const char *tx_duplex_pc13_name(void)
{
    static const char *const names[] = {
        "PC13 left as RX had it",
        "PC13 driven low at TX (the stock's T/R state)",
        "PC13 driven high at TX",
    };

    return names[s_duplex_pc13];
}

const char *tx_duplex_af_name(void)
{
    static const char *const names[] = {
        "stock TX-entry mute (0x47 = 0x6040)",
        "Normal AF (0x47 = 0x6142, the BK4815's audio stays audible)",
        "K1 mute (0x47 = 0x6042)",
    };

    return names[s_duplex_af];
}

const char *tx_duplex_path_name(void)
{
    return s_duplex_path == 0u ? "full band path (PA0/PA1 written)"
                               : "PA0/PA1 left as RX had them";
}

/* The AF output the transmit chip gets in duplex mode, per the live mode. */
static void tx_duplex_apply_af(void)
{
    BK4819_WriteRegister(BK4819_REG_47, tx_duplex_af_value());
}

/* ---- Route probe (console 'o'): does the BK4829's microphone path carry the
 * BK4815's receive audio?  See tx.h for the question and the method.
 *
 * Three things have to be true before the level means anything:
 *   - the BK4815 must actually be demodulating and its AF output open (its
 *     `0x49` gate is shut while the receive squelch is closed, and the console
 *     blocks `rx_poll()` for the probe's duration);
 *   - the speaker must be silent, or a route-1 downlink would reach the
 *     microphone acoustically;
 *   - the microphone path must be powered and the reading must respond to a
 *     real voice, which is what the console's control phase proves before it
 *     compares the carrier levels.
 * The PA is never enabled, so nothing is radiated and the 4829's TX chain runs
 * only as far as the DSP. */
static bool     s_route;
static uint8_t  s_route_mic_mode;
static uint16_t s_route_reg31;          /* 0x31 before the stock's VOX bit */
static bool     s_route_reg31_saved;

bool tx_route_probe_active(void)
{
    return s_route;
}

uint8_t tx_route_probe_mic_mode_get(void)
{
    return s_route_mic_mode;
}

void tx_route_probe_mic_mode(uint8_t mode)
{
    if (!s_route)
        return;

    s_route_mic_mode = (uint8_t)(mode != 0u);

    if (s_route_mic_mode == 0u) {
        /* The transmit entry's microphone path: mic ADC + TX DSP, the TX
         * unmute and the measured mic gain. */
        BK4819_WriteRegister(BK4819_REG_30, 0xC1FEu);
        BK4819_WriteRegister(BK4819_REG_50, TX_REG50_UNMUTE);
        BK4819_WriteRegister(BK4819_REG_40,
                             (uint16_t)(0x3000u | ((uint16_t)TX_MIC_GAIN << 4)));
        return;
    }

    /* The stock's VOX enable (`FUN_0801754A`: 0x46/0x79/0x7A, 0x31 bit 2,
     * 0x30 = 0xBDF1).  This is the microphone path the chip uses while
     * receiving, and 0x64 is the stock's own VOX-amplitude register. */
    if (!s_route_reg31_saved) {
        s_route_reg31       = BK4819_ReadRegister(BK4819_REG_31);
        s_route_reg31_saved = true;
    }
    BK4819_WriteRegister(BK4819_REG_46, 0xA00Au);
    BK4819_WriteRegister(BK4819_REG_79, 0x6005u);
    BK4819_WriteRegister(BK4819_REG_7A, 0x589Au);
    BK4819_WriteRegister(BK4819_REG_31,
                         (uint16_t)(s_route_reg31 | (1u << 2)));
    BK4819_WriteRegister(BK4819_REG_30, 0xBDF1u);
}

bool tx_route_probe_begin(void)
{
    if (s_route)
        return true;
    if (!s_duplex || !s_duplex_rx_is_bk4815)
        return false;

    /* Open the receive lane's audio: the app's own gate is the BK4815's 0x49,
     * and the console blocks the poller that opens it.  Volume at the maximum
     * (REG73 nibble) so the level is unambiguous if the path exists. */
    bk4815_enter_rx();
    bk4815_write_reg(BK4815_REG_AF, 0x1A0Fu);

    /* Speaker silent: under the summed-node route that also loads the node, so
     * the microphone cannot hear the downlink by any acoustic path. */
    BK4819_SetAF(BK4819_AF_MUTE);

    s_route_reg31_saved = false;
    s_route             = true;
    tx_route_probe_mic_mode(0);
    return true;
}

uint16_t tx_route_probe_vox_amp(void)
{
    uint16_t amp = 0;

    BK4819_GetVoxAmp(&amp);
    return amp;
}

void tx_route_probe_end(void)
{
    if (!s_route)
        return;

    s_route = false;
    if (s_route_reg31_saved)
        BK4819_WriteRegister(BK4819_REG_31, s_route_reg31);
    s_route_reg31_saved = false;

    BK4819_RX_TurnOn();                     /* 0x37 = 0x9F1F, 0x30 = 0xBFF1 */
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_50, TX_REG50_UNMUTE);
    BK4819_WriteRegister(BK4819_REG_40, 0x3516u);   /* the RX value */

    /* Closed; the app's poller re-opens it with the squelch. */
    bk4815_set_af(false);
}


const char *tx_variant_name(void)
{
    static const char *const names[] = {
        "base: the K1 chain as measured",
        "0x47 = 0x6142 (the stock's Normal AF)",
        "0x13 = 0x03FF (the squelch ramp max)",
        "0x36 = 0x8822 (the bench's value)",
        "0x31 bit 2 cleared",
        "PC13 high (the stock's T/R line)",
    };

    return names[s_variant];
}

/* Apply the current variant to the chip.  Called from tx_start() and from the
 * console ('w'), so a running transmission can be changed without re-keying. */
void tx_variant_apply(void)
{
    switch (s_variant) {
        case 1: BK4819_WriteRegister(BK4819_REG_47, 0x6142u); break;
        case 2: BK4819_WriteRegister(BK4819_REG_13, 0x03FFu); break;
        case 3: BK4819_WriteRegister(BK4819_REG_36, 0x8822u); break;
        case 4: {
            const uint16_t v = BK4819_ReadRegister(BK4819_REG_31);

            BK4819_WriteRegister(BK4819_REG_31, (uint16_t)(v & ~(1u << 2)));
            break;
        }
        case 5: audio_path_drive(1); break;
        default: break;
    }
}

void tx_init(void)
{
    pa_init();
}

/* The BK4815 transmit chain, from the stock's `FUN_080171d0` (the 4815's own
 * PA, 0x6d = 0xE718) -- the swap mode's counterpart to the 4829 chain. */
static void tx_start_4815(uint32_t freq_10hz, uint8_t power, tx_source_t source)
{
    pa_set_touch_4815(true);        /* the 4815 is the transmit chip here */
    pa_set_touch_chip_path(false);  /* the 4829 is receiving: leave it alone */
    pa_select_band(freq_10hz);      /* PA0/PA1 + the 4815's 0x75 band */

    bk4815_set_frequency(freq_10hz, true);
    bk4815_write_reg(0x12u, 0x415cu);
    bk4815_write_reg(0x0cu, 0xf823u);
    bk4815_write_reg(0x16u, 0x3200u);
    bk4815_write_reg(0x2bu, 0x803fu);
    bk4815_write_reg(0x6du, 0xe718u);   /* the datasheet's transmit PA value */
    bk4815_write_reg(0x73u, 0x8000u);
    bk4815_write_reg(0x11u, 0x5800u);
    bk4815_write_reg(0x12u, 0x0148u);
    bk4815_set_af(true);                /* modulator input stage */

    /* The PA is shared: its control registers (0x36/0x33) and the PB14 bias
     * live on the 4829.  Enabling them does not touch 0x30, so the 4829 stays
     * a receiver -- it just supplies the PA hardware. */
    pa_tx_enable(power);

    led_set(LED_RED);
    s_active = true;
    s_source = source;
}

void tx_start(uint32_t freq_10hz, uint8_t power, tx_source_t source)
{
    const bool duplex = s_duplex && s_duplex_rx_is_bk4815;

    if (s_active && source == s_source)
        return;

    s_last_tx_freq = freq_10hz;

    if (s_swap) {
        tx_start_4815(freq_10hz, power, source);
        return;
    }

    /* In duplex the BK4815 is receiving: leave its band register (0x75) alone,
     * and optionally the shared PA0/PA1 pins too ('O' mode 1). */
    pa_set_touch_4815(!duplex);
    pa_set_touch_band_pins(!(duplex && s_duplex_path == 1u));

    /* The band state first (PA0/PA1, the BK4815 0x75 band and the RX path). */
    pa_select_band(freq_10hz);
    if (!duplex) {
        /* Above 134 MHz the BK4815 is part of the transmit path (the stock's
         * `0x0C = 0x0203`, with the BK4829 as the modulator and PA control).  Put it
         * on the TX frequency in TX mode (`0x70 = 0xE000`) rather than leaving it on
         * whatever receive frequency the coordinator last set -- otherwise a
         * repeater offset or the other VFO leaves it on the wrong channel. */
        if (pa_band_is_main())
            bk4815_set_frequency(freq_10hz, true);
        bk4815_write_reg(0x0C, pa_band_is_main() ? 0x0203u : 0xFFFBu);  /* the T/R path */
    }
    BK4819_SetFrequency(freq_10hz);
    BK4819_WriteRegister(BK4819_REG_7D, TX_REG7D_POWER);
    BK4819_PrepareTransmit();               /* 0x37 = 0x9D1F, 0x30 = 0xC1FE */
    /* The PA enable (`0x36`, PA-CTL + bias + gain), the TX band pin (`0x33`) and
     * the PB14 bias PWM must be applied **after** `BK4819_PrepareTransmit()`:
     * its `BK4819_TxOn_Beep()` writes `0x36 = 0`, so enabling the PA before it
     * leaves the whole transmission unamplified -- the chip's own low-level
     * carrier only, which a nearby receiver hears but a power meter reads as
     * nothing.  The validated bench wrote `0x36` after this call for exactly
     * this reason (docs/ra89r_rfpath.md, "The transmit configuration"). */
    pa_tx_enable(power);
    BK4819_SetAF(BK4819_AF_MUTE);
    if (duplex) {
        /* The K1's PrepareTransmit wrote 0xC1FE; optionally bring the AF DAC
         * (bit 9) back -- but with TX DSP running that loops the TX audio to
         * the speaker (the stock's 0xC3FE is its beep word, not a receive
         * route), so it is off by default. */
        BK4819_WriteRegister(BK4819_REG_30, tx_duplex_reg30_value());
        tx_duplex_apply_af();
        if (s_duplex_pc13 == 1u)
            audio_path_drive(0);
        else if (s_duplex_pc13 == 2u)
            audio_path_drive(1);
        if (s_duplex_tr)
            bk4815_write_reg(0x0Cu, 0x0203u);
    }

    if (source == TX_SOURCE_TONE) {
        /* The chip's tone generator, over the air -- the K1's PlayDTMFEx order,
         * which unmutes *after* loading the tone. */
        BK4819_EnterDTMF_TX(false);
        BK4819_PlayDTMF('5');
        BK4819_ExitTxMute();
        BK4819_WriteRegister(BK4819_REG_50, TX_REG50_UNMUTE);
    } else {
        BK4819_WriteRegister(BK4819_REG_50, TX_REG50_UNMUTE);
        BK4819_WriteRegister(BK4819_REG_40,
                             (uint16_t)(0x3000u | ((uint16_t)TX_MIC_GAIN << 4)));
    }

    /* The 'w' sweep: one extra stock register on top of the K1 chain.  The
     * stock's own TX (`FUN_08016228`) sends these and the K1 chain does not, so
     * this is what settles which one the PA needs. */
    tx_variant_apply();

    led_set(LED_RED);                       /* red = transmit, as the stock shows it */

    s_active = true;
    s_source = source;
}

void tx_stop(void)
{
    if (!s_active)
        return;

    if (s_swap) {
        /* Back to a safe 4815 state; the 4829 (the receiver) was only touched
         * through the shared PA registers, which this restores. */
        pa_rx_enable();
        bk4815_write_reg(0x6du, 0x6618u);   /* the receive PA value */
        bk4815_write_reg(0x0cu, 0x0a03u);
        bk4815_write_reg(0x73u, 0x81d0u);
        bk4815_enter_rx();
        rx_4815_set_applied();
        pa_set_touch_chip_path(true);
        led_set(LED_OFF);
        s_active = false;
        return;
    }

    if (s_duplex && s_duplex_rx_is_bk4815) {
        /* Duplex: only the transmit chip is restored.  The BK4815 keeps its
         * frequency, band register, T/R state and AF, so its receive is never
         * interrupted -- not even at PTT release. */
        if (s_duplex_tr)
            bk4815_write_reg(0x0Cu, 0x0A03u);
        bk4815_enter_rx();                  /* the demod set survives the TX */
        rx_4815_set_applied();              /* its writes nudge the RSSI */
        pa_rx_enable();                     /* 0x33 back, 0x36 = 0, compare 0 */
        BK4819_RX_TurnOn();
        BK4819_SetAF(BK4819_AF_MUTE);
        BK4819_WriteRegister(BK4819_REG_50, TX_REG50_UNMUTE);
        BK4819_WriteRegister(BK4819_REG_40, 0x3516u);   /* the RX value */
        led_set(LED_OFF);

        s_active = false;
        pa_set_touch_4815(true);
        pa_set_touch_band_pins(true);
        /* The TX retuned the BK4829 to the transmit frequency; ask the
         * coordinator to put both lanes back (the ordinary path below does the
         * same).  Without it the mirror shadow still claimed the 4829 was on
         * the downlink and the radio was left hearing its transmit-frequency
         * demod noise after a duplex test (radio 2026-10-19). */
        rf_dual_reapply();
        return;
    }

    pa_rx_enable();                         /* 0x33 back, 0x36 = 0, compare 0 */
    bk4815_write_reg(0x0C, pa_band_is_main() ? 0x0A03u : 0xFFFBu);
    /* A TX above 134 MHz switched the BK4815's operation-control register
     * (0x70) to E000 in bk4815_set_frequency(..., true).  Restore its RX mode
     * even when neither VFO is assigned to it: rf_dual_reapply() only retunes
     * an assigned BK4815, so otherwise the unused part remains in TX mode and
     * can load the shared RF path after PTT is released. */
    bk4815_write_reg(0x70u, 0xA000u);
    BK4819_RX_TurnOn();
    BK4819_SetAF(BK4819_AF_MUTE);
    /* Back to receive, so the audio path goes to its ON value (`0x3B20`), not
     * the muted `0xBB18`: the squelch is the thing that mutes receive audio, via
     * `0x47` (SetAF), and it cannot undo a `0x50` mute.  The stock writes
     * `0x3B20` per channel; the K1's `ExitTxMute` is the same word. */
    BK4819_WriteRegister(BK4819_REG_50, TX_REG50_UNMUTE);
    BK4819_WriteRegister(BK4819_REG_40, 0x3516u);       /* the RX value */
    led_set(LED_OFF);

    s_active = false;

    /* tx_start retuned the BK4829 (and, above 134 MHz, the BK4815) to the TX
     * frequency; ask the coordinator to put both back on their receive
     * frequencies, which matters on an offset channel. */
    rf_dual_reapply();
}

bool tx_active(void) { return s_active; }
tx_source_t tx_source(void) { return s_source; }

/* PTT from the keypad.  The K1's own PTT path runs its chip sequence, which this
 * radio has not compared against the stock, so PTT drives the measured chain
 * (this file) instead -- see docs/ra89r_rfpath.md.  The K1 function state is set
 * too, so the screens show TX and the status line follows.  Called by the app
 * loop (the K1's CheckKeys() deliberately leaves PTT alone here). */
void tx_poll_ptt(void)
{
    const KEY_Code_t key  = KEYBOARD_GetKey();
    const bool       down = (key == KEY_PTT) || (key == KEY_PTT2);

    /* A console-bench transmission ('T', the chip's own DTMF tone) is not PTT:
     * it must hold until the console stops it.  Otherwise this poller sees
     * "PTT not pressed" while tx_active() is true and cancels the bench TX on
     * the very next loop pass -- the radio emits only a brief burst, which is
     * exactly what a power meter reads as nothing. */
    if (!down && tx_active() && tx_source() == TX_SOURCE_TONE)
        return;

    if (down == tx_active())
        return;

    if (down) {
        /* In swap mode the BK4829 is the receiver: entering the K1's transmit
         * state would run RADIO_SetTxParameters() and take the 4829 out of RX
         * (0x30 = 0xC1FE, RSSI 0).  Keep the receive state; driver/tx.c owns
         * the 4815 transmission. */
        if (!tx_swap())
            FUNCTION_Select(FUNCTION_TRANSMIT);
        tx_start(gTxVfo->freq_config_TX.Frequency,
                 gTxVfo->TXP_CalculatedSetting, TX_SOURCE_MIC);
    } else {
        tx_stop();
        FUNCTION_Select(FUNCTION_RECEIVE);
    }
    gUpdateDisplay = true;
}
