/* The RA89R codeplug decoder -- see port_codeplug.h and ra89r_codeplug.md.
 *
 * Read-only: every function here reads the chip and never writes it.  The one
 * write path the port will need (a channel the user edited) is not implemented,
 * because the stock's journal is how that has to be done and the journal's
 * `valid` byte is still an open question (ra89r_eeprom.md).
 */
#include <string.h>

#include "dcs.h"
#include "driver/py25q16.h"
#include "frequencies.h"
#include "misc.h"
#include "port_codeplug.h"

/* BK4819_FILTER_BW_WIDE / _NARROW, and the OUTPUT_POWER_* ladder, without
 * pulling the RF driver into a file the PC preview also builds. */
#define CP_BANDWIDTH_WIDE    0u
#define CP_BANDWIDTH_NARROW  1u

/* Bits 12..15 of the tone words.  The CPS wraps the scrambler code into the
 * top nibble of the *receive* tone and two DCS-polarity bits into the top
 * nibble of the transmit tone (ChinfDetail.ChgChStringPro). */
#define CP_RX_TONE_SCRAMBLE_MASK 0xF000u
#define CP_TONE_VALUE_MASK       0x0FFFu
#define CP_TONE_OFF_THRESHOLD    2600u    /* above the CTCSS range: no tone */
#define CP_TONE_DCS_MAX          511u     /* at or below: a DCS code */

/* The channel bitmaps are 32 bytes each and are read on every attribute lookup,
 * so they live in RAM once they have been read.  `port_codeplug_init()` is what
 * makes that safe: it must run after the external-NOR driver is up, and it drops
 * anything a read before that might have cached (which, on a bus that is not
 * answering yet, is all 0xFF -- i.e. a radio with no channels at all). */
static uint8_t cp_enable[RA89R_CP_BITMAP_BYTES];
static uint8_t cp_skip[RA89R_CP_BITMAP_BYTES];
static bool    cp_bitmaps_valid;

/* The K1's band for a channel, stored as band + 1 with 0 meaning "not looked up
 * yet".  Deriving it needs the record's frequency, and the K1 asks for a
 * channel's attributes far more often than once -- its own cache holds ten at a
 * time while a scan walks all of them -- so the record is read once per channel
 * instead of once per lookup. */
static uint8_t cp_band[RA89R_CP_RECORD_COUNT];

static void cp_freq_defaults(void);

void port_codeplug_init(void)
{
    cp_bitmaps_valid = false;
    memset(cp_band, 0, sizeof cp_band);
    /* The frequency channels' store starts at the band's own lower bound: a
     * zeroed store decodes as 0 Hz, which RADIO_ConfigureChannel then clamps to
     * band 1 -- frequency mode opening on 18 MHz, which is what the first radio
     * run of the VFO/channel switch did.  A stored blob overwrites these. */
    cp_freq_defaults();
}

/* ---------------------------------------------------------------------------
 * Reading
 * ------------------------------------------------------------------------- */

bool port_codeplug_read(uint16_t channel, ra89r_codeplug_record_t *record)
{
    if (record == 0 || channel >= RA89R_CP_RECORD_COUNT)
        return false;

    PY25Q16_ReadBuffer(RA89R_CP_RECORD_BASE + (uint32_t)channel * RA89R_CP_RECORD_STRIDE,
                       record, sizeof *record);
    return true;
}

static void cp_load_bitmaps(void)
{
    if (cp_bitmaps_valid)
        return;

    PY25Q16_ReadBuffer(RA89R_CP_ENABLE_BASE, cp_enable, sizeof cp_enable);
    PY25Q16_ReadBuffer(RA89R_CP_SKIP_BASE, cp_skip, sizeof cp_skip);
    cp_bitmaps_valid = true;
}

static bool cp_bitmap_get(const uint8_t *bitmap, uint16_t channel)
{
    if (channel >= RA89R_CP_BITMAP_BYTES * 8u)
        return false;

    return (bitmap[channel >> 3] & (uint8_t)(1u << (channel & 7u))) != 0u;
}

