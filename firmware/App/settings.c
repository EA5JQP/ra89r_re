/* The port's settings layer.
 *
 * The K1's settings.c reads and writes a flat blob in the K1's own EEPROM
 * format.  This radio's codeplug is a different format on a different chip
 * (docs/ra89r_codeplug.md), so the *interface* is the K1's and the *layout* is the
 * stock's: every function below is the K1's contract, implemented against
 * settings.c for the stock's regions and against the port's own blob
 * (driver/py25q16.c) for the values the stock has no place for.
 *
 * What is deliberately read-only: the stock's codeplug.  Nothing here writes a
 * channel, a name or the settings block, so the stock firmware and the CPS keep
 * seeing exactly the radio they wrote.  The port's own state -- VFO
 * frequencies, its menu settings -- lives in the blob at 0x1FF000, which the
 * 2 MB part leaves empty.
 */
#include <string.h>

#include "app/dtmf.h"
#ifdef ENABLE_FMRADIO
    #include "app/fm.h"
#endif
#include "dcs.h"
#include "driver/py25q16.h"
#include "frequencies.h"
#include "misc.h"
#include "driver/py25q16.h"
#include "settings.h"
#include "version.h"

/* ---------------------------------------------------------------------------
 * The stock RA89R codeplug format (docs/ra89r_codeplug.md).
 *
 * This is the one piece with no K1 counterpart: the stock's foreign 21-byte
 * records, its two bitmaps and its tone encoding.  It lives here, next to the
 * K1 settings interface, so the port's only stock-specific knowledge has one
 * home.  On this radio the K1's own EEPROM image is imported from it (see
 * driver/py25q16.c) and the K1 code reads that image; this decoder is for the
 * import and for the stock's shared settings.
 * ------------------------------------------------------------------------- */

#define RA89R_CP_CODEPLUG_SIZE   0x4000u
#define RA89R_CP_RECORD_BASE     0x0000u
#define RA89R_CP_RECORD_STRIDE   21u
#define RA89R_CP_RECORD_COUNT    210u
#define RA89R_CP_NAME_BASE       4416u
#define RA89R_CP_NAME_STRIDE     10u
#define RA89R_CP_ENABLE_BASE     7936u
#define RA89R_CP_SKIP_BASE       7968u
#define RA89R_CP_BITMAP_BYTES    32u
#define RA89R_CP_BAND_BASE       8000u
#define RA89R_CP_OPEN_NAME_BASE  8048u
#define RA89R_CP_FREQ_CODE_BASE  8080u
#define RA89R_CP_MODEL_NAME_BASE 8096u
#define RA89R_CP_CAL_BASE        8208u
#define RA89R_CP_SETTINGS_BASE   8224u
#define RA89R_CP_SIGNATURE_BASE  0x3FF0u

typedef struct {
    uint32_t rx_frequency;
    uint32_t tx_frequency;
    uint16_t rx_tone;
    uint16_t tx_tone;
    uint8_t  flags_a;
    uint8_t  flags_b;
    uint8_t  flags_c;
    char     name[6];
} __attribute__((packed)) ra89r_codeplug_record_t;

#define RA89R_CP_FLAGA_TX_INHIBIT 0x01u
#define RA89R_CP_FLAGA_FREQ_REV   0x02u
#define RA89R_CP_FLAGA_BUSY_MASK  0x0Cu
#define RA89R_CP_FLAGA_BW_MASK    0x30u
#define RA89R_CP_FLAGA_POWER_MASK 0xC0u
#define RA89R_CP_FLAGB_GPS        0x01u
#define RA89R_CP_FLAGB_TALKAROUND 0x02u
#define RA89R_CP_FLAGB_NAME       0x04u
#define RA89R_CP_FLAGB_SIG_MASK   0x18u
#define RA89R_CP_FLAGB_SQL_MASK   0x60u
#define RA89R_CP_FLAGC_STEP_MASK  0x0Fu

void     codeplug_init(void);
bool     codeplug_read(uint16_t channel, ra89r_codeplug_record_t *record);
bool     codeplug_used(uint16_t channel);
bool     codeplug_excluded(uint16_t channel);
bool     codeplug_scan_info(uint16_t channel, ChannelScanDisplayInfo_t *info);
void     codeplug_name(char *out, size_t size, uint16_t channel);
uint16_t codeplug_attributes(uint16_t channel);
bool     codeplug_freq_get(uint16_t channel, uint8_t vfo, ChannelScanDisplayInfo_t *info);
void     codeplug_freq_set(uint16_t channel, uint8_t vfo, const ChannelScanDisplayInfo_t *info);
void     codeplug_freq_snapshot(uint8_t *dest, size_t size);
bool     codeplug_freq_restore(const uint8_t *src, size_t size);
void     codeplug_shared_settings(void);

