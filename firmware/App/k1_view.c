/* The K1 virtual EEPROM CHIRP's F4HWN driver reads and writes.  See
 * App/k1_view.h and docs/ra89r_chirp.md.
 *
 * k1_settings_pack()/k1_settings_unpack() are the F4HWN v6.2.0
 * SETTINGS_SaveSettings()/SETTINGS_InitEEPROM() byte layout (Copyright 2023
 * Dual Tachyon, F4HWN; Apache-2.0), rewritten to fill/parse a RAM image
 * instead of the K1's flash, because this port keeps those values in its
 * settings blob.  Bytes for a feature the port does not build come out as the
 * K1's own neutral value, never as a stray 0xFF the driver would index with. */
#include "k1_view.h"

#include <string.h>

#include "app/dtmf.h"
#include "driver/py25q16.h"
#include "frequencies.h"
#include "misc.h"
#include "settings.h"
#ifdef ENABLE_FMRADIO
#include "app/fm.h"
#include "driver/bk1080.h"
#endif

/* ---------------------------------------------------------------------------
 * Settings: 0xA000-0xA177 <-> gEeprom and the gSetting_* globals
 * ------------------------------------------------------------------------- */

#define O(addr) ((addr) - K1VIEW_SETTINGS_BASE)

/* The K1's version field, 0xA160.  The port has its own version; the driver
 * only shows it. */
#define K1VIEW_VERSION_FIELD VERSION_STRING_2

static void wr16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint8_t lim(uint8_t v, uint8_t n, uint8_t def)
{
    return v < n ? v : def;
}

/* A DTMF code as the K1 holds it: the K1's boot replaces an empty/invalid one
 * with its default (SETTINGS_LoadEepromDtmf), and the driver refuses an empty
 * code, while this port leaves the fields zeroed. */
static void pack_code(uint8_t *dst, const char *src, size_t size, const char *def)
{
    char buf[16];

    if (size > sizeof buf)
        size = sizeof buf;
    memcpy(buf, src, size);
    memset(dst, 0, size);
    if (DTMF_ValidateCodes(buf, (unsigned)size))
        memcpy(dst, buf, size);
    else
        memcpy(dst, def, strlen(def));
}

