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

    pa_tx_enable();                         /* 0x33 pin 1, 0x36 = 0x8822 */
    pa_band_path();
    bk4815_write_reg(0x0C, 0x0203u);        /* the T/R path, other branch */
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
    bk4815_write_reg(0x0C, 0x0A03u);
    BK4819_RX_TurnOn();
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_50, 0xBB18u);       /* muted again */
    BK4819_WriteRegister(BK4819_REG_40, 0x3516u);       /* the RX value */
    led_set(LED_OFF);

    s_active = false;
}

bool tx_active(void) { return s_active; }
tx_source_t tx_source(void) { return s_source; }