EEPROM_Config_t gEeprom;

/* The VFO objects live inside gEeprom and carry pointers into themselves, so
 * anything that replaces or clears gEeprom -- the defaults, or a blob read back
 * from flash -- has to re-establish them before the screens dereference them.
 * The K1 does this in its boot; here it is one call the boot and the settings
 * load both make. */
void SETTINGS_FixupVfoPointers(void)
{
    unsigned i;

    for (i = 0; i < 2; i++) {
        VFO_Info_t *vfo = &gEeprom.VfoInfo[i];

        vfo->pRX = &vfo->freq_config_RX;
        vfo->pTX = &vfo->freq_config_TX;
    }

    if (gEeprom.TX_VFO > 1u)
        gEeprom.TX_VFO = 0;
    if (gEeprom.RX_VFO > 1u)
        gEeprom.RX_VFO = 0;

    gTxVfo = &gEeprom.VfoInfo[gEeprom.TX_VFO];
    gRxVfo = &gEeprom.VfoInfo[gEeprom.RX_VFO];
    gCurrentVfo = gRxVfo;
}

/* The port's own state, saved in the blob next to gEeprom. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    /* Was `uint16_t reserved` (always zero).  Reused for the per-VFO RF
     * transceiver choice so the blob layout and its version are unchanged: an
     * older blob reads back as {0,0} = AUTO for both VFOs. */
    uint8_t  rf_xcvr[2];                  /* rf_xcvr_t, VFO A and B */
    uint8_t  freq_channels[7 * 2 * 16];   /* codeplug_freq_snapshot() */
#ifdef ENABLE_FMRADIO
    uint16_t fm_channels[FM_CHANNELS_MAX]; /* the K1's FM memories (app/fm.c) */
#endif
} settings_extra_t;

#define EXTRA_MAGIC   0x58545241u    /* "ARTX" */
#define EXTRA_VERSION 2u

/* The per-VFO RF transceiver choice, mirrored into the blob on save. */
static uint8_t s_rf_xcvr[2] = { RF_XCVR_BK4829, RF_XCVR_BK4829 };

rf_xcvr_t SETTINGS_GetVfoTransceiver(uint8_t vfo)
{
    const uint8_t v = (vfo > 1u) ? (uint8_t)RF_XCVR_BK4829 : s_rf_xcvr[vfo];

    /* Old settings blobs contain 0 for Auto in these former reserved bytes;
     * invalid values also safely fall back to the fixed BK4829 default. */
    if (v != (uint8_t)RF_XCVR_BK4829 && v != (uint8_t)RF_XCVR_BK4815)
        return RF_XCVR_BK4829;
    return (rf_xcvr_t)v;
}

void SETTINGS_SetVfoTransceiver(uint8_t vfo, rf_xcvr_t xcvr)
{
    if (vfo > 1u)
        return;
    if (xcvr != RF_XCVR_BK4829 && xcvr != RF_XCVR_BK4815)
        xcvr = RF_XCVR_BK4829;
    s_rf_xcvr[vfo] = (uint8_t)xcvr;
}