bool port_codeplug_used(uint16_t channel)
{
    if (channel >= RA89R_CP_RECORD_COUNT)
        return false;

    cp_load_bitmaps();
    return cp_bitmap_get(cp_enable, channel);
}

bool port_codeplug_excluded(uint16_t channel)
{
    /* The CPS's "scan allow" bit is set for a channel that may be scanned, so
     * the K1's exclude flag is its inverse.  A channel outside the bitmap can
     * never be scanned. */
    if (channel >= RA89R_CP_RECORD_COUNT)
        return true;

    cp_load_bitmaps();
    return !cp_bitmap_get(cp_skip, channel);
}

/* ---------------------------------------------------------------------------
 * Decoding
 * ------------------------------------------------------------------------- */

/* Turn one of the stock's 12-bit tone words into the K1's (code type, index)
 * pair.  The stock stores a CTCSS tone as Hz/10 and a DCS code as the octal
 * number the CPS prints; the K1's tables hold exactly those numbers, so the
 * conversion is a table lookup and the two agree on every tone but 62.5 Hz,
 * which the K1 does not have. */
static void cp_decode_tone(uint16_t word, bool transmit, DCS_CodeType_t *type, uint8_t *code)
{
    const uint16_t value = word & CP_TONE_VALUE_MASK;
    unsigned int i;

    *type = CODE_TYPE_OFF;
    *code = 0;

    if (value > CP_TONE_OFF_THRESHOLD)
        return;                                   /* 0x0FFF and friends */

    if (value > CP_TONE_DCS_MAX) {
        for (i = 0; i < sizeof CTCSS_Options / sizeof CTCSS_Options[0]; i++) {
            if (CTCSS_Options[i] == value) {
                *type = CODE_TYPE_CONTINUOUS_TONE;
                *code = (uint8_t)i;
                return;
            }
        }
        return;                                   /* 62.5 Hz: not in the K1 table */
    }

    for (i = 0; i < sizeof DCS_Options / sizeof DCS_Options[0]; i++) {
        if (DCS_Options[i] == value) {
            const uint16_t invert_bit = transmit ? 0x8000u : 0x4000u;

            *type = (word & invert_bit) != 0u ? CODE_TYPE_REVERSE_DIGITAL : CODE_TYPE_DIGITAL;
            *code = (uint8_t)i;
            return;
        }
    }
}

/* The stock's three-level power setting onto the K1's ladder. */
static uint8_t cp_decode_power(uint8_t flags_a)
{
    switch ((flags_a & RA89R_CP_FLAGA_POWER_MASK) >> 6) {
        case 0:  return OUTPUT_POWER_HIGH;   /* "Hig" */
        case 1:  return OUTPUT_POWER_MID;    /* "Mid" */
        default: return OUTPUT_POWER_LOW1;   /* "Low" */
    }
}

