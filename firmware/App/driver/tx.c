#include "driver/tx.h"

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

void tx_init(void)
{
    pa_init();
}

void tx_start(uint32_t freq_10hz, uint8_t power, tx_source_t source)
{
    if (s_active && source == s_source)
        return;

    /* The band state first (PA0/PA1, the BK4815 0x75 band and the RX path). */
    pa_select_band(freq_10hz);
    /* Above 134 MHz the BK4815 is part of the transmit path (the stock's
     * `0x0C = 0x0203`, with the BK4829 as the modulator and PA control).  Put it
     * on the TX frequency in TX mode (`0x70 = 0xE000`) rather than leaving it on
     * whatever receive frequency the coordinator last set -- otherwise a
     * repeater offset or the other VFO leaves it on the wrong channel. */
    if (pa_band_is_main())
        bk4815_set_frequency(freq_10hz, true);
    bk4815_write_reg(0x0C, pa_band_is_main() ? 0x0203u : 0xFFFBu);  /* the T/R path */
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

    led_set(LED_RED);                       /* red = transmit, as the stock shows it */

    s_active = true;
    s_source = source;
}

void tx_stop(void)
{
    if (!s_active)
        return;

    pa_rx_enable();                         /* 0x33 back, 0x36 = 0, compare 0 */
    bk4815_write_reg(0x0C, pa_band_is_main() ? 0x0A03u : 0xFFFBu);
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
        FUNCTION_Select(FUNCTION_TRANSMIT);
        tx_start(gTxVfo->freq_config_TX.Frequency,
                 gTxVfo->TXP_CalculatedSetting, TX_SOURCE_MIC);
    } else {
        tx_stop();
        FUNCTION_Select(FUNCTION_RECEIVE);
    }
    gUpdateDisplay = true;
}
