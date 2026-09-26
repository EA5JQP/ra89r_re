/* The port's application state facade (see ra89r_port.md, stage 2).
 *
 * Everything the imported K1 screens read: the VFO objects their pointers point
 * at, the runtime flags, and stubs for the modules the port has not brought in
 * yet.  Each stub names the K1 module that owns it, so replacing them one by
 * one is mechanical.
 */
#include <string.h>

#include "app/chFrScanner.h"
#include "app/dtmf.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "driver/py25q16.h"
#include "app/common.h"
#include "app/scanner.h"
#include "audio.h"
#include "radio.h"
#include "ui/ui.h"
#include "version.h"
#include "settings.h"

VFO_Info_t *gTxVfo;
VFO_Info_t *gRxVfo;
VFO_Info_t *gCurrentVfo;

void port_state_init(void)
{
    unsigned i;

    PORT_SettingsDefaults();

    /* The screens read gEeprom.VfoInfo[] directly, and the pointers alias it. */
    for (i = 0; i < 2; i++) {
        VFO_Info_t *vfo = &gEeprom.VfoInfo[i];

        memset(vfo, 0, sizeof *vfo);
        vfo->pRX = &vfo->freq_config_RX;
        vfo->pTX = &vfo->freq_config_TX;
        vfo->freq_config_RX.Frequency = 14575000u;
        vfo->freq_config_TX.Frequency = 14575000u;
        vfo->CHANNEL_BANDWIDTH = 0;
        vfo->OUTPUT_POWER = 2;
        vfo->STEP_SETTING = STEP_12_5kHz;
        vfo->StepFrequency = 1250;
    }

    gTxVfo = &gEeprom.VfoInfo[gEeprom.TX_VFO];
    gRxVfo = &gEeprom.VfoInfo[gEeprom.RX_VFO];
    gCurrentVfo = gRxVfo;
}

/* ---------------------------------------------------------------------------
 * Globals the ported screens read.  Each is owned by a K1 module the port has
 * not brought in yet; the comment names it so the real definition can replace
 * this one.  Defaults are chosen to look sane on screen, not to mimic a radio.
 * ------------------------------------------------------------------------- */

FUNCTION_Type_t gCurrentFunction;                 /* functions.c */
bool            gMonitor;                         /* misc.c      */
bool            gRxVfoIsActive;                   /* misc.c      */
uint8_t         gVFO_RSSI_bar_level[2];           /* misc.c      */
char            gListName[MR_CHANNELS_LIST][4];   /* misc.c      */
int8_t          gScanStateDir;                    /* app/chFrScanner.c */
uint8_t         gSetting_set_pwr = 2;             /* settings.c  */
bool            gSetting_set_gui = true;          /* settings.c  */
bool            gSetting_live_DTMF_decoder;       /* settings.c  */
VfoState_t      VfoState[2];                      /* radio.c     */

const char gModulationStr[MODULATION_UKNOWN][4] = { "FM", "AM", "USB" };

char   gDTMF_InputBox[15];                        /* app/dtmf.c  */
bool   gDTMF_InputMode;                           /* app/dtmf.c  */
char   gDTMF_RX_live[20];                         /* app/dtmf.c  */

/* ---------------------------------------------------------------------------
 * Functions the ported screens call.
 * ------------------------------------------------------------------------- */

bool FUNCTION_IsRx(void)
{
    return true;
}

ChannelAttributes_t *MR_GetChannelAttributes(uint16_t channel_id)
{
    static ChannelAttributes_t attributes;

    (void)channel_id;
    return &attributes;
}

void SETTINGS_FetchChannelName(char *s, const uint16_t channel)
{
    (void)channel;
    s[0] = 0;
}

/* The menu screen's own decorations, from misc.c.  gMicGain_dB2 are the K1's
 * real values (misc.c): the dB/2 steps for the mic gain menu. */
uint8_t          gMenuListCount;                  /* misc.c */
const uint8_t    gMicGain_dB2[9] = { 3, 8, 16, 24, 32, 40, 48, 56, 63 };

bool RADIO_CheckValidChannel(uint16_t channel, bool checkScanList, uint8_t scanList)
{
    /* radio.c: without a codeplug every channel is "valid"; the real check
     * reads the codeplug once the storage layer is in. */
    (void)channel;
    (void)checkScanList;
    (void)scanList;
    return true;
}

