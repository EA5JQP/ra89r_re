#include "driver/tx.h"

#include "app/bt.h"             /* the earpiece's PTT button */
#include "driver/bk4815.h"
#include "driver/bk4819.h"
#include "driver/led.h"
#include "driver/pa.h"
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

    bt_set_radio_tx_active(true);

    /* The band state first (PA0/PA1, the BK4815 0x75 band and the RX path),
     * then the TX band pin -- pa_tx_enable() overwrites 0x33 and sets 0x36 and
     * the PB14 bias PWM from `power`. */
    pa_select_band(freq_10hz);
    pa_tx_enable(power);
    bk4815_write_reg(0x0C, pa_band_is_main() ? 0x0203u : 0xFFFBu);  /* the T/R path */
    BK4819_SetFrequency(freq_10hz);
    BK4819_WriteRegister(BK4819_REG_7D, TX_REG7D_POWER);
    BK4819_PrepareTransmit();               /* 0x37 = 0x9D1F, 0x30 = 0xC1FE */
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
    /* Stock FUN_080177A8 re-opens BT_CALL when the radio returns to RX and the
     * earpiece is still linked.  A SCO-disconnect event during PTT clears the
     * call state but is not a BT device disconnect. */
    bt_set_radio_tx_active(false);
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
    const bool       down = (key == KEY_PTT) || (key == KEY_PTT2) ||
                            bt_ptt_down();      /* the earpiece's PTT button */

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