void k1_settings_pack(uint8_t out[K1VIEW_SETTINGS_SIZE])
{
    uint8_t *s;

    memset(out, 0xFF, K1VIEW_SETTINGS_SIZE);

    /* 0xA000 */
    s = &out[O(0xA000)];
#ifdef ENABLE_FEAT_F4HWN_AUDIO
    s[0] = (uint8_t)((gSetting_set_audio_fm & 0x0Fu) | ((gSetting_set_audio_am & 0x0Fu) << 4));
#else
    s[0] = 0;
#endif
#ifdef ENABLE_FEAT_F4HWN
    s[1] = (gSquelchLevelOriginal < 10u) ? gSquelchLevelOriginal : gEeprom.SQUELCH_LEVEL;
#else
    s[1] = gEeprom.SQUELCH_LEVEL;
#endif
    s[2] = gEeprom.TX_TIMEOUT_TIMER;
    s[3] = 0;                                   /* NOAA auto scan: no NOAA */
    s[4] = (uint8_t)((gEeprom.KEY_LOCK ? 0x01u : 0u) |
#ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                     (gEeprom.MENU_LOCK ? 0x02u : 0u) |
                     ((gEeprom.SET_KEY & 0x0Fu) << 2) |
#endif
                     (gEeprom.SET_NAV ? 0x40u : 0u));
    s[5] = gEeprom.VOX_SWITCH;
    s[6] = gEeprom.VOX_LEVEL;
    s[7] = gEeprom.MIC_SENSITIVITY;

    /* 0xA008 */
    s = &out[O(0xA008)];
    s[0] = (uint8_t)((gEeprom.BACKLIGHT_MIN << 4) + gEeprom.BACKLIGHT_MAX);
    s[1] = gEeprom.CHANNEL_DISPLAY_MODE;
    s[2] = gEeprom.CROSS_BAND_RX_TX;
    s[3] = gEeprom.BATTERY_SAVE;
    s[4] = gEeprom.DUAL_WATCH;
#ifdef ENABLE_FEAT_F4HWN
    if (!gSaveRxMode) {
        s[2] = gCB;
        s[4] = gDW;
    }
    s[5] = gBackLight ? gBacklightTimeOriginal : gEeprom.BACKLIGHT_TIME;
#else
    s[5] = gEeprom.BACKLIGHT_TIME;
#endif
#ifdef ENABLE_FEAT_F4HWN_NARROWER
    s[6] = (uint8_t)((gEeprom.TAIL_TONE_ELIMINATION & 0x01u) | ((gSetting_set_nfm & 0x01u) << 1));
#else
    s[6] = gEeprom.TAIL_TONE_ELIMINATION ? 1u : 0u;
#endif
    s[7] = gEeprom.VFO_OPEN;

    /* 0xA010: the VFO indices (NOAA stays 0xFFFF: no NOAA here) */
    s = &out[O(0xA010)];
    wr16le(&s[0],  gEeprom.ScreenChannel[0]);
    wr16le(&s[2],  gEeprom.MrChannel[0]);
    wr16le(&s[4],  gEeprom.FreqChannel[0]);
    wr16le(&s[6],  gEeprom.ScreenChannel[1]);
    wr16le(&s[8],  gEeprom.MrChannel[1]);
    wr16le(&s[10], gEeprom.FreqChannel[1]);

#ifdef ENABLE_FMRADIO
    /* 0xA020: FM config {selFreq u16, selChn u8, isMrMode:1 band:2} */
    s = &out[O(0xA020)];
    wr16le(&s[0], gEeprom.FM_SelectedFrequency);
    s[2] = gEeprom.FM_SelectedChannel;
    s[3] = (uint8_t)(0xF8u | (gEeprom.FM_IsMrMode ? 1u : 0u) | ((gEeprom.FM_Band & 3u) << 1));
    /* 0xA028: the 48 FM memories */
    memcpy(&out[O(0xA028)], gFM_Channels, sizeof gFM_Channels);
#endif

    /* 0xA0A8 */
    s = &out[O(0xA0A8)];
    s[0] = (uint8_t)((gEeprom.BEEP_CONTROL ? 1u : 0u) | (gEeprom.KEY_M_LONG_PRESS_ACTION << 1));
    s[1] = gEeprom.KEY_1_SHORT_PRESS_ACTION;
    s[2] = gEeprom.KEY_1_LONG_PRESS_ACTION;
    s[3] = gEeprom.KEY_2_SHORT_PRESS_ACTION;
    s[4] = gEeprom.KEY_2_LONG_PRESS_ACTION;
    s[5] = gEeprom.SCAN_RESUME_MODE;
    s[6] = gEeprom.AUTO_KEYPAD_LOCK;
    s[7] = gEeprom.POWER_ON_DISPLAY_MODE;
    /* 0xA0B0: power-on password, not built -> 0xFF */

    /* 0xA0B8: voice prompt (off: no voice here), dBm correction */
    s = &out[O(0xA0B8)];
    s[0] = 0;
#ifdef ENABLE_FEAT_F4HWN
    memcpy(&s[1], dBmCorrTable, 7);
#else
    memset(&s[1], 0, 7);
#endif

    /* 0xA0C0 */
    s = &out[O(0xA0C0)];
    s[0] = 0;
    s[1] = gEeprom.ROGER;
    s[2] = gEeprom.REPEATER_TAIL_TONE_ELIMINATION;
    s[3] = gEeprom.TX_VFO;
    s[4] = gEeprom.BATTERY_TYPE;

    /* 0xA0C8-0xA0E7: boot message -- physical pass-through (k1_view_read) */

    /* 0xA0E8: DTMF */
    s = &out[O(0xA0E8)];
    s[0] = gEeprom.DTMF_SIDE_TONE;
#ifdef ENABLE_DTMF_CALLING
    s[1] = (uint8_t)gEeprom.DTMF_SEPARATE_CODE;
    s[2] = (uint8_t)gEeprom.DTMF_GROUP_CALL_CODE;
    s[3] = gEeprom.DTMF_DECODE_RESPONSE;
    s[4] = gEeprom.DTMF_auto_reset_time;
#else
    /* DTMF calling is not built: the K1's defaults, so the driver has sane
     * values to show (uploading them changes nothing here). */
    s[1] = '*';
    s[2] = '#';
    s[3] = 0;
    s[4] = 10;
#endif
    s[5] = (uint8_t)(gEeprom.DTMF_PRELOAD_TIME / 10u);
    s[6] = (uint8_t)(gEeprom.DTMF_FIRST_CODE_PERSIST_TIME / 10u);
    s[7] = (uint8_t)(gEeprom.DTMF_HASH_CODE_PERSIST_TIME / 10u);
    s[8] = (uint8_t)(gEeprom.DTMF_CODE_PERSIST_TIME / 10u);
    s[9] = (uint8_t)(gEeprom.DTMF_CODE_INTERVAL_TIME / 10u);
    /* 0xA0F8.. the codes (each field's own size, the K1's addresses) */
#ifdef ENABLE_DTMF_CALLING
    s[10] = gEeprom.PERMIT_REMOTE_KILL;
    memcpy(&out[O(0xA0F8)], gEeprom.ANI_DTMF_ID, sizeof gEeprom.ANI_DTMF_ID);
    memcpy(&out[O(0xA100)], gEeprom.KILL_CODE, sizeof gEeprom.KILL_CODE);
    memcpy(&out[O(0xA108)], gEeprom.REVIVE_CODE, sizeof gEeprom.REVIVE_CODE);
#else
    s[10] = 0;
    memset(&out[O(0xA0F8)], 0, 0x18);
    memcpy(&out[O(0xA0F8)], "123", 3);
    memcpy(&out[O(0xA100)], "ABCD9", 5);
    memcpy(&out[O(0xA108)], "9DCBA", 5);
#endif
    pack_code(&out[O(0xA110)], gEeprom.DTMF_UP_CODE, sizeof gEeprom.DTMF_UP_CODE, "12345");
    pack_code(&out[O(0xA120)], gEeprom.DTMF_DOWN_CODE, sizeof gEeprom.DTMF_DOWN_CODE, "54321");

    /* 0xA130: scan lists */
    s = &out[O(0xA130)];
    s[0] = (uint8_t)((gEeprom.SCAN_LIST_DEFAULT & 0x7Fu) | ((gEeprom.SCAN_LIST_ENABLED & 0x01u) << 7));
    wr16le(&s[1], gEeprom.SCANLIST_PRIORITY_CH[0]);
    wr16le(&s[3], gEeprom.SCANLIST_PRIORITY_CH[1]);
    wr16le(&s[5], gEeprom.CHAN_1_CALL);

    /* 0xA138: AES key, not used by F4HWN -> 0xFF.
     * 0xA148-0xA14F: spectrum -- physical pass-through. */

    /* 0xA150 */
    s = &out[O(0xA150)];
    s[0] = gSetting_F_LOCK;
    s[1] = 0;
#ifdef ENABLE_DTMF_CALLING
    s[2] = gSetting_KILLED;
#else
    s[2] = 0;
#endif
    s[3] = 0;
    s[4] = 0;
    s[5] = gSetting_350EN;
    s[6] = 0;
    if (!gSetting_live_DTMF_decoder)
        s[7] &= (uint8_t)~(1u << 1);
    s[7] = (uint8_t)((s[7] & ~(3u << 2)) | ((gSetting_battery_text & 3u) << 2));
#ifdef ENABLE_AUDIO_BAR
    if (!gSetting_mic_bar)
        s[7] &= (uint8_t)~(1u << 4);
#endif
    s[7] = (uint8_t)((s[7] & ~(3u << 6)) | (((uint8_t)gSetting_backlight_on_tx_rx & 3u) << 6));

#ifdef ENABLE_FEAT_F4HWN
    /* 0xA158 */
    s = &out[O(0xA158)];
    s[2] = gSetting_set_lck;
#ifdef ENABLE_FEAT_F4HWN_SLEEP
    s[4] = (uint8_t)((gSetting_set_off << 1) | (gSetting_set_tmr & 0x01u));
#else
    s[4] = gSetting_set_tmr ? 1u : 0u;
#endif
    s[5] = (uint8_t)((((gSetting_set_inv ? 1u : 0u) |
                       ((gSetting_set_met ? 1u : 0u) << 2) |
                       ((gSetting_set_gui ? 1u : 0u) << 3)) << 4) |
                     (gSetting_set_ctr & 0x0Fu));
    s[6] = (uint8_t)((gSetting_set_tot << 4) | (gSetting_set_eot & 0x0Fu));
    {
        uint8_t pss = gSetting_set_ptt ? 1u : 0u;
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
        /* The K1 stores SetScn inverted: bit 1 set = NORMAL. */
        if (!gSetting_set_scn)
            pss |= 0x02u;
#endif
#ifdef ENABLE_FEAT_F4HWN_LOGO_SAV
        pss |= (uint8_t)((gSetting_set_sav & 0x03u) << 2);
#endif
        s[7] = (uint8_t)((gSetting_set_pwr << 4) | pss);
    }
#endif

    /* 0xA160: version */
    memset(&out[O(0xA160)], 0, 16);
    strncpy((char *)&out[O(0xA160)], K1VIEW_VERSION_FIELD, 16);

    /* 0xA170: mixed scan lists.  The port has no MIX mode: every list. */
    s = &out[O(0xA170)];
    s[0] = 0xFF;
    s[1] = 0xFF;
    s[2] = 0xFF;
    s[3] = 'M';
    s[4] = 'I';
    s[5] = 'X';
    s[6] = 1;
}

