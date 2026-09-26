/* The port's settings state.
 *
 * The K1 keeps its codeplug in `gEeprom` (EEPROM_Config_t, settings.h) and
 * reads it through SETTINGS_* accessors in its settings.c, backed by the
 * external SPI NOR flash.  The port has the external-NOR driver on an unmerged
 * branch (driver/eeprom) and its write test is still pending, so for now:
 *
 *   * gEeprom is a plain RAM object (definition here) filled by
 *     PORT_SettingsDefaults();
 *   * the accessors the screens call live in port_state.c with the rest of the
 *     facade, and are replaced by the K1's real settings.c once the storage
 *     backend is validated.
 *
 * gKeypadLocked lives in the K1's misc.c, which the port has not brought in
 * yet, so it is defined here until it does.
 */
#include <string.h>

#include "misc.h"
#include "settings.h"

EEPROM_Config_t gEeprom;

uint8_t gKeypadLocked;

/* Values that make the VFO screen show something sane before the codeplug is
 * readable: a channel (rather than a bare frequency), 145.7500 MHz, FM, the
 * stock squelch level and the stock display mode. */
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
    gEeprom.VOX_SWITCH = false;
    gEeprom.VOX_LEVEL = 5;
    gEeprom.BEEP_CONTROL = true;
    gEeprom.CHANNEL_DISPLAY_MODE = 0;
    gEeprom.TAIL_TONE_ELIMINATION = false;
    gEeprom.VFO_OPEN = true;
    /* The double-channel UI: ui/main.c's isMainOnly() is
     * (DUAL_WATCH == OFF && CROSS_BAND == OFF), so any other dual-watch mode
     * makes the main screen draw both VFOs.  Cross-band stays off: that is an
     * RF behaviour, and the port has no engine for it yet. */
    gEeprom.DUAL_WATCH = DUAL_WATCH_CHAN_A;
    gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;
    gEeprom.BATTERY_SAVE = 0;
    gEeprom.BACKLIGHT_TIME = 4;
    gEeprom.SCAN_RESUME_MODE = 0;
    gEeprom.SCAN_LIST_DEFAULT = 0;
    gEeprom.SCAN_LIST_ENABLED = false;
    gEeprom.CURRENT_STATE = 0;
    gEeprom.CURRENT_LIST = 0;
}
