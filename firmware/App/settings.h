/* STOPGAP settings header for the port (NOT the K1's settings.h).
 *
 * The imported drawing helpers (ui/helper.c) touch exactly one setting,
 * gEeprom.KEY_LOCK, so this header exists to let them compile before the real
 * state model lands.  The K1's own settings.h (346 lines, with the codeplug
 * struct and its enums) replaces this file in port stage 2, together with the
 * gEeprom storage behind it -- see ra89r_port.md.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

typedef struct {
    uint8_t KEY_LOCK;
} EEPROM_Config_t;

extern EEPROM_Config_t gEeprom;

#endif