/* The globals half of the unpack: only gSetting_* and the like, never gEeprom.
 * Shared by k1_settings_unpack() and the blob's v6 record. */
static void unpack_globals(const uint8_t *img)
{
    const uint8_t *s;

#ifdef ENABLE_FEAT_F4HWN_AUDIO
    s = &img[O(0xA000)];
    gSetting_set_audio_fm = ((s[0] & 0x0Fu) < 5u) ? (s[0] & 0x0Fu) : 0u;
    gSetting_set_audio_am = (((s[0] >> 4) & 0x0Fu) < 3u) ? ((s[0] >> 4) & 0x0Fu) : 0u;
#endif
#ifdef ENABLE_FEAT_F4HWN_NARROWER
    gSetting_set_nfm = (img[O(0xA00E)] >> 1) & 0x01u;
#endif
#ifdef ENABLE_FEAT_F4HWN
    {
        unsigned i;

        for (i = 0; i < 7u; i++) {
            const int8_t v = (int8_t)img[O(0xA0B9) + i];

            if (v >= -64 && v <= 64)
                dBmCorrTable[i] = v;
        }
    }
#endif

    s = &img[O(0xA150)];
    gSetting_F_LOCK = lim(s[0], F_LOCK_LEN, F_LOCK_DEF);
#ifdef ENABLE_DTMF_CALLING
    gSetting_KILLED = (s[2] < 2u) ? s[2] : false;
#endif
    gSetting_350EN  = (s[5] < 2u) ? s[5] : true;
    gSetting_live_DTMF_decoder = (s[7] & (1u << 1)) != 0u;
    gSetting_battery_text = (((s[7] >> 2) & 3u) <= 2u) ? ((s[7] >> 2) & 3u) : 2u;
#ifdef ENABLE_AUDIO_BAR
    gSetting_mic_bar = (s[7] & (1u << 4)) != 0u;
#endif
    gSetting_backlight_on_tx_rx = (enum BacklightOnRxTx_t)((s[7] >> 6) & 3u);

#ifdef ENABLE_FEAT_F4HWN
    s = &img[O(0xA158)];
    {
        const uint8_t pss    = s[7] & 0x0Fu;
        const bool    erased = s[7] == 0xFFu;
#ifdef ENABLE_FEAT_F4HWN_LOGO_SAV
        const bool    valid  = !erased && pss < (SET_SAV_LEN << 2);
#else
        const bool    valid  = !erased && pss < 4u;
#endif
        const uint8_t tmp    = (uint8_t)((s[5] & 0xF0u) >> 4);

        gSetting_set_pwr = (((s[7] & 0xF0u) >> 4) < 7u) ? ((s[7] & 0xF0u) >> 4) : 0u;
        gSetting_set_ptt = valid ? (pss & 0x01u) : 0u;
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
        gSetting_set_scn = valid ? ((pss & 0x02u) == 0u) : 0u;
#endif
#ifdef ENABLE_FEAT_F4HWN_LOGO_SAV
        gSetting_set_sav = valid ? ((pss >> 2) & 0x03u) : SET_SAV_OFF;
#endif
        gSetting_set_tot = (((s[6] & 0xF0u) >> 4) < 4u) ? ((s[6] & 0xF0u) >> 4) : 0u;
        gSetting_set_eot = ((s[6] & 0x0Fu) < 4u) ? (s[6] & 0x0Fu) : 0u;
#ifdef ENABLE_FEAT_F4HWN_INV
        gSetting_set_inv = (tmp >> 0) & 0x01u;
#else
        gSetting_set_inv = 0;
#endif
        gSetting_set_lck = lim(s[2], SET_LCK_LEN, SET_LCK_KEYS);
        gSetting_set_met = (tmp >> 2) & 0x01u;
        gSetting_set_gui = (tmp >> 3) & 0x01u;
#ifdef ENABLE_FEAT_F4HWN_CTR
        gSetting_set_ctr = ((s[5] & 0x0Fu) > 0u && (s[5] & 0x0Fu) < 16u) ? (s[5] & 0x0Fu) : 10u;
#endif
        gSetting_set_tmr = s[4] & 0x01u;
#ifdef ENABLE_FEAT_F4HWN_SLEEP
        gSetting_set_off = ((s[4] >> 1) > 120u) ? 60u : (uint8_t)(s[4] >> 1);
#endif
        gSetting_set_ptt_session = gSetting_set_ptt;
    }
#endif
}

