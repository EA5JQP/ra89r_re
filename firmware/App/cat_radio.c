/* FT-817 CAT actions: the radio side of the parsed frames.
 *
 * `cat.c` is the pure parser; this file is the glue that tunes, sets the
 * modulation, keys PTT and sets CTCSS on the **selected** VFO.  It is the only
 * place the CAT feature touches the radio.
 */
#include "cat.h"

#include "dcs.h"
#include "driver/rx.h"
#include "driver/tx.h"
#include "driver/uart.h"
#include "misc.h"
#include "settings.h"
#include "radio.h"

static void cat_ack(void)
{
    uart_putc(0x00);
}

/* Tune the selected TX VFO (the active one the K1 tunes) to f. */
static void cat_tune(uint32_t freq_10hz)
{
    if (freq_10hz < 1000000u || freq_10hz > 130000000u)   /* 1..1300 MHz sanity */
        return;

    gTxVfo->freq_config_RX.Frequency = freq_10hz;
    gTxVfo->freq_config_TX.Frequency = freq_10hz;
    if (gRxVfo != gTxVfo) {
        gRxVfo->freq_config_RX.Frequency = freq_10hz;
        gRxVfo->freq_config_TX.Frequency = freq_10hz;
    }

    RADIO_ApplyOffset(gTxVfo);
    RADIO_ConfigureSquelchAndOutputPower(gTxVfo);
    RADIO_SetupRegisters(true);

    gUpdateDisplay = true;
    gUpdateStatus  = true;
}

static cat_mod_t cat_current_mod(void)
{
    return (gRxVfo->Modulation == MODULATION_AM) ? CAT_MOD_AM : CAT_MOD_FM;
}

/* READ returns 4 BCD frequency bytes then the mode byte. */
static void cat_reply_read(void)
{
    uint8_t bcd[4];
    uint8_t out[CAT_FRAME_LEN];

    cat_freq_10hz_to_bcd(gRxVfo->freq_config_RX.Frequency, bcd);
    out[0] = bcd[0];
    out[1] = bcd[1];
    out[2] = bcd[2];
    out[3] = bcd[3];
    out[4] = cat_modulation_to_mode(cat_current_mod());

    uart_write_raw((const char *)out, CAT_FRAME_LEN);
}

/* The CTCSS tone is a 0.1 Hz value in the same units as CTCSS_Options. */
static int8_t cat_ctcss_index(uint16_t tone_01hz)
{
    uint8_t i, best = 0;
    int best_delta = 0x7FFF;

    for (i = 0; i < ARRAY_SIZE(CTCSS_Options); i++) {
        int d = (int)tone_01hz - (int)CTCSS_Options[i];
        if (d < 0)
            d = -d;
        if (d < best_delta) {
            best_delta = d;
            best = i;
        }
    }
    return (int8_t)best;
}

static void cat_set_ctcss(bool on, uint16_t tone_01hz)
{
    FREQ_Config_t *cfg = &gTxVfo->freq_config_TX;

    if (on) {
        cfg->CodeType = CODE_TYPE_CONTINUOUS_TONE;
        cfg->Code     = (uint8_t)cat_ctcss_index(tone_01hz);
    } else {
        cfg->CodeType = CODE_TYPE_OFF;
        cfg->Code     = 0;
    }

    RADIO_SetupRegisters(true);
    gUpdateStatus = true;
}

void cat_apply(const uint8_t frame[CAT_FRAME_LEN])
{
    switch (cat_frame_cmd(frame)) {
    case CAT_CMD_SET_FREQ:
        cat_tune(cat_bcd_to_freq_10hz(frame));
        cat_ack();
        break;

    case CAT_CMD_READ:
        cat_reply_read();
        break;

    case CAT_CMD_SET_MODE:
        gTxVfo->Modulation = (cat_mode_to_modulation(frame[0]) == CAT_MOD_AM)
                                 ? MODULATION_AM : MODULATION_FM;
        RADIO_SetModulation(gTxVfo->Modulation);
        gUpdateDisplay = true;
        cat_ack();
        break;

    case CAT_CMD_PTT_ON:
        /* The remote PTT: the radio's TX rules apply (band plan, TX lock,
         * the airband ban), the PTT poller leaves it keyed, 30 s cap. */
        (void)tx_remote_ptt(true);
        cat_ack();
        break;

    case CAT_CMD_PTT_OFF:
        (void)tx_remote_ptt(false);
        cat_ack();
        break;

    case CAT_CMD_CTCSS_MODE:
        cat_set_ctcss(frame[0] == 0x2Au, CTCSS_Options[gTxVfo->freq_config_TX.Code]);
        cat_ack();
        break;

    case CAT_CMD_CTCSS_TONE: {
        /* frame[0..1] = BCD tone in 0.1 Hz. */
        uint16_t tone = (uint16_t)(((frame[0] >> 4) & 0x0F) * 1000u +
                                   (frame[0] & 0x0F) * 100u +
                                   ((frame[1] >> 4) & 0x0F) * 10u +
                                   (frame[1] & 0x0F));
        cat_set_ctcss(true, tone);
        cat_ack();
        break;
    }

    case CAT_CMD_SM: {
        /* FT-817 S-meter read: 4 payload bytes, the meter as a 0-255 value in
         * the last byte (the first three are zero on a real FT-817).  The
         * mapping is the lane's raw RSSI clamped to 255: the BK4829 reports a
         * 9-bit value in 0x67, the BK4815 a 7-bit one in 0x44. */
        uint8_t out[CAT_FRAME_LEN];

        out[0] = 0u;
        out[1] = 0u;
        out[2] = 0u;
        out[3] = (uint8_t)((rx_rssi() > 255u) ? 255u : rx_rssi());
        out[4] = CAT_CMD_SM;
        uart_write_raw((const char *)out, CAT_FRAME_LEN);
        break;
    }

    case CAT_CMD_STATUS: {
        /* The port's own receive state (see cat.h). */
        uint8_t out[CAT_FRAME_LEN];
        const uint8_t status = (uint8_t)((rx_squelch_open() ? 0x01u : 0u) |
                                         (rx_audio_is_4815() ? 0x02u : 0u) |
                                         (uint8_t)(gSetting_set_scn << 2));

        out[0] = status;
        out[1] = (uint8_t)(rx_rssi() & 0xFFu);
        out[2] = (uint8_t)(rx_rssi() >> 8);
        out[3] = gEeprom.SQUELCH_LEVEL;
        out[4] = CAT_CMD_STATUS;
        uart_write_raw((const char *)out, CAT_FRAME_LEN);
        break;
    }

    case CAT_CMD_VS:
        /* FT-817 VFO select: payload[0] 0 = A, 1 = B. */
        if (frame[0] <= 1u) {
            gEeprom.RX_VFO = frame[0];
            gEeprom.TX_VFO = frame[0];
            RADIO_ConfigureChannel(frame[0], VFO_CONFIGURE_RELOAD);
            gUpdateDisplay = true;
            gUpdateStatus  = true;
        }
        cat_ack();
        break;

    default:
        break;
    }
}
