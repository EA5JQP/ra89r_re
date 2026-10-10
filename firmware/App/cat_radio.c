/* FT-817 CAT actions: the radio side of the parsed frames.
 *
 * `cat.c` is the pure parser; this file is the glue that tunes, sets the
 * modulation, keys PTT and sets CTCSS on the **selected** VFO.  It is the only
 * place the CAT feature touches the radio.
 */
#include "cat.h"

#include "dcs.h"
#include "driver/tx.h"
#include "driver/uart.h"
#include "misc.h"
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
        tx_start(gTxVfo->freq_config_TX.Frequency, gTxVfo->TXP_CalculatedSetting,
                 TX_SOURCE_MIC);
        cat_ack();
        break;

    case CAT_CMD_PTT_OFF:
        tx_stop();
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

    default:
        break;
    }
}