static void load_dtmf(const uint8_t *src, char *dest, size_t size)
{
    char buf[16];

    if (size > sizeof buf)
        size = sizeof buf;
    memcpy(buf, src, size);
    if (DTMF_ValidateCodes(buf, (unsigned)size))
        memcpy(dest, buf, size);
}

void k1_settings_unpack(const uint8_t in[K1VIEW_SETTINGS_SIZE])
{
    const uint8_t *s;

    /* 0xA000 */
    s = &in[O(0xA000)];
    gEeprom.SQUELCH_LEVEL    = lim(s[1], 10u, 1u);
#ifdef ENABLE_FEAT_F4HWN
    gSquelchLevelOriginal    = 10u;         /* no temporary squelch override */
#endif
    /* The K1 refuses 0..4 (its menu starts at 30 s); this port's default is 3
     * (20 s), so accept what the port can hold or a round trip would change it. */
    gEeprom.TX_TIMEOUT_TIMER = (s[2] < 180u) ? s[2] : 11u;
    gEeprom.KEY_LOCK  = (s[4] & 0x01u) != 0u;
#ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
    gEeprom.MENU_LOCK = (s[4] & 0x02u) != 0u;
    gEeprom.SET_KEY   = (((s[4] >> 2) & 0x0Fu) > 4u) ? 0u : ((s[4] >> 2) & 0x0Fu);
#endif
    gEeprom.SET_NAV   = (s[4] & 0x40u) != 0u;
    gEeprom.VOX_SWITCH = lim(s[5], 2u, false);
    gEeprom.VOX_LEVEL  = lim(s[6], 10u, 1u);
    gEeprom.MIC_SENSITIVITY = lim(s[7], 9u, 4u);

    /* 0xA008 */
    s = &in[O(0xA008)];
    gEeprom.BACKLIGHT_MAX = ((s[0] & 0x0Fu) <= 10u) ? (s[0] & 0x0Fu) : 10u;
    gEeprom.BACKLIGHT_MIN = ((s[0] >> 4) < gEeprom.BACKLIGHT_MAX) ? (s[0] >> 4) : 0u;
    gEeprom.CHANNEL_DISPLAY_MODE = lim(s[1], 4u, MDF_FREQUENCY);
    gEeprom.CROSS_BAND_RX_TX     = lim(s[2], 3u, CROSS_BAND_OFF);
    gEeprom.BATTERY_SAVE         = lim(s[3], 6u, 4u);
    gEeprom.DUAL_WATCH           = lim(s[4], 3u, DUAL_WATCH_CHAN_A);
    gEeprom.BACKLIGHT_TIME       = lim(s[5], 62u, 12u);
#ifdef ENABLE_FEAT_F4HWN_NARROWER
    gEeprom.TAIL_TONE_ELIMINATION = s[6] & 0x01u;
#else
    gEeprom.TAIL_TONE_ELIMINATION = (s[6] & 0x01u) != 0u;
#endif
    gEeprom.VFO_OPEN = lim(s[7], 2u, true);

    /* 0xA010 */
    s = &in[O(0xA010)];
    {
        const uint16_t def = (uint16_t)(FREQ_CHANNEL_FIRST + BAND6_400MHz);
        uint16_t v;

        v = rd16le(&s[0]);  gEeprom.ScreenChannel[0] = IS_VALID_CHANNEL(v) ? v : def;
        v = rd16le(&s[2]);  gEeprom.MrChannel[0]     = IS_MR_CHANNEL(v) ? v : MR_CHANNEL_FIRST;
        v = rd16le(&s[4]);  gEeprom.FreqChannel[0]   = IS_FREQ_CHANNEL(v) ? v : def;
        v = rd16le(&s[6]);  gEeprom.ScreenChannel[1] = IS_VALID_CHANNEL(v) ? v : def;
        v = rd16le(&s[8]);  gEeprom.MrChannel[1]     = IS_MR_CHANNEL(v) ? v : MR_CHANNEL_FIRST;
        v = rd16le(&s[10]); gEeprom.FreqChannel[1]   = IS_FREQ_CHANNEL(v) ? v : def;
    }

#ifdef ENABLE_FMRADIO
    s = &in[O(0xA020)];
    {
        const uint16_t f    = rd16le(&s[0]);
        const uint8_t  band = (uint8_t)((s[3] >> 1) & 3u);
        const uint16_t lo   = BK1080_GetFreqLoLimit(band);

        gEeprom.FM_Band              = band;
        gEeprom.FM_SelectedFrequency = (f >= lo && f <= BK1080_GetFreqHiLimit(band)) ? f : lo;
        gEeprom.FM_SelectedChannel   = s[2];
        gEeprom.FM_IsMrMode          = (s[3] & 1u) != 0u;
    }
    memcpy(gFM_Channels, &in[O(0xA028)], sizeof gFM_Channels);
#endif

    /* 0xA0A8 */
    s = &in[O(0xA0A8)];
    gEeprom.BEEP_CONTROL             = s[0] & 1u;
    gEeprom.KEY_M_LONG_PRESS_ACTION  = lim((uint8_t)(s[0] >> 1), ACTION_OPT_LEN, ACTION_OPT_NONE);
    gEeprom.KEY_1_SHORT_PRESS_ACTION = lim(s[1], ACTION_OPT_LEN, ACTION_OPT_MONITOR);
    gEeprom.KEY_1_LONG_PRESS_ACTION  = lim(s[2], ACTION_OPT_LEN, ACTION_OPT_NONE);
    gEeprom.KEY_2_SHORT_PRESS_ACTION = lim(s[3], ACTION_OPT_LEN, ACTION_OPT_SCAN);
    gEeprom.KEY_2_LONG_PRESS_ACTION  = lim(s[4], ACTION_OPT_LEN, ACTION_OPT_NONE);
    gEeprom.SCAN_RESUME_MODE         = lim(s[5], 105u, 14u);
    gEeprom.AUTO_KEYPAD_LOCK         = lim(s[6], 41u, 0u);
    gEeprom.POWER_ON_DISPLAY_MODE    = lim(s[7], 6u, POWER_ON_DISPLAY_MODE_VOLTAGE);

    /* 0xA0C0 */
    s = &in[O(0xA0C0)];
    gEeprom.ROGER                          = lim(s[1], 3u, ROGER_MODE_OFF);
    gEeprom.REPEATER_TAIL_TONE_ELIMINATION = lim(s[2], 11u, 0u);
    gEeprom.TX_VFO                         = lim(s[3], 2u, 0u);
    gEeprom.BATTERY_TYPE                   = lim(s[4], BATTERY_TYPE_UNKNOWN, BATTERY_TYPE_1600_MAH);

    /* 0xA0E8: DTMF */
    s = &in[O(0xA0E8)];
    gEeprom.DTMF_SIDE_TONE = lim(s[0], 2u, true);
#ifdef ENABLE_DTMF_CALLING
    {
        char c;

        c = (char)s[1];
        if (DTMF_ValidateCodes(&c, 1u))
            gEeprom.DTMF_SEPARATE_CODE = (char)s[1];
        c = (char)s[2];
        if (DTMF_ValidateCodes(&c, 1u))
            gEeprom.DTMF_GROUP_CALL_CODE = (char)s[2];
    }
    gEeprom.DTMF_DECODE_RESPONSE         = lim(s[3], 4u, 0u);
    gEeprom.DTMF_auto_reset_time         = (s[4] < 61u) ? s[4] : ((s[4] >= 5u) ? s[4] : 10u);
#endif
    gEeprom.DTMF_PRELOAD_TIME            = (s[5] < 101u) ? (uint16_t)(s[5] * 10u) : 300u;
    gEeprom.DTMF_FIRST_CODE_PERSIST_TIME = (s[6] < 101u) ? (uint16_t)(s[6] * 10u) : 100u;
    gEeprom.DTMF_HASH_CODE_PERSIST_TIME  = (s[7] < 101u) ? (uint16_t)(s[7] * 10u) : 100u;
    gEeprom.DTMF_CODE_PERSIST_TIME       = (s[8] < 101u) ? (uint16_t)(s[8] * 10u) : 100u;
    gEeprom.DTMF_CODE_INTERVAL_TIME      = (s[9] < 101u) ? (uint16_t)(s[9] * 10u) : 100u;
#ifdef ENABLE_DTMF_CALLING
    gEeprom.PERMIT_REMOTE_KILL           = lim(s[10], 2u, true);
    load_dtmf(&in[O(0xA0F8)], gEeprom.ANI_DTMF_ID, sizeof gEeprom.ANI_DTMF_ID);
    load_dtmf(&in[O(0xA100)], gEeprom.KILL_CODE, sizeof gEeprom.KILL_CODE);
    load_dtmf(&in[O(0xA108)], gEeprom.REVIVE_CODE, sizeof gEeprom.REVIVE_CODE);
#endif
    load_dtmf(&in[O(0xA110)], gEeprom.DTMF_UP_CODE, sizeof gEeprom.DTMF_UP_CODE);
    load_dtmf(&in[O(0xA120)], gEeprom.DTMF_DOWN_CODE, sizeof gEeprom.DTMF_DOWN_CODE);

    /* 0xA130: scan lists.  The K1's MIX (lists + 2) is not built here: it loads
     * as "all" (lists + 1), the port's default. */
    s = &in[O(0xA130)];
    {
        const uint8_t def = s[0] & 0x7Fu;

        gEeprom.SCAN_LIST_DEFAULT = (def >= 1u && def <= MR_CHANNELS_LIST + 1u) ? def
                                                                                : (uint8_t)(MR_CHANNELS_LIST + 1u);
        gEeprom.SCAN_LIST_ENABLED = (s[0] >> 7) & 0x01u;
        gEeprom.SCANLIST_PRIORITY_CH[0] = rd16le(&s[1]);
        gEeprom.SCANLIST_PRIORITY_CH[1] = rd16le(&s[3]);
        gEeprom.CHAN_1_CALL             = rd16le(&s[5]);
    }

    if (!gEeprom.VFO_OPEN) {
        gEeprom.ScreenChannel[0] = gEeprom.MrChannel[0];
        gEeprom.ScreenChannel[1] = gEeprom.MrChannel[1];
    }

    unpack_globals(in);
    SETTINGS_FixupVfoPointers();
}

