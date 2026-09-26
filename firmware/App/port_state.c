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
#include "app/common.h"
#include "app/scanner.h"
#include "audio.h"
#include "port_storage.h"
#include "radio.h"
#include "ui/ui.h"
#include "version.h"
#include "settings.h"


/* The VFO objects live inside gEeprom and carry pointers into themselves, so
 * anything that replaces or clears gEeprom -- the defaults, or a blob read back
 * from flash -- has to re-establish them before the screens dereference them. */
void port_state_fixup_vfo(void)
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

void port_state_init(void)
{
    unsigned i;

    PORT_SettingsDefaults();

    /* If the port saved its settings on the external flash, they win over the
     * defaults.  (The stock codeplug itself is a different format and is not
     * mapped in yet -- see port_storage.c.) */
    port_storage_init();
    (void)port_storage_load_settings();

    /* The screens read gEeprom.VfoInfo[] directly, and the pointers alias it. */
    for (i = 0; i < 2; i++) {
        VFO_Info_t *vfo = &gEeprom.VfoInfo[i];

        memset(vfo, 0, sizeof *vfo);
        vfo->pRX = &vfo->freq_config_RX;
        vfo->pTX = &vfo->freq_config_TX;
        /* Placeholder channels until the stock codeplug is mapped in: VFO A on
         * 145.7500 and VFO B on 145.5000, so the double-channel screen shows
         * two different rows. */
        vfo->freq_config_RX.Frequency = (i == 0) ? 14575000u : 14550000u;
        vfo->freq_config_TX.Frequency = vfo->freq_config_RX.Frequency;
        vfo->CHANNEL_BANDWIDTH = 0;
        vfo->OUTPUT_POWER = 2;
        vfo->STEP_SETTING = STEP_12_5kHz;
        vfo->StepFrequency = 1250;
    }

    port_state_fixup_vfo();
}

/* ---------------------------------------------------------------------------
 * Globals the ported screens read.  Each is owned by a K1 module the port has
 * not brought in yet; the comment names it so the real definition can replace
 * this one.  Defaults are chosen to look sane on screen, not to mimic a radio.
 * ------------------------------------------------------------------------- */

int8_t          gScanStateDir;                    /* app/chFrScanner.c */


char   gDTMF_InputBox[15];                        /* app/dtmf.c  */
bool   gDTMF_InputMode;                           /* app/dtmf.c  */
char   gDTMF_RX_live[20];                         /* app/dtmf.c  */

/* ---------------------------------------------------------------------------
 * Functions the ported screens call.
 * ------------------------------------------------------------------------- */



void SETTINGS_FetchChannelName(char *s, const uint16_t channel)
{
    (void)channel;
    s[0] = 0;
}

/* The menu screen's own decorations, from misc.c.  gMicGain_dB2 are the K1's
 * real values (misc.c): the dB/2 steps for the mic gain menu. */


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
uint8_t          gDTMF_RX_live_timeout;           /* app/dtmf.c */
bool             gScanPauseMode;                  /* app/scanner.c */

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


/* The two the port can have outright: both are pure. */


/* The status line and welcome screen's remaining dependencies.  APP_* is
 * functions.c/app.c, gAirCopyBootMode is misc.c (air copy is not ported), and
 * UI_DrawBattery is ui/status.c's own -- it is only listed here because the
 * welcome screen references it through a feature guard. */
bool APP_IsScreenSaverDisplayed(void) { return false; }
void UI_DrawBattery(uint8_t *bitmap, uint8_t level, uint8_t blink)
{
    (void)bitmap;
    (void)level;
    (void)blink;
}

/* ---------------------------------------------------------------------------
 * Step-3 modules: the scanner, DTMF and the common key actions.  Ported later
 * (ra89r_port.md), stubbed here so the app core links -- owners in order:
 * app/scanner.c, app/chFrScanner.c, app/dtmf.c, app/common.c.
 * ------------------------------------------------------------------------- */
bool SCANNER_IsScanning(void) { return false; }
void CHFRSCANNER_ManualResume(const int8_t scan_direction) { (void)scan_direction; }
void CHFRSCANNER_Start(const bool storeBackupSettings, const int8_t scan_direction)
{
    (void)storeBackupSettings;
    (void)scan_direction;
}
void DTMF_clear_input_box(void) { }
void DTMF_clear_input_box_memory(void) { }
void DTMF_Reply(void) { }
void COMMON_KeypadLockToggle(void) { }
void COMMON_SwitchVFOMode(void) { }
void COMMON_SwitchVFOs(void) { }
void SCANNER_Stop(void) { }
void CHFRSCANNER_Stop(void) { }
