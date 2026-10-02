/* The port's settings layer.
 *
 * The K1's settings.c reads and writes a flat blob in the K1's own EEPROM
 * format.  This radio's codeplug is a different format on a different chip
 * (docs/ra89r_codeplug.md), so the *interface* is the K1's and the *layout* is the
 * stock's: every function below is the K1's contract, implemented against
 * port_codeplug.c for the stock's regions and against the port's own blob
 * (port_storage.c) for the values the stock has no place for.
 *
 * What is deliberately read-only: the stock's codeplug.  Nothing here writes a
 * channel, a name or the settings block, so the stock firmware and the CPS keep
 * seeing exactly the radio they wrote.  The port's own state -- VFO
 * frequencies, its menu settings -- lives in the blob at 0x1FF000, which the
 * 2 MB part leaves empty.
 */
#include <string.h>

#include "app/dtmf.h"
#include "driver/py25q16.h"
#include "frequencies.h"
#include "misc.h"
#include "port_codeplug.h"
#include "port_storage.h"
#include "settings.h"
#include "version.h"

EEPROM_Config_t gEeprom;

/* The port's own state, saved in the blob next to gEeprom. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint8_t  freq_channels[7 * 2 * 16];   /* port_codeplug_freq_snapshot() */
} port_settings_extra_t;

#define PORT_EXTRA_MAGIC   0x58545241u    /* "ARTX" */
#define PORT_EXTRA_VERSION 1u

void PORT_SettingsDefaults(void)
{
    memset(&gEeprom, 0, sizeof gEeprom);

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
     * ui/main.c's isMainOnly() false (port_features.h) to draw both rows, and
     * leaves dual-watch and cross-band OFF.  Both are RF behaviours -- the K1's
     * DualwatchAlternate() toggles the receive VFO between the two channels --
     * and the port has no engine for them. */
    gEeprom.DUAL_WATCH = DUAL_WATCH_OFF;
    gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;
    gEeprom.BATTERY_SAVE = 0;
    gEeprom.BACKLIGHT_TIME = 4;
    gEeprom.SCAN_RESUME_MODE = 0;
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
    gEeprom.BATTERY_TYPE = BATTERY_TYPE_1600_MAH;
    gEeprom.POWER_ON_DISPLAY_MODE = POWER_ON_DISPLAY_MODE_ALL;
    gEeprom.ROGER = ROGER_MODE_OFF;
    gEeprom.BACKLIGHT_MIN = 1;
    gEeprom.BACKLIGHT_MAX = 5;
}

/* The first channel the codeplug has at or after `start`, or 0xFFFF. */
static uint16_t settings_first_channel(uint16_t start)
{
    uint16_t channel;

    for (channel = start; channel < RA89R_CP_RECORD_COUNT; channel++) {
        ra89r_codeplug_record_t record;

        if (!port_codeplug_used(channel))
            continue;
        if (!port_codeplug_read(channel, &record))
            continue;
        if (record.rx_frequency == 0u || record.rx_frequency == 0xFFFFFFFFu)
            continue;
        return channel;
    }

    return 0xFFFFu;
}