static void cp_decode_record(const ra89r_codeplug_record_t *record, ChannelScanDisplayInfo_t *info)
{
    uint8_t step;

    memset(info, 0, sizeof *info);

    info->rx.Frequency = record->rx_frequency;
    info->tx.Frequency = (record->tx_frequency != 0u) ? record->tx_frequency : record->rx_frequency;

    cp_decode_tone(record->rx_tone, false, &info->rx.CodeType, &info->rx.Code);
    cp_decode_tone(record->tx_tone, true,  &info->tx.CodeType, &info->tx.Code);

    /* The stock stores both frequencies outright; the K1 carries receive plus a
     * signed offset. */
    if (info->tx.Frequency > info->rx.Frequency) {
        info->txOffsetFrequencyDirection = TX_OFFSET_FREQUENCY_DIRECTION_ADD;
        info->offset = info->tx.Frequency - info->rx.Frequency;
    } else if (info->tx.Frequency < info->rx.Frequency) {
        info->txOffsetFrequencyDirection = TX_OFFSET_FREQUENCY_DIRECTION_SUB;
        info->offset = info->rx.Frequency - info->tx.Frequency;
    } else {
        info->txOffsetFrequencyDirection = TX_OFFSET_FREQUENCY_DIRECTION_OFF;
        info->offset = 0;
    }

    info->modulation = MODULATION_FM;

    step = record->flags_c & RA89R_CP_FLAGC_STEP_MASK;
    if (step >= STEP_N_ELEM)
        step = STEP_12_5kHz;
    info->stepSetting   = (STEP_Setting_t)step;
    info->stepFrequency = gStepFrequencyTable[step];

    info->outputPower      = cp_decode_power(record->flags_a);
    info->channelBandwidth = ((record->flags_a & RA89R_CP_FLAGA_BW_MASK) == 0u)
                                 ? CP_BANDWIDTH_WIDE : CP_BANDWIDTH_NARROW;
    info->busyChannelLock  = (record->flags_a & RA89R_CP_FLAGA_BUSY_MASK) != 0u;

    /* The stock's per-channel "Tx inhibit" and "Freq. reverse" are exactly the
     * K1's TX_LOCK and FrequencyReverse.  Its "talk around" (flags B bit 1) has
     * no per-channel equivalent in this K1 build, so it is left out rather than
     * folded into FrequencyReverse, where it would be a different behaviour. */
    info->frequencyReverse = (record->flags_a & RA89R_CP_FLAGA_FREQ_REV) != 0u;
    info->txLock           = (record->flags_a & RA89R_CP_FLAGA_TX_INHIBIT) != 0u;

    info->dtmfPttIdTxMode = PTT_ID_OFF;
#ifdef ENABLE_DTMF_CALLING
    info->dtmfDecodingEnable = 0;
#endif
}

bool port_codeplug_scan_info(uint16_t channel, ChannelScanDisplayInfo_t *info)
{
    ra89r_codeplug_record_t record;

    if (info == 0 || channel >= RA89R_CP_RECORD_COUNT)
        return false;
    if (!port_codeplug_used(channel))
        return false;
    if (!port_codeplug_read(channel, &record))
        return false;
    if (record.rx_frequency == 0u || record.rx_frequency == 0xFFFFFFFFu)
        return false;

    cp_decode_record(&record, info);
    return true;
}

void port_codeplug_name(char *out, size_t size, uint16_t channel)
{
    ra89r_codeplug_record_t record;
    char extended[RA89R_CP_NAME_STRIDE];
    size_t used = 0;
    size_t i;

    if (out == 0 || size == 0u)
        return;

    out[0] = 0;

    if (channel >= RA89R_CP_RECORD_COUNT || !port_codeplug_used(channel) || size < 2u)
        return;

    /* The record's own six characters come first; the ten-byte table at 4416 is
     * the extension the CPS appends for names that need more room. */
    if (port_codeplug_read(channel, &record)) {
        for (i = 0; i < sizeof record.name && used + 1u < size; i++) {
            const char c = record.name[i];

            if (c == 0)
                break;
            if (c == ' ')
                continue;              /* the CPS pads with spaces */
            out[used++] = c;
        }
    }

    PY25Q16_ReadBuffer(RA89R_CP_NAME_BASE + (uint32_t)channel * RA89R_CP_NAME_STRIDE,
                       extended, sizeof extended);

    for (i = 0; i < sizeof extended && used + 1u < size; i++) {
        const unsigned char c = (unsigned char)extended[i];

        if (c == 0u || c == 0xFFu)
            break;
        if (c < 0x20u || c > 0x7Eu)
            break;                     /* GBK tail: not ASCII, stop cleanly */
        if (c == ' ')
            continue;
        out[used++] = (char)c;
    }

    out[used] = 0;
}

/* ---------------------------------------------------------------------------
 * The K1's channel attributes
 * ------------------------------------------------------------------------- */