/* The record the blob keeps: offsets into the settings image. */
static uint16_t globals_offset(unsigned i)
{
    if (i == 0u)
        return O(0xA000);
    if (i == 1u)
        return O(0xA00E);
    if (i < 9u)
        return (uint16_t)(O(0xA0B9) + (i - 2u));
    return (uint16_t)(O(0xA150) + (i - 9u));
}

static uint8_t s_img[K1VIEW_SETTINGS_SIZE];   /* scratch for the globals record */

void k1_view_globals_pack(uint8_t out[K1VIEW_GLOBALS_SIZE])
{
    unsigned i;

    k1_settings_pack(s_img);
    for (i = 0; i < K1VIEW_GLOBALS_SIZE; i++)
        out[i] = s_img[globals_offset(i)];
}

void k1_view_globals_unpack(const uint8_t in[K1VIEW_GLOBALS_SIZE])
{
    unsigned i;

    memset(s_img, 0xFF, sizeof s_img);
    for (i = 0; i < K1VIEW_GLOBALS_SIZE; i++)
        s_img[globals_offset(i)] = in[i];
    unpack_globals(s_img);
}

/* ---------------------------------------------------------------------------
 * The address map
 * ------------------------------------------------------------------------- */

enum {
    R_HOLE,
    R_PHYS,      /* read/write flash at `phys` */
    R_CAL,       /* read flash at `phys`, writes dropped */
    R_VFO,       /* s_vfo shadow */
    R_SET        /* s_set shadow */
};