void SettingsDefaults(void)
{
    memset(&gEeprom, 0, sizeof gEeprom);

    s_rf_xcvr[0] = (uint8_t)RF_XCVR_BK4829;
    s_rf_xcvr[1] = (uint8_t)RF_XCVR_BK4829;

    gEeprom.RX_VFO = 0;
    gEeprom.TX_VFO = 0;
    gEeprom.ScreenChannel[0] = 0;
    gEeprom.ScreenChannel[1] = 0;
    gEeprom.FreqChannel[0] = FREQ_CHANNEL_FIRST;
    gEeprom.FreqChannel[1] = FREQ_CHANNEL_FIRST;
    gEeprom.MrChannel[0] = 0;
    gEeprom.MrChannel[1] = 0;

    gEeprom.SQUELCH_LEVEL = 4;
    gEeprom.TX_TIMEOUT_TIMER = 3;
    gEeprom.KEY_LOCK = false;
    /* The K1's SET_NAV picks which way the up/down keys run: false is the
     * UV-K1's left/right layout, which makes UP step *down* a channel.  This
     * radio has up/down keys, so it wants the UV-K5(8) sense. */
    gEeprom.SET_NAV = true;
    gEeprom.VOX_SWITCH = false;
    gEeprom.VOX_LEVEL = 5;
    gEeprom.BEEP_CONTROL = true;
    gEeprom.CHANNEL_DISPLAY_MODE = 0;
    gEeprom.TAIL_TONE_ELIMINATION = false;
    gEeprom.VFO_OPEN = true;
    /* The double-channel UI no longer rides on dual-watch: the port forces
     * ui/main.c's isMainOnly() false (k1_features.h) to draw both rows, and
     * leaves dual-watch and cross-band OFF.  Both are RF behaviours -- the K1's
     * DualwatchAlternate() toggles the receive VFO between the two channels --
     * and the port has no engine for them. */
    gEeprom.DUAL_WATCH = DUAL_WATCH_OFF;
    gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;
    gEeprom.BATTERY_SAVE = 0;
    gEeprom.BACKLIGHT_TIME = 4;
    /* The scan-resume delay (`ScnRev`).  0 is the K1's "stop scanning" mode:
     * app/app.c's end-of-RX path calls CHFRSCANNER_Stop() when it is 0, so the
     * first carrier ends the scan entirely.  The K1's own fallback when its
     * EEPROM byte is absent is 14 (a 3.5 s pause), which is what a fresh radio
     * should resume with. */
    gEeprom.SCAN_RESUME_MODE = 14;
    /* The K1's scan-list selector is 1..24 plus "all" (`MR_CHANNELS_LIST + 1`,
     * and 0 means "no list at all").  Every channel this port reports is in
     * "all" (the stock has one channel set, not 24 lists), so 0 would make
     * RADIO_CheckValidChannel() reject every channel it is asked about with a
     * list check -- and the channel-up/down walk then finds nothing. */
    gEeprom.SCAN_LIST_DEFAULT = MR_CHANNELS_LIST + 1;
    gEeprom.SCAN_LIST_ENABLED = false;
    gEeprom.CURRENT_STATE = 0;
    gEeprom.CURRENT_LIST = 0;

    gEeprom.MIC_SENSITIVITY = 4;
    gEeprom.MIC_SENSITIVITY_TUNING = 0;
    gEeprom.VOLUME_GAIN = 8;
    gEeprom.DAC_GAIN = 8;
    gEeprom.BATTERY_TYPE = BATTERY_TYPE_2800_MAH;
    /* The K1 loads this from its settings block and maps a blank (0xFF) field
     * to 2 (percentage).  The port bypasses that loader, so it used to stay 0
     * -- and app/app.c only refreshes the status bar, where the battery icon
     * lives, when this is > 0.  The bar was therefore painted once at boot,
     * before the first sample, and the icon stayed empty. */
    gSetting_battery_text = 2;
    gEeprom.POWER_ON_DISPLAY_MODE = POWER_ON_DISPLAY_MODE_ALL;
    gEeprom.ROGER = ROGER_MODE_OFF;
    gEeprom.BACKLIGHT_MIN = 1;
    gEeprom.BACKLIGHT_MAX = 5;

#ifdef ENABLE_FMRADIO
    /* The FM broadcast feature (app/fm.c).  The K1 loads these from its flat
     * EEPROM; this port's stock codeplug has no such field, so the defaults are
     * the port's and the user's choices round-trip in the port's own blob.
     * BAND 1 = 76-108 MHz, the K1's usual one; 87.5 MHz is its lower edge. */
    gEeprom.FM_Band              = 1;
    gEeprom.FM_SelectedFrequency = 875;
    gEeprom.FM_FrequencyPlaying  = 875;
    gEeprom.FM_SelectedChannel   = 0;
    gEeprom.FM_IsMrMode          = false;
#endif
}

/* The first channel the codeplug has at or after `start`, or 0xFFFF. */
static uint16_t settings_first_channel(uint16_t start)
{
    uint16_t channel;

    for (channel = start; channel < RA89R_CP_RECORD_COUNT; channel++) {
        ra89r_codeplug_record_t record;

        if (!codeplug_used(channel))
            continue;
        if (!codeplug_read(channel, &record))
            continue;
        if (record.rx_frequency == 0u || record.rx_frequency == 0xFFFFFFFFu)
            continue;
        return channel;
    }

    return 0xFFFFu;
}