void SETTINGS_InitEEPROM(void)
{
    PORT_SettingsDefaults();

    /* The external NOR driver first: everything below reads the chip, and a
     * read taken before the bus is up caches 0xFF -- a radio with no channels
     * and no stored settings, which is exactly what it is not. */
    port_storage_init();
    port_codeplug_init();

    /* Give the K1 its own EEPROM image in the erased band (port_storage.c); it
     * imports from the stock on first use.  Do it before the calibration load
     * and the attribute cache, which both read it. */
    port_storage_import_k1();

    /* The K1's channel-attribute cache marks an unused slot with
     * channel_id == 0xFFFF, so it has to be initialised before the first lookup
     * -- otherwise a lookup of channel 0 hits the zeroed slot and is told the
     * channel has band 0 and no scan lists.  misc.c has the function; its own
     * comment says it belongs in this boot sequence. */
    MR_InitChannelAttributesCache();

    /* The stock's shared settings next: they are what a radio configured by
     * the stock firmware or its CPS already carries. */
    port_codeplug_shared_settings();

    /* Then the port's own blob, which wins where the two overlap: it is what
     * the user last set with this firmware. */
    if (port_storage_load_settings()) {
        port_settings_extra_t extra;

        memset(&extra, 0, sizeof extra);
        if (port_storage_get_extra(&extra, sizeof extra) &&
            extra.magic == PORT_EXTRA_MAGIC &&
            extra.version == PORT_EXTRA_VERSION) {
            port_codeplug_freq_restore(extra.freq_channels, sizeof extra.freq_channels);
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
}

void SETTINGS_LoadCalibration(void)
{
    /* The K1's calibration image now exists in the port's own store at its
     * native addresses (port_storage.c imports it from the stock's 0x3000
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
    ra89r_codeplug_record_t record;

    if (IS_MR_CHANNEL(channel)) {
        if (!port_codeplug_read(channel, &record))
            return 0u;
        if (record.rx_frequency == 0xFFFFFFFFu)
            return 0u;
        return record.rx_frequency;
    }

    if (IS_FREQ_CHANNEL(channel)) {
        ChannelScanDisplayInfo_t info;

        if (!port_codeplug_freq_get(channel, gEeprom.RX_VFO, &info))
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

    if (IS_MR_CHANNEL(channel))
        return port_codeplug_scan_info(channel, info);

    if (IS_FREQ_CHANNEL(channel))
        return port_codeplug_freq_get(channel, gEeprom.RX_VFO, info);

    return false;
}

void SETTINGS_FetchChannelName(char *s, const uint16_t channel)
{
    if (s == 0)
        return;

    s[0] = 0;

    if (!IS_MR_CHANNEL(channel))
        return;

    port_codeplug_name(s, 16u, channel);
}

/* ---------------------------------------------------------------------------
 * Writing.  The stock's regions stay untouched; the port's own state goes to
 * its blob.  A channel the user edits is a write into the stock's codeplug and
 * is not implemented yet (docs/ra89r_codeplug.md, "Writing").
 * ------------------------------------------------------------------------- */

/* A deferred save has been asked for (see SETTINGS_SaveVfoIndices). */
static bool settings_dirty;

static bool settings_save_all(void)
{
    port_settings_extra_t extra;
    memset(&extra, 0, sizeof extra);
    extra.magic = PORT_EXTRA_MAGIC;
    extra.version = PORT_EXTRA_VERSION;
    port_codeplug_freq_snapshot(extra.freq_channels, sizeof extra.freq_channels);

    if (!port_storage_set_extra(&extra, sizeof extra))
        return false;

    return port_storage_save_settings();
}

void SETTINGS_SaveSettings(void)
{
    /* A menu change: write now, and there is nothing left pending afterwards. */
    settings_dirty = false;
    (void)settings_save_all();
}

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
    (void)Channel;
    (void)VFO;
    (void)pVFO;
    (void)Mode;
    /* A memory channel lives in the stock's records, which this firmware does
     * not write yet.  A frequency channel is the port's own, so it can. */
    if (IS_FREQ_CHANNEL(Channel) && pVFO != 0) {
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
        port_codeplug_freq_set(Channel, gEeprom.TX_VFO, &info);
    }

    (void)settings_save_all();
}

void SETTINGS_SaveChannelName(uint16_t channel, const char *name)
{
    (void)channel;
    (void)name;
    /* The stock's channel names are the codeplug's; writing one is a write into
     * a stock region, which waits for the journal work. */
}

void SETTINGS_UpdateChannel(uint16_t channel, const VFO_Info_t *pVFO, bool keep)
{
    SETTINGS_SaveChannel(channel, gEeprom.TX_VFO, pVFO, keep ? 1u : 0u);
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