uint32_t SETTINGS_FetchChannelFrequency(const uint16_t channel)
{
    /* settings.c: the real one reads the codeplug.  Until then the menu shows
     * the frequency the VFO is on. */
    (void)channel;
    return gRxVfo != 0 ? gRxVfo->freq_config_RX.Frequency : 0u;
}

/* ---------------------------------------------------------------------------
 * Menu actions.  The menu table (app/menu.c) points at these; the port has no
 * RF/scanner/DTMF engine yet, so they are inert.  Owners, in order of the
 * list: functions.c (APP_*, FUNCTION_NOP), driver/bk4819.c (the BK4819 calls
 * -- those two live in the real driver, App/driver/bk4819.c, and only the
 * host preview needs a stand-in for them: tools/host/host_hw.c),
 * app/chFrScanner.c + app/scanner.c, misc.c (COMMON_*), app/dtmf.c,
 * radio.c and ui/ui.c.
 * ------------------------------------------------------------------------- */

void APP_StartListening(FUNCTION_Type_t function) { (void)function; }
void FUNCTION_NOP(void) { }
void COMMON_KeypadLockToggle(void) { }
void COMMON_SwitchVFOMode(void) { }
void COMMON_SwitchVFOs(void) { }
void DTMF_clear_input_box_memory(void) { }
void DTMF_clear_input_box(void) { }   /* app/dtmf.c */
bool SCANNER_IsScanning(void) { return false; }
void CHFRSCANNER_ManualResume(const int8_t scan_direction) { (void)scan_direction; }
void CHFRSCANNER_Start(const bool storeBackupSettings, const int8_t scan_direction)
{
    (void)storeBackupSettings;
    (void)scan_direction;
}
void CHFRSCANNER_Stop(void) { }
void RADIO_NextValidList(int8_t direction) { (void)direction; }
void RADIO_SelectVfos(void) { }
void RADIO_SetupRegisters(bool switchToForeground) { (void)switchToForeground; }
BK4819_FilterBandwidth_t RADIO_GetAMFilterBandwidth(const VFO_Info_t *pVfo)
{
    (void)pVfo;
    return BK4819_FILTER_BW_AM;
}
bool             gCssBackgroundScan;              /* misc.c  */
uint8_t          gDTMF_RX_live_timeout;           /* app/dtmf.c */
bool             gDualWatchActive;                /* radio.c */
bool             gFlagReconfigureVfos;            /* radio.c */
bool             gMute;                           /* audio.c */
uint16_t         gNextMrChannel;                  /* radio.c */
uint16_t         gRequestSaveChannel;             /* misc.c  */
bool             gRequestSaveSettings;            /* misc.c  */
bool             gSaveRxMode;                     /* radio.c */
volatile uint16_t gScanPauseDelayIn_10ms;         /* app/scanner.c */
bool             gScanPauseMode;                  /* app/scanner.c */
volatile bool    gScheduleScanListen;             /* app/scanner.c */
bool             gSetting_350EN;                  /* settings.c */
uint8_t          gSetting_F_LOCK;                 /* settings.c */
bool             gF_LOCK;                         /* misc.c */
bool             gUpdateDisplay = true;           /* ui/ui.c */
bool             gSetting_set_ptt_session;        /* settings.c */
uint8_t          gUpdateStatus;                   /* misc.c */
const uint16_t   scan_pause_delay_in_1_10ms = 0;  /* app/scanner.c */

/* ui/scanner.c owns this; the scanner screen is not ported yet. */
void UI_DisplayScanner(void) { }

/* ---------------------------------------------------------------------------
 * The menu's settings, scanner and key-action surface.  Same rule as above:
 * every one of these is owned by a K1 module the port has not brought in yet,
 * and each is inert or a faithful copy of the small pure ones.
 * Owners: settings.c (SETTINGS_*, gSetting_*), app/scanner.c (SCANNER_*),
 * app/generic.c (GENERIC_Key_*), misc.c (NUMBER_AddWithWraparound, StrToUL,
 * gMenuCountdown, menu_timeout_*), audio.c/driver/beeper (gBeepToPlay),
 * radio.c (RADIO_FindNextChannel, gVfoConfigureMode, gFlagResetVfos),
 * functions.c (gFlagAcceptSetting, gFlagRefreshSetting, gPttWasReleased),
 * driver/backlight.c (gBackLight), misc.c (the remaining g* state).
 * ------------------------------------------------------------------------- */