void SETTINGS_InitEEPROM(void)
{
    SettingsDefaults();

    /* The external NOR driver first: everything below reads the chip, and a
     * read taken before the bus is up caches 0xFF -- a radio with no channels
     * and no stored settings, which is exactly what it is not. */
    storage_init();
    codeplug_init();

    /* Give the K1 its own EEPROM image in the erased band (driver/py25q16.c); it
     * imports from the stock on first use.  Do it before the calibration load
     * and the attribute cache, which both read it. */
    storage_import_k1();

    /* The K1's channel-attribute cache marks an unused slot with
     * channel_id == 0xFFFF, so it has to be initialised before the first lookup
     * -- otherwise a lookup of channel 0 hits the zeroed slot and is told the
     * channel has band 0 and no scan lists.  misc.c has the function; its own
     * comment says it belongs in this boot sequence. */
    MR_InitChannelAttributesCache();

    /* The stock's shared settings next: they are what a radio configured by
     * the stock firmware or its CPS already carries. */
    codeplug_shared_settings();

#ifdef ENABLE_FMRADIO
    /* The FM memories start empty -- 0xFFFF is the K1's "no channel" marker
     * (`FM_CheckValidChannel`).  A blob, if there is one, replaces them below. */
    memset(gFM_Channels, 0xFF, sizeof gFM_Channels);
#endif

    /* Then the port's own blob, which wins where the two overlap: it is what
     * the user last set with this firmware. */
    if (storage_load_settings()) {
        settings_extra_t extra;

        memset(&extra, 0, sizeof extra);
        if (storage_get_extra(&extra, sizeof extra) &&
            extra.magic == EXTRA_MAGIC &&
            extra.version == EXTRA_VERSION) {
            codeplug_freq_restore(extra.freq_channels, sizeof extra.freq_channels);
            memcpy(s_rf_xcvr, extra.rf_xcvr, sizeof s_rf_xcvr);
#ifdef ENABLE_FMRADIO
            memcpy(gFM_Channels, extra.fm_channels, sizeof gFM_Channels);
#endif
        }
    } else {
        /* No blob yet: land the two VFOs on the first two channels the codeplug
         * actually has, so the double-channel screen shows two real rows on any
         * radio rather than a bare frequency. */
        const uint16_t first = settings_first_channel(0);
        const uint16_t second = (first == 0xFFFFu) ? 0xFFFFu
                                                   : settings_first_channel((uint16_t)(first + 1u));

        if (first != 0xFFFFu) {
            gEeprom.ScreenChannel[0] = first;
            gEeprom.MrChannel[0]     = first;
        }
        if (second != 0xFFFFu) {
            gEeprom.ScreenChannel[1] = second;
            gEeprom.MrChannel[1]     = second;
        }
    }

    /* A blob saved before the layout was decoupled still carries a dual-watch
     * mode, and the K1's dual-watch engine would then toggle the receiver
     * between the two VFOs.  The receiver must follow the selected VFO, so
     * force it off whatever the blob says. */
    gEeprom.DUAL_WATCH = DUAL_WATCH_OFF;

    /* A blob saved before BATTERY_TYPE_2800_MAH existed carries the old default
     * (0 = 1600 mAh), the wrong curve for this radio's 2800 mAh pack.  The blob
     * round-trips the whole gEeprom, so the default in SettingsDefaults() is
     * overwritten on every boot; force the type here, as DUAL_WATCH is.  The
     * BatTyp menu can still change it at runtime. */
    gEeprom.BATTERY_TYPE = BATTERY_TYPE_2800_MAH;

#ifdef ENABLE_FMRADIO
    /* The K1's own boot does this right after loading the FM memories: if the
     * selected memory is empty it falls back, and FM_FrequencyPlaying is set
     * from the selected frequency/memory. */
    FM_ConfigureChannelState();
#endif
}

