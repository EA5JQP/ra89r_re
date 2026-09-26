/* The port's application state facade (see ra89r_port.md, stage 2).
 *
 * Everything the imported K1 screens read: the VFO objects their pointers point
 * at, the runtime flags, and the boot order the K1's own main() performs.
 * The settings and the codeplug live in settings.c / port_codeplug.c; this file
 * only puts them together and keeps the VFO self-pointers valid.
 */
#include <string.h>

#include "misc.h"
#include "port_codeplug.h"
#include "radio.h"
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

/* The K1's own boot order (App/main.c): settings, calibration, then the two
 * VFOs from the codeplug and the pointer swap that publishes them.  Without
 * this the screens would show whatever the defaults left in gEeprom. */
void port_state_init(void)
{
    SETTINGS_InitEEPROM();
    SETTINGS_LoadCalibration();

    RADIO_ConfigureChannel(0, VFO_CONFIGURE_RELOAD);
    RADIO_ConfigureChannel(1, VFO_CONFIGURE_RELOAD);

    RADIO_SelectVfos();

    port_state_fixup_vfo();
}
