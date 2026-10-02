/* Storage: the external SPI NOR flash behind the K1's PY25Q16 interface, and
 * the port's settings blob on it (see port_storage.c and docs/ra89r_eeprom.md). */
#ifndef APP_PORT_STORAGE_H
#define APP_PORT_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

void port_storage_init(void);
uint32_t port_storage_size(void);
bool port_storage_id(uint16_t *man_dev, uint32_t *jedec);

/* The blob is gEeprom plus a private payload for whatever the K1's struct has
 * no room for -- today the frequency (VFO) channels the stock has no place
 * for.  The payload is handed over in RAM so that saving the settings does not
 * also have to know what it means. */
#define PORT_STORAGE_EXTRA_MAX 240u

/* Read the port's blob back into gEeprom and its payload into the RAM copy.
 * False = no valid blob, gEeprom is left alone. */
bool port_storage_load_settings(void);
/* Erase + program the blob, then read it back.  False = the program did not
 * take. */
bool port_storage_save_settings(void);

/* The private payload.  Set copies into the RAM copy the next save writes;
 * get copies it back out after a load.  Both are bounded by
 * PORT_STORAGE_EXTRA_MAX and refuse anything larger. */
bool port_storage_set_extra(const void *data, uint32_t size);
bool port_storage_get_extra(void *data, uint32_t size);

/* The pending write test (erase/program/read-back on a scratch sector).  False
 * reports the first differing address in *bad_offset. */
bool port_storage_write_test(uint32_t *bad_offset);

/* The K1 application's own EEPROM image, rebuilt inside the RA89R's erased band
 * at the addresses the K1 code already uses (see docs/ra89r_calibration.md).
 * Only the channel-record base is relocated: the K1 puts records at 0, which is
 * the stock's codeplug, so they move to 0x9000.  Names (0x4000), attributes
 * (0x8000) and calibration (0x100C0) are already the K1's own addresses. */
#define K1_IMAGE_NAME_BASE 0x04000u
#define K1_IMAGE_ATTR_BASE 0x08000u
#define K1_IMAGE_CH_BASE   0x09000u
#define K1_IMAGE_CAL_BASE  0x100C0u
#define K1_IMAGE_CAL_SIZE  0xD0u        /* 0x100C0 .. 0x1018F */
#define K1_IMAGE_BASE      0x04000u
#define K1_IMAGE_END       0x20000u

/* True when [addr, addr+size) is a region the port may write: the K1 image band
 * or the tail blob.  Anything else is the stock's, and read-only here. */
bool port_storage_writable(uint32_t addr, uint32_t size);

/* One-time import of the stock's codeplug and calibration into the K1 image, so
 * the K1's own settings/misc/radio code has the EEPROM it expects.  It only
 * writes when the image is not already present, so it is safe to call at boot. */
void port_storage_import_k1(void);

#endif /* APP_PORT_STORAGE_H */