void SETTINGS_LoadCalibration(void)
{
    /* The K1's calibration image now exists in the port's own store at its
     * native addresses (driver/py25q16.c imports it from the stock's 0x3000
     * window).  This is the K1's own loader, unchanged except for where the
     * bytes are -- they used to live in a region that is erased on this radio. */
    uint8_t  misc[8];
    int16_t  xtal;

    PY25Q16_ReadBuffer(0x100C0u, gEEPROM_RSSI_CALIB[3], 8);
    memcpy(gEEPROM_RSSI_CALIB[4], gEEPROM_RSSI_CALIB[3], 8);
    memcpy(gEEPROM_RSSI_CALIB[5], gEEPROM_RSSI_CALIB[3], 8);
    memcpy(gEEPROM_RSSI_CALIB[6], gEEPROM_RSSI_CALIB[3], 8);

    PY25Q16_ReadBuffer(0x100C8u, gEEPROM_RSSI_CALIB[0], 8);
    memcpy(gEEPROM_RSSI_CALIB[1], gEEPROM_RSSI_CALIB[0], 8);
    memcpy(gEEPROM_RSSI_CALIB[2], gEEPROM_RSSI_CALIB[0], 8);

    PY25Q16_ReadBuffer(0x10140u, gBatteryCalibration, 12);
    if (gBatteryCalibration[0] >= 5000) {
        gBatteryCalibration[0] = 1900;
        gBatteryCalibration[1] = 2000;
    }
    /* [3] is the divisor in BATTERY_GetReadings' (voltage * 760) / [3].  On this
     * port the pack is read on ADC channel 9 (board.c -> driver/battery.c) and
     * reported in 10 mV, so the identity is 760 -- the K1's own battery menu can
     * still trim it.  The flash's value is the K1's raw-ADC reference (2300 on
     * this radio), which must not be used: it divides the 10 mV reading by ~3. */
    gBatteryCalibration[3] = 760;
    gBatteryCalibration[5] = 2300;

#ifdef ENABLE_VOX
    PY25Q16_ReadBuffer(0x10150u + (gEeprom.VOX_LEVEL * 2),
                       &gEeprom.VOX1_THRESHOLD, 2);
    PY25Q16_ReadBuffer(0x10168u + (gEeprom.VOX_LEVEL * 2),
                       &gEeprom.VOX0_THRESHOLD, 2);
#endif

    gEeprom.MIC_SENSITIVITY_TUNING = gMicGain_dB2[gEeprom.MIC_SENSITIVITY];

    PY25Q16_ReadBuffer(0x10188u, misc, sizeof misc);
    xtal = (int16_t)((uint16_t)misc[0] | ((uint16_t)misc[1] << 8));
    gEeprom.BK4819_XTAL_FREQ_LOW = (xtal >= -1000 && xtal <= 1000) ? xtal : 0;
    gEEPROM_1F8A = (uint16_t)((uint16_t)misc[2] | ((uint16_t)misc[3] << 8)) & 0x01FFu;
    gEEPROM_1F8C = (uint16_t)((uint16_t)misc[4] | ((uint16_t)misc[5] << 8)) & 0x01FFu;
    gEeprom.VOLUME_GAIN = (misc[6] < 64u) ? misc[6] : 58u;
    gEeprom.DAC_GAIN = (misc[7] < 16u) ? misc[7] : 8u;
#ifdef ENABLE_FEAT_F4HWN
    gEeprom.VOLUME_GAIN_BACKUP = gEeprom.VOLUME_GAIN;
#endif

    BK4819_WriteRegister(BK4819_REG_3B,
                         (uint16_t)(22656 + gEeprom.BK4819_XTAL_FREQ_LOW));
}

/* ---------------------------------------------------------------------------
 * Channels
 * ------------------------------------------------------------------------- */

uint32_t SETTINGS_FetchChannelFrequency(const uint16_t channel)
{
    if (IS_MR_CHANNEL(channel)) {
        uint32_t freq = 0u;

        PY25Q16_ReadBuffer(K1_IMAGE_CH_BASE + (uint32_t)channel * 16u, &freq, 4u);
        if (freq == 0xFFFFFFFFu)
            return 0u;
        return freq;
    }

    if (IS_FREQ_CHANNEL(channel)) {
        ChannelScanDisplayInfo_t info;

        if (!codeplug_freq_get(channel, gEeprom.RX_VFO, &info))
            return 0u;
        return info.rx.Frequency;
    }

    return 0u;
}

bool SETTINGS_FetchChannelScanInfo(const uint16_t channel, uint32_t *frequency, ModulationMode_t *modulation)
{
    ChannelScanDisplayInfo_t info;

    if (!SETTINGS_FetchChannelScanDisplayInfo(channel, &info)) {
        if (frequency != 0)
            *frequency = 0u;
        if (modulation != 0)
            *modulation = MODULATION_FM;
        return false;
    }

    if (frequency != 0)
        *frequency = info.rx.Frequency;
    if (modulation != 0)
        *modulation = info.modulation;
    return true;
}

bool SETTINGS_FetchChannelScanDisplayInfo(const uint16_t channel, ChannelScanDisplayInfo_t *info)
{
    if (info == 0)
        return false;

    if (IS_MR_CHANNEL(channel)) {
        uint8_t raw[16];

        PY25Q16_ReadBuffer(K1_IMAGE_CH_BASE + (uint32_t)channel * 16u, raw, sizeof raw);
        if ((raw[0] | raw[1] | raw[2] | raw[3]) == 0u ||
            (raw[0] & raw[1] & raw[2] & raw[3]) == 0xFFu)
            return false;
        codeplug_channel_unpack(raw, info);
        return true;
    }

    if (IS_FREQ_CHANNEL(channel))
        return codeplug_freq_get(channel, gEeprom.RX_VFO, info);

    return false;
}