static uint8_t          s_vfo[K1VIEW_VFO_SIZE];
static uint8_t          s_set[K1VIEW_SETTINGS_SIZE];
static bool             s_open;
static k1_view_stats_t  s_stats;

/* The region `addr` is in, its physical address (R_PHYS/R_CAL), and how many
 * bytes from `addr` stay in it (at most `len`). */
static uint16_t region(uint16_t addr, uint16_t len, int *kind, uint32_t *phys)
{
    uint32_t end;
    uint32_t a = addr;

    *phys = 0;
    if (a < 0x4000u) {
        *kind = R_PHYS;
        *phys = K1_IMAGE_CH_BASE + a;
        end   = 0x4000u;
    } else if (a < 0x886Eu) {
        *kind = R_PHYS;                 /* names, attributes, list names */
        *phys = a;
        end   = 0x886Eu;
    } else if (a < K1VIEW_VFO_BASE) {
        *kind = R_HOLE;
        end   = K1VIEW_VFO_BASE;
    } else if (a < K1VIEW_VFO_BASE + K1VIEW_VFO_SIZE) {
        *kind = R_VFO;
        end   = K1VIEW_VFO_BASE + K1VIEW_VFO_SIZE;
    } else if (a < K1VIEW_SETTINGS_BASE) {
        *kind = R_HOLE;
        end   = K1VIEW_SETTINGS_BASE;
    } else if (a < 0xA0C8u) {
        *kind = R_SET;
        end   = 0xA0C8u;
    } else if (a < 0xA0E8u) {
        *kind = R_PHYS;                 /* boot message lines */
        *phys = a;
        end   = 0xA0E8u;
    } else if (a < 0xA148u) {
        *kind = R_SET;
        end   = 0xA148u;
    } else if (a < 0xA150u) {
        *kind = R_PHYS;                 /* spectrum settings */
        *phys = a;
        end   = 0xA150u;
    } else if (a < K1VIEW_SETTINGS_BASE + K1VIEW_SETTINGS_SIZE) {
        *kind = R_SET;
        end   = K1VIEW_SETTINGS_BASE + K1VIEW_SETTINGS_SIZE;
    } else if (a < K1VIEW_CAL_BASE) {
        *kind = R_HOLE;
        end   = K1VIEW_CAL_BASE;
    } else if (a < K1VIEW_CAL_BASE + K1VIEW_CAL_SIZE) {
        *kind = R_CAL;
        *phys = K1VIEW_CAL_PHYS + (a - K1VIEW_CAL_BASE);
        end   = K1VIEW_CAL_BASE + K1VIEW_CAL_SIZE;
    } else {
        *kind = R_HOLE;
        end   = 0x10000u;
    }

    return (uint16_t)((end - a) < len ? (end - a) : len);
}