uint16_t port_codeplug_attributes(uint16_t channel)
{
    uint16_t value;
    uint8_t  band;
    uint8_t  scanlist;
    uint8_t  exclude;

    if (IS_FREQ_CHANNEL(channel)) {
        /* The frequency channels always exist: their band is the channel index
         * itself and they are in every scan list. */
        band     = (uint8_t)(channel - FREQ_CHANNEL_FIRST);
        scanlist = MR_CHANNELS_LIST + 1;
        exclude  = 0;
    } else if (channel < RA89R_CP_RECORD_COUNT && port_codeplug_used(channel)) {
        if (cp_band[channel] == 0u) {
            ra89r_codeplug_record_t record;

            if (!port_codeplug_read(channel, &record) || record.rx_frequency == 0u ||
                record.rx_frequency == 0xFFFFFFFFu)
                return 0xFFFFu;

            cp_band[channel] = (uint8_t)(FREQUENCY_GetBand(record.rx_frequency) + 1);
        }
        band     = (uint8_t)(cp_band[channel] - 1u);
        scanlist = MR_CHANNELS_LIST + 1;   /* the stock has one set, not lists */
        exclude  = port_codeplug_excluded(channel) ? 1u : 0u;
    } else {
        return 0xFFFFu;                    /* not a channel this radio has */
    }

    value  = (uint16_t)(band & 0x07u);
    value |= (uint16_t)((exclude & 0x01u) << 7);
    value |= (uint16_t)((uint16_t)scanlist << 8);
    return value;
}

void port_codeplug_save_attributes(uint16_t channel, uint16_t value)
{
    /* The K1 writes its 2-byte attribute table at 0x8000; on this chip that is
     * the middle of the stock's channel records, so writing it would destroy
     * the codeplug.  The stock's own scan-allow bitmap is what this would have
     * to change, and that is a write into a stock region the port does not do
     * yet (ra89r_codeplug.md, "Writing"). */
    (void)channel;
    (void)value;
}

/* ---------------------------------------------------------------------------
 * The frequency (VFO) channels
 * ------------------------------------------------------------------------- */

/* One K1-shaped frequency-channel record per (band, VFO), held in RAM and
 * carried in the port's settings blob.  Same 16 bytes RADIO_ConfigureChannel
 * used to read from 0x009000 in the K1. */
#define CP_FREQ_BANDS  ((unsigned)BAND_N_ELEM)
#define CP_FREQ_VFOS   2u
#define CP_FREQ_SIZE   16u

static uint8_t cp_freq[CP_FREQ_BANDS][CP_FREQ_VFOS][CP_FREQ_SIZE];

static void cp_freq_pack(const ChannelScanDisplayInfo_t *info, uint8_t *out)
{
    memset(out, 0, CP_FREQ_SIZE);

    out[0] = (uint8_t)(info->rx.Frequency);
    out[1] = (uint8_t)(info->rx.Frequency >> 8);
    out[2] = (uint8_t)(info->rx.Frequency >> 16);
    out[3] = (uint8_t)(info->rx.Frequency >> 24);
    out[4] = (uint8_t)(info->offset);
    out[5] = (uint8_t)(info->offset >> 8);
    out[6] = (uint8_t)(info->offset >> 16);
    out[7] = (uint8_t)(info->offset >> 24);

    out[8]  = info->rx.Code;
    out[9]  = info->tx.Code;
    out[10] = (uint8_t)((info->rx.CodeType & 0x0Fu) |
                        ((info->tx.CodeType & 0x0Fu) << 4));
    out[11] = (uint8_t)((info->txOffsetFrequencyDirection & 0x0Fu) |
                        ((info->modulation & 0x0Fu) << 4));
    out[12] = (uint8_t)((info->frequencyReverse ? 1u : 0u) |
                        ((info->channelBandwidth ? 1u : 0u) << 1) |
                        ((info->outputPower & 0x07u) << 2) |
                        ((info->busyChannelLock ? 1u : 0u) << 5) |
                        ((info->txLock ? 1u : 0u) << 6));
    out[13] = 0;
    out[14] = (uint8_t)info->stepSetting;
    out[15] = 0;
}

