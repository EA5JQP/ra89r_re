/* STOPGAP definitions for the port (see settings.h).
 *
 * gEeprom is all-defaults for now; it becomes the external-NOR-backed codeplug
 * in port stage 2.  gKeypadLocked lives in the K1's misc.c, which the port has
 * not brought in yet, so it is defined here until it does.
 */
#include "settings.h"

EEPROM_Config_t gEeprom;

uint8_t gKeypadLocked;
