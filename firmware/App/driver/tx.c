#include "driver/tx.h"

#include "driver/bk4815.h"
#include "driver/bk4819.h"
#include "driver/led.h"
#include "driver/pa.h"

static bool s_active;
static tx_source_t s_source;

void tx_init(void)
{
    pa_init();
}

void tx_start(uint32_t freq_10hz, tx_source_t source)
{
    if (s_active && source == s_source)
        return;

    /* The band state first (PA0/PA1, the BK4815 0x75 band and the RX path),
     * then the TX band pin -- pa_tx_enable() overwrites 0x33. */
    pa_select_band(freq_10hz);
    pa_tx_enable();                         /* 0x33 = 0x42 VHF / 0x22 UHF, 0x36 = 0x8822 */
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

    pa_power(TX_POWER_COMPARE);
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
}

bool tx_active(void) { return s_active; }
tx_source_t tx_source(void) { return s_source; }