static void cp_freq_unpack(const uint8_t *in, ChannelScanDisplayInfo_t *info)
{
    memset(info, 0, sizeof *info);

    info->rx.Frequency = (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
                         ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    info->tx.Frequency = info->rx.Frequency;
    info->offset = (uint32_t)in[4] | ((uint32_t)in[5] << 8) |
                   ((uint32_t)in[6] << 16) | ((uint32_t)in[7] << 24);

    info->rx.CodeType = (DCS_CodeType_t)(in[10] & 0x0Fu);
    info->tx.CodeType = (DCS_CodeType_t)((in[10] >> 4) & 0x0Fu);
    info->rx.Code = in[8];
    info->tx.Code = in[9];

    info->txOffsetFrequencyDirection = in[11] & 0x0Fu;
    info->modulation = (ModulationMode_t)((in[11] >> 4) & 0x0Fu);
    if (info->modulation >= MODULATION_UKNOWN)
        info->modulation = MODULATION_FM;

    info->frequencyReverse = (in[12] & 0x01u) != 0u;
    info->channelBandwidth = (in[12] >> 1) & 0x01u;
    info->outputPower      = (in[12] >> 2) & 0x07u;
    info->busyChannelLock  = (in[12] >> 5) & 0x01u;
    info->txLock           = (in[12] >> 6) & 0x01u;

    info->stepSetting = (STEP_Setting_t)in[14];
    if (info->stepSetting >= STEP_N_ELEM)
        info->stepSetting = STEP_12_5kHz;
    info->stepFrequency = gStepFrequencyTable[info->stepSetting];

    info->dtmfPttIdTxMode = PTT_ID_OFF;

    /* A frequency channel keeps its receive frequency and its offset
     * separately in the K1; the stock's VFO has them in the other direction. */
    switch (info->txOffsetFrequencyDirection) {
        case TX_OFFSET_FREQUENCY_DIRECTION_ADD:
            info->tx.Frequency = info->rx.Frequency + info->offset;
            break;
        case TX_OFFSET_FREQUENCY_DIRECTION_SUB:
            info->tx.Frequency = info->rx.Frequency - info->offset;
            break;
        default:
            info->tx.Frequency = info->rx.Frequency;
            info->offset = 0;
            break;
    }
}

static void cp_freq_defaults(void)
{
    unsigned band;
    unsigned vfo;

    for (band = 0; band < CP_FREQ_BANDS; band++) {
        for (vfo = 0; vfo < CP_FREQ_VFOS; vfo++) {
            ChannelScanDisplayInfo_t info;

            memset(&info, 0, sizeof info);
            info.rx.Frequency = frequencyBandTable[band].lower;
            info.tx.Frequency = info.rx.Frequency;
            info.modulation = MODULATION_FM;
            info.stepSetting = STEP_12_5kHz;
            info.stepFrequency = gStepFrequencyTable[STEP_12_5kHz];
            info.outputPower = OUTPUT_POWER_HIGH;
            info.txLock = 0;
            cp_freq_pack(&info, cp_freq[band][vfo]);
        }
    }
}

bool port_codeplug_freq_get(uint16_t channel, uint8_t vfo, ChannelScanDisplayInfo_t *info)
{
    const unsigned band = (unsigned)(channel - FREQ_CHANNEL_FIRST);

    if (info == 0 || band >= CP_FREQ_BANDS || vfo >= CP_FREQ_VFOS)
        return false;

    cp_freq_unpack(cp_freq[band][vfo], info);
    return true;
}

void port_codeplug_freq_set(uint16_t channel, uint8_t vfo, const ChannelScanDisplayInfo_t *info)
{
    const unsigned band = (unsigned)(channel - FREQ_CHANNEL_FIRST);

    if (info == 0 || band >= CP_FREQ_BANDS || vfo >= CP_FREQ_VFOS)
        return;

    cp_freq_pack(info, cp_freq[band][vfo]);
}

void port_codeplug_freq_snapshot(uint8_t *dest, size_t size)
{
    if (dest == 0 || size < sizeof cp_freq)
        return;

    memcpy(dest, cp_freq, sizeof cp_freq);
}

bool port_codeplug_freq_restore(const uint8_t *src, size_t size)
{
    if (src == 0 || size < sizeof cp_freq)
        return false;

    memcpy(cp_freq, src, sizeof cp_freq);
    return true;
}

/* ---------------------------------------------------------------------------
 * The stock's shared settings
 * ------------------------------------------------------------------------- */

void port_codeplug_shared_settings(void)
{
    /* Not mapped yet: ra89r_codeplug.md records what the block contains and
     * which of it the K1 has an equivalent for.  Until then the port's own
     * defaults and its blob decide, and the stock's block is left alone. */
}