static void snapshot(void)
{
    memset(s_vfo, 0xFF, sizeof s_vfo);
    codeplug_freq_snapshot(s_vfo, K1VIEW_VFO_SIZE - 8u);
    k1_settings_pack(s_set);
}

void k1_view_open(void)
{
    /* A second hello inside the same session (CHIRP reconnects) must not
     * throw away what the first one wrote. */
    if (!s_stats.settings_dirty && !s_stats.vfo_dirty)
        snapshot();
    s_open = true;
}

void k1_view_read(uint16_t addr, uint8_t *buf, uint8_t len)
{
    if (!s_open)
        k1_view_open();

    while (len > 0u) {
        int      kind;
        uint32_t phys;
        const uint16_t n = region(addr, len, &kind, &phys);

        switch (kind) {
        case R_PHYS:
        case R_CAL:
            PY25Q16_ReadBuffer(phys, buf, n);
            break;
        case R_VFO:
            memcpy(buf, &s_vfo[addr - K1VIEW_VFO_BASE], n);
            break;
        case R_SET:
            memcpy(buf, &s_set[addr - K1VIEW_SETTINGS_BASE], n);
            break;
        default:
            memset(buf, 0xFF, n);
            break;
        }
        addr = (uint16_t)(addr + n);
        buf += n;
        len  = (uint8_t)(len - n);
    }
}