void SETTINGS_FetchChannelName(char *s, const uint16_t channel)
{
    if (s == 0)
        return;

    s[0] = 0;

    if (!IS_MR_CHANNEL(channel))
        return;

    {
        char raw[10];
        size_t i;

        PY25Q16_ReadBuffer(K1_IMAGE_NAME_BASE + (uint32_t)channel * 16u,
                           raw, sizeof raw);
        for (i = 0; i < sizeof raw && i < 10u; i++) {
            if (raw[i] < 32 || raw[i] > 126)
                break;
            s[i] = raw[i];
        }
        s[i] = 0;
        while (i > 0 && s[i - 1] == ' ')
            s[--i] = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Writing.  The stock's regions stay untouched: an edited channel, and a
 * frequency channel, are written into the port's own K1 image (driver/py25q16.c),
 * and the port's settings state goes to its blob.  The stock's codeplug,
 * calibration window and signature are read-only.
 * ------------------------------------------------------------------------- */

/* A deferred save has been asked for (see SETTINGS_SaveVfoIndices). */
static bool settings_dirty;

static bool settings_save_all(void)
{
    settings_extra_t extra;
    memset(&extra, 0, sizeof extra);
    extra.magic = EXTRA_MAGIC;
    extra.version = EXTRA_VERSION;
    memcpy(extra.rf_xcvr, s_rf_xcvr, sizeof extra.rf_xcvr);
    codeplug_freq_snapshot(extra.freq_channels, sizeof extra.freq_channels);
#ifdef ENABLE_FMRADIO
    memcpy(extra.fm_channels, gFM_Channels, sizeof extra.fm_channels);
#endif

    if (!storage_set_extra(&extra, sizeof extra))
        return false;

    return storage_save_settings();
}

void SETTINGS_SaveSettings(void)
{
    /* A menu change: write now, and there is nothing left pending afterwards. */
    settings_dirty = false;
    (void)settings_save_all();
}

#ifdef ENABLE_FMRADIO
void SETTINGS_SaveFM(void)
{
    /* The K1 writes its FM config and memories into its flat EEPROM at
     * 0x00A020/0x00A028.  On this radio those addresses are inside the stock's
     * codeplug, which the port never writes (docs/ra89r_port.md); the FM state
     * -- the config fields are already part of gEeprom -- and the 48 memories
     * go into the port's own blob instead, through the same save path as the
     * rest of the settings. */
    (void)settings_save_all();
}
#endif

/* The VFO indices are the K1's *deferred* save: SETTINGS_SaveVfoIndices() asks
 * for one and SETTINGS_SaveVfoIndicesFlush() -- which APP_TimeSlice10ms() calls
 * on every 10 ms slice -- performs it if it was asked for.  Writing here
 * unconditionally would erase and program a 4 KB sector a hundred times a
 * second, which is slow enough to stop the radio behaving like a radio. */
void SETTINGS_SaveVfoIndices(void)
{
    settings_dirty = true;
}

void SETTINGS_SaveVfoIndicesFlush(void)
{
    if (!settings_dirty)
        return;

    settings_dirty = false;
    (void)settings_save_all();
}

void SETTINGS_SaveChannel(uint16_t Channel, uint8_t VFO, const VFO_Info_t *pVFO, uint8_t Mode)
{
    (void)VFO;
    (void)Mode;

    /* Both kinds of channel live in the port's own image now (the K1's own
     * format), so a user edit is a write into the K1 image, not the stock's. */
    if (pVFO != 0 && (IS_FREQ_CHANNEL(Channel) || IS_MR_CHANNEL(Channel))) {
        ChannelScanDisplayInfo_t info;

        memset(&info, 0, sizeof info);
        info.rx = pVFO->freq_config_RX;
        info.tx = pVFO->freq_config_TX;
        info.offset = pVFO->TX_OFFSET_FREQUENCY;
        info.stepSetting = pVFO->STEP_SETTING;
        info.stepFrequency = pVFO->StepFrequency;
        info.modulation = pVFO->Modulation;
        info.txOffsetFrequencyDirection = pVFO->TX_OFFSET_FREQUENCY_DIRECTION;
        info.outputPower = pVFO->OUTPUT_POWER;
        info.frequencyReverse = pVFO->FrequencyReverse;
        info.channelBandwidth = pVFO->CHANNEL_BANDWIDTH;
        info.busyChannelLock = pVFO->BUSY_CHANNEL_LOCK;
        info.txLock = pVFO->TX_LOCK;
        info.dtmfPttIdTxMode = pVFO->DTMF_PTT_ID_TX_MODE;

        if (IS_FREQ_CHANNEL(Channel)) {
            codeplug_freq_set(Channel, gEeprom.TX_VFO, &info);
        } else {
            uint8_t raw[16];

            codeplug_channel_pack(&info, raw);
            PY25Q16_WriteBuffer(K1_IMAGE_CH_BASE + (uint32_t)Channel * 16u,
                                raw, sizeof raw, false);
        }

        /* The K1 writes the channel's attributes alongside the record.  For a
         * frequency channel this is what makes the channel valid:
         * RADIO_ConfigureChannel only reloads the channel's own frequency when
         * MR_GetChannelAttributes() finds a real entry, and an absent one
         * (0xFFFF) makes it reset to the band's lower frequency.  Without this,
         * a frequency typed in the VFO reverts as soon as the save-triggered
         * reconfigure runs. */
        SETTINGS_UpdateChannel(Channel, pVFO, true);
    }

    (void)settings_save_all();
}

void SETTINGS_SaveChannelName(uint16_t channel, const char *name)
{
    if (IS_MR_CHANNEL(channel) && name != 0) {
        char raw[16];
        unsigned i;

        memset(raw, ' ', sizeof raw);
        for (i = 0; i < sizeof raw && name[i] != 0; i++)
            raw[i] = name[i];
        PY25Q16_WriteBuffer(K1_IMAGE_NAME_BASE + (uint32_t)channel * 16u,
                            raw, sizeof raw, false);
    }
}

void SETTINGS_UpdateChannel(uint16_t channel, const VFO_Info_t *pVFO, bool keep)
{
    /* The K1's own implementation: this writes the *attributes*, not the
     * record (the port used to send this through SETTINGS_SaveChannel, which is
     * both circular and the wrong table).  A frequency channel's attributes make
     * it valid for RADIO_ConfigureChannel(); the MR clear-name branch is the
     * K1's too.  pVFO may be NULL when keep is false. */
    ChannelAttributes_t att = {
        .band      = 0x7,
        .compander = 0,
        .unused_1  = 0,
        .unused_2  = 0,
        .exclude   = 0,
        .scanlist  = 0,
    };

    if (keep && pVFO != 0) {
        att.band      = pVFO->Band;
        att.compander = pVFO->Compander;
        att.scanlist  = pVFO->SCANLIST_PARTICIPATION;
    }

    MR_SetChannelAttributes(channel, &att);

    if (IS_MR_CHANNEL(channel) && !keep)
        SETTINGS_SaveChannelName(channel, "");
}

void SETTINGS_FactoryReset(bool bIsAll)
{
    (void)bIsAll;
    /* Never: this would erase the stock's codeplug.  A factory reset here would
     * have to restore the port's own blob, and that is not needed yet. */
}

void SETTINGS_SaveBatteryCalibration(const uint16_t *batteryCalibration)
{
    (void)batteryCalibration;
    /* The gauge chip has not answered yet (docs/ra89r_battery.md); there is nothing
     * to calibrate against. */
}

void SETTINGS_ResetTxLock(void)
{
    /* The K1 walks its channel table clearing TX_LOCK.  Here that is a write
     * into the stock's records; the lock is per channel and the user can still
     * change one from the radio. */
}
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
 * so they live in RAM once they have been read.  `codeplug_init()` is what
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

void codeplug_init(void)
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

bool codeplug_read(uint16_t channel, ra89r_codeplug_record_t *record)
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

bool codeplug_used(uint16_t channel)
{
    if (channel >= RA89R_CP_RECORD_COUNT)
        return false;

    cp_load_bitmaps();
    return cp_bitmap_get(cp_enable, channel);
}

bool codeplug_excluded(uint16_t channel)
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

bool codeplug_scan_info(uint16_t channel, ChannelScanDisplayInfo_t *info)
{
    ra89r_codeplug_record_t record;

    if (info == 0 || channel >= RA89R_CP_RECORD_COUNT)
        return false;
    if (!codeplug_used(channel))
        return false;
    if (!codeplug_read(channel, &record))
        return false;
    if (record.rx_frequency == 0u || record.rx_frequency == 0xFFFFFFFFu)
        return false;

    cp_decode_record(&record, info);
    return true;
}

void codeplug_name(char *out, size_t size, uint16_t channel)
{
    ra89r_codeplug_record_t record;
    char extended[RA89R_CP_NAME_STRIDE];
    size_t used = 0;
    size_t i;

    if (out == 0 || size == 0u)
        return;

    out[0] = 0;

    if (channel >= RA89R_CP_RECORD_COUNT || !codeplug_used(channel) || size < 2u)
        return;

    /* The record's own six characters come first; the ten-byte table at 4416 is
     * the extension the CPS appends for names that need more room. */
    if (codeplug_read(channel, &record)) {
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

uint16_t codeplug_attributes(uint16_t channel)
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
    } else if (channel < RA89R_CP_RECORD_COUNT && codeplug_used(channel)) {
        if (cp_band[channel] == 0u) {
            ra89r_codeplug_record_t record;

            if (!codeplug_read(channel, &record) || record.rx_frequency == 0u ||
                record.rx_frequency == 0xFFFFFFFFu)
                return 0xFFFFu;

            cp_band[channel] = (uint8_t)(FREQUENCY_GetBand(record.rx_frequency) + 1);
        }
        band     = (uint8_t)(cp_band[channel] - 1u);
        scanlist = MR_CHANNELS_LIST + 1;   /* the stock has one set, not lists */
        exclude  = codeplug_excluded(channel) ? 1u : 0u;
    } else {
        return 0xFFFFu;                    /* not a channel this radio has */
    }

    value  = (uint16_t)(band & 0x07u);
    value |= (uint16_t)((exclude & 0x01u) << 7);
    value |= (uint16_t)((uint16_t)scanlist << 8);
    return value;
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

void codeplug_channel_pack(const ChannelScanDisplayInfo_t *info, uint8_t *out)
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

void codeplug_channel_unpack(const uint8_t *in, ChannelScanDisplayInfo_t *info)
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

static void cp_freq_default_one(unsigned band, unsigned vfo)
{
    ChannelScanDisplayInfo_t info;

    memset(&info, 0, sizeof info);
    info.rx.Frequency = frequencyBandTable[band].lower;
    info.tx.Frequency = info.rx.Frequency;
    info.modulation = MODULATION_FM;
    info.stepSetting = STEP_12_5kHz;
    info.stepFrequency = gStepFrequencyTable[STEP_12_5kHz];
    info.outputPower = OUTPUT_POWER_HIGH;
    info.txLock = 0;
    codeplug_channel_pack(&info, cp_freq[band][vfo]);
}

static void cp_freq_defaults(void)
{
    unsigned band;
    unsigned vfo;

    for (band = 0; band < CP_FREQ_BANDS; band++)
        for (vfo = 0; vfo < CP_FREQ_VFOS; vfo++)
            cp_freq_default_one(band, vfo);
}

bool codeplug_freq_get(uint16_t channel, uint8_t vfo, ChannelScanDisplayInfo_t *info)
{
    const unsigned band = (unsigned)(channel - FREQ_CHANNEL_FIRST);

    if (info == 0 || band >= CP_FREQ_BANDS || vfo >= CP_FREQ_VFOS)
        return false;

    codeplug_channel_unpack(cp_freq[band][vfo], info);
    return true;
}

void codeplug_freq_set(uint16_t channel, uint8_t vfo, const ChannelScanDisplayInfo_t *info)
{
    const unsigned band = (unsigned)(channel - FREQ_CHANNEL_FIRST);

    if (info == 0 || band >= CP_FREQ_BANDS || vfo >= CP_FREQ_VFOS)
        return;

    codeplug_channel_pack(info, cp_freq[band][vfo]);
}

void codeplug_freq_snapshot(uint8_t *dest, size_t size)
{
    if (dest == 0 || size < sizeof cp_freq)
        return;

    memcpy(dest, cp_freq, sizeof cp_freq);
}

bool codeplug_freq_restore(const uint8_t *src, size_t size)
{
    unsigned band;
    unsigned vfo;

    if (src == 0 || size < sizeof cp_freq)
        return false;

    memcpy(cp_freq, src, sizeof cp_freq);

    /* A stored entry with a zero frequency is not an entry: it decodes as 0 Hz,
     * which RADIO_ConfigureChannel then clamps into band 1 -- 18 MHz in
     * frequency mode, which is what a settings blob saved before the defaults
     * existed produced.  Keep the band's own default instead. */
    for (band = 0; band < CP_FREQ_BANDS; band++) {
        for (vfo = 0; vfo < CP_FREQ_VFOS; vfo++) {
            const uint8_t *e = cp_freq[band][vfo];

            if ((e[0] | e[1] | e[2] | e[3]) == 0u)
                cp_freq_default_one(band, vfo);
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * The stock's shared settings
 * ------------------------------------------------------------------------- */

void codeplug_shared_settings(void)
{
    /* Not mapped yet: docs/ra89r_codeplug.md records what the block contains and
     * which of it the K1 has an equivalent for.  Until then the port's own
     * defaults and its blob decide, and the stock's block is left alone. */
}
