/* Storage: the external SPI NOR flash behind the K1's PY25Q16 interface, and
 * the port's settings blob on it (see port_storage.c and ra89r_eeprom.md). */
#ifndef APP_PORT_STORAGE_H
#define APP_PORT_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

void port_storage_init(void);
uint32_t port_storage_size(void);
bool port_storage_id(uint16_t *man_dev, uint32_t *jedec);

/* Read the port's blob back into gEeprom.  False = no valid blob, gEeprom is
 * left alone. */
bool port_storage_load_settings(void);
/* Erase + program the blob, then read it back.  False = the program did not
 * take. */
bool port_storage_save_settings(void);

/* The pending write test (erase/program/read-back on a scratch sector).  False
 * reports the first differing address in *bad_offset. */
bool port_storage_write_test(uint32_t *bad_offset);

#endif /* APP_PORT_STORAGE_H */