static void phys_write(uint32_t phys, const uint8_t *buf, uint16_t n)
{
    uint8_t cur[128];
    uint16_t done = 0;

    while (done < n) {
        const uint16_t k = (uint16_t)(((uint32_t)(n - done) < sizeof cur) ? (uint32_t)(n - done) : sizeof cur);

        PY25Q16_ReadBuffer(phys + done, cur, k);
        if (memcmp(cur, buf + done, k) == 0) {
            s_stats.unchanged++;
        } else {
            PY25Q16_WriteBuffer(phys + done, buf + done, k, false);
            s_stats.written++;
        }
        done = (uint16_t)(done + k);
    }
}

/* Channel attributes (2 B each: band:3 compander:2 unused:2 exclude:1,
 * scanlist:8).  `exclude` is the K1's *runtime* "skip in this scan" flag;
 * upstream F4HWN clears it on every channel at boot, and CHIRP's driver never
 * sets it -- it leaves those bits as the slot held them, so a channel created
 * in an erased slot arrives with exclude = 1 and the scan skips it (radio
 * 2026-11-05: every uploaded channel was excluded and the memory scan sat on
 * one channel).  This port does not clear it at boot, so it is cleared here,
 * on the way in.  An entry of 0xFFFF (no channel) is kept. */
static void attr_write(uint16_t addr, uint32_t phys, const uint8_t *buf, uint16_t n)
{
    uint8_t  tmp[128];
    uint16_t i;

    memcpy(tmp, buf, n);
    for (i = 0; i < n; i++) {
        const uint16_t a = (uint16_t)(addr + i);

        if (((a - K1VIEW_ATTR_BASE) & 1u) != 0u)
            continue;                   /* the scanlist byte */
        if (i + 1u < n && tmp[i] == 0xFFu && tmp[i + 1u] == 0xFFu)
            continue;                   /* deleted / never used */
        tmp[i] &= 0x7Fu;
    }
    phys_write(phys, tmp, n);
}

void k1_view_write(uint16_t addr, const uint8_t *buf, uint8_t len)
{
    if (!s_open)
        k1_view_open();

    while (len > 0u) {
        int      kind;
        uint32_t phys;
        const uint16_t n = region(addr, len, &kind, &phys);

        switch (kind) {
        case R_PHYS:
            if (addr >= K1VIEW_ATTR_BASE && addr < K1VIEW_ATTR_END)
                attr_write(addr, phys, buf, n);
            else
                phys_write(phys, buf, n);
            break;
        case R_VFO:
            if (memcmp(&s_vfo[addr - K1VIEW_VFO_BASE], buf, n) != 0) {
                memcpy(&s_vfo[addr - K1VIEW_VFO_BASE], buf, n);
                s_stats.vfo_dirty = true;
            }
            break;
        case R_SET:
            if (memcmp(&s_set[addr - K1VIEW_SETTINGS_BASE], buf, n) != 0) {
                memcpy(&s_set[addr - K1VIEW_SETTINGS_BASE], buf, n);
                s_stats.settings_dirty = true;
            }
            break;
        default:                        /* holes, and the calibration */
            s_stats.dropped += n;
            break;
        }
        addr = (uint16_t)(addr + n);
        buf += n;
        len  = (uint8_t)(len - n);
    }
}

bool k1_view_commit(void)
{
    bool save = false;

    if (s_stats.vfo_dirty) {
        codeplug_freq_restore(s_vfo, K1VIEW_VFO_SIZE - 8u);
        save = true;
    }
    if (s_stats.settings_dirty) {
        k1_settings_unpack(s_set);
        save = true;
    }
    if (save)
        SETTINGS_SaveSettings();

    s_stats.vfo_dirty      = false;
    s_stats.settings_dirty = false;
    s_open                 = false;
    return save;
}

const k1_view_stats_t *k1_view_stats(void)
{
    return &s_stats;
}
