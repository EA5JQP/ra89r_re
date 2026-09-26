#include "driver/rx.h"

#include "driver/audio_path.h"
#include "driver/bk4815.h"
#include "driver/bk4819.h"
#include "driver/bk4829.h"
#include "driver/pa.h"
#include "driver/tx.h"

static bool s_ready;
static bool s_squelch_open;
static uint16_t s_rssi;

void rx_init(uint32_t freq_10hz)
{
    /* The order is the one the bench was validated in, kept deliberately: the
     * K1 bring-up and receive first, then the second transceiver, the PA's PWM
     * and the audio path. */
    BK4819_SetAudioPathCallback(audio_path_drive);
    BK4819_Init();
    BK4819_SetFrequency(freq_10hz);
    /* The VHF/UHF front-end path, which this chain never used to set: without it
     * both bands ran with whatever the chip's GPIO register held.  See pa.h. */
    pa_select_band(freq_10hz);
    BK4819_SetAF(BK4819_AF_FM);
    BK4819_RX_TurnOn();

    bk4815_configure();
    bk4815_write_reg(0x0C, 0x0A03u);
    tx_init();
    audio_path_init();

    BK4819_SetAF(BK4819_AF_MUTE);       /* start quiet; rx_poll() opens it */

    s_ready = true;
    s_squelch_open = false;
}

void rx_set_frequency(uint32_t freq_10hz)
{
    BK4819_SetFrequency(freq_10hz);
    pa_select_band(freq_10hz);
}

bool rx_ready(void)
{
    return s_ready;
}

void rx_poll(void)
{
    if (!s_ready)
        return;

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

uint16_t rx_rssi(void)
{
    return s_rssi;
}