bool             gBackLight = true;               /* driver/backlight.c */
BEEP_Type_t      gBeepToPlay;                     /* audio.c / beeper */
uint8_t          gCB;                             /* misc.c */
uint8_t          gDW;                             /* misc.c */
bool             gFlagAcceptSetting;              /* functions.c */
bool             gFlagRefreshSetting;             /* functions.c */
bool             gFlagResetVfos;                  /* radio.c */
uint8_t          gKeyLockCountdown;                /* misc.c */
uint16_t         gMenuCountdown;                  /* misc.c */
bool             gPttWasReleased;                 /* functions.c */
enum BacklightOnRxTx_t gSetting_backlight_on_tx_rx;  /* settings.c */
uint8_t          gSetting_battery_text;           /* settings.c */
uint8_t          gSetting_set_eot;                /* settings.c */
bool             gSetting_set_inv;                /* settings.c */
uint8_t          gSetting_set_lck;                /* settings.c */
bool             gSetting_set_met;                /* settings.c */
bool             gSetting_set_ptt;                /* settings.c */
bool             gSetting_set_tmr;                /* settings.c */
uint8_t          gSetting_set_tot;                /* settings.c */
uint8_t          gSquelchLevelOriginal;           /* settings.c */
uint8_t          gVfoConfigureMode;               /* radio.c */
const uint8_t    menu_timeout_500ms = 0;          /* misc.c */
const uint16_t   menu_timeout_long_500ms = 0;     /* misc.c */

void GENERIC_Key_F(bool bKeyPressed, bool bKeyHeld)
{
    (void)bKeyPressed;
    (void)bKeyHeld;
}

void GENERIC_Key_PTT(bool bKeyPressed)
{
    (void)bKeyPressed;
}

void SCANNER_Start(bool singleFreq)
{
    (void)singleFreq;
}

void SCANNER_Stop(void) { }

void SETTINGS_FactoryReset(bool bIsAll)
{
    (void)bIsAll;
}

void SETTINGS_LoadCalibration(void) { }

void SETTINGS_ResetTxLock(void) { }

void SETTINGS_SaveBatteryCalibration(const uint16_t *batteryCalibration)
{
    (void)batteryCalibration;
}

void SETTINGS_SaveChannelName(uint16_t channel, const char *name)
{
    (void)channel;
    (void)name;
}

void SETTINGS_UpdateChannel(uint16_t channel, const VFO_Info_t *pVFO, bool keep)
{
    (void)channel;
    (void)pVFO;
    (void)keep;
}

uint16_t RADIO_FindNextChannel(uint16_t ChNum, int8_t Direction, bool bCheckScanList,
                               uint8_t RadioNum)
{
    (void)Direction;
    (void)bCheckScanList;
    (void)RadioNum;
    return ChNum;
}

/* The two the port can have outright: both are pure. */
int32_t NUMBER_AddWithWraparound(int32_t Base, int32_t Add, int32_t LowerLimit,
                                 int32_t UpperLimit)
{
    Base += Add;

    if (Base == 0x7fffffff || Base < LowerLimit)
        return UpperLimit;
    if (Base > UpperLimit)
        return LowerLimit;
    return Base;
}

unsigned long StrToUL(const char *str)
{
    unsigned long value = 0;

    while (*str >= '0' && *str <= '9')
        value = (value * 10u) + (unsigned long)(*str++ - '0');
    return value;
}

/* The status line and welcome screen's remaining dependencies.  APP_* is
 * functions.c/app.c, gAirCopyBootMode is misc.c (air copy is not ported), and
 * UI_DrawBattery is ui/status.c's own -- it is only listed here because the
 * welcome screen references it through a feature guard. */
bool APP_IsScreenSaverDisplayed(void) { return false; }
bool gAirCopyBootMode;
void UI_DrawBattery(uint8_t *bitmap, uint8_t level, uint8_t blink)
{
    (void)bitmap;
    (void)level;
    (void)blink;
}

/* ---------------------------------------------------------------------------
 * The external SPI NOR flash -- the codeplug.  driver/py25q16.c (and this
 * repo's own driver/eeprom, still on its unmerged branch) own this; until one
 * of them is in the port, every read returns an empty part: the boot messages
 * read as empty and the menus show their compiled-in defaults.  That is the
 * seam the storage stage replaces (see ra89r_eeprom.md and ra89r_port.md).
 * ------------------------------------------------------------------------- */
void PY25Q16_ReadBuffer(uint32_t address, void *pBuffer, uint32_t size)
{
    (void)address;
    if (pBuffer != 0)
        memset(pBuffer, 0, size);
}
