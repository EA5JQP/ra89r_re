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
#include "radio.h"
#include "ui/ui.h"
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
bool            gLowBattery;                      /* helper/battery.c */
bool            gLowBatteryConfirmed;             /* helper/battery.c */
uint8_t         gSetting_set_pwr = 2;             /* settings.c  */
bool            gSetting_set_gui = true;          /* settings.c  */
bool            gSetting_live_DTMF_decoder;       /* settings.c  */
VfoState_t      VfoState[2];                      /* radio.c     */
GUI_DisplayType_t gScreenToDisplay = DISPLAY_MAIN;/* ui/ui.c     */

const char gModulationStr[MODULATION_UKNOWN][4] = { "FM", "AM", "USB" };

/* Tone tables (dcs.c): only their presence is needed to draw, the codeplug
 * indexes them once the codeplug is readable. */
const uint16_t CTCSS_Options[50];
const uint16_t DCS_Options[104];

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

int32_t TX_freq_check(uint32_t Frequency)
{
    (void)Frequency;
    return 0;
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
