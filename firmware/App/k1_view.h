/* The K1 (F4HWN v6.2.0) virtual EEPROM, as CHIRP's F4HWN driver addresses it
 * (0x0000-0xB18F), mapped onto where this port really keeps each piece.
 *
 *   0x0000-0x3FFF  channel records      -> K1 image, K1_IMAGE_CH_BASE
 *   0x4000-0x886D  names, attrs, lists  -> K1 image, same address
 *   0x9000-0x90DF  frequency channels   -> cp_freq (settings blob)
 *   0xA000-0xA177  settings             -> gEeprom + gSetting_* (settings blob),
 *                  except 0xA0C8-0xA0E7 (boot message) and 0xA148-0xA14F
 *                  (spectrum), which the K1 code here reads at those
 *                  physical addresses and so pass straight through
 *   0xB000-0xB18F  calibration          -> K1 image 0x10000, read-only
 *   everything else                     -> reads 0xFF, writes dropped
 *
 * The stock codeplug (physical 0x0000-0x3FFF, calibration 0x3000-0x38AF) is
 * not reachable from any virtual address.  See docs/ra89r_chirp.md.
 */
#ifndef APP_K1_VIEW_H
#define APP_K1_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#define K1VIEW_SETTINGS_BASE 0xA000u
#define K1VIEW_SETTINGS_SIZE 0x0178u
#define K1VIEW_VFO_BASE      0x9000u
#define K1VIEW_VFO_SIZE      0x00E8u
#define K1VIEW_CAL_BASE      0xB000u
#define K1VIEW_CAL_SIZE      0x0190u
#define K1VIEW_CAL_PHYS      0x10000u
#define K1VIEW_ATTR_BASE     0x8000u   /* (1024 + 7) x 2 B channel attributes */
#define K1VIEW_ATTR_END      0x880Eu

/* The gSetting_* globals the settings blob carries as their K1 bytes (see
 * k1_view_globals_pack): 0xA000[0], 0xA00E, 0xA0B9-0xA0BF, 0xA150-0xA15F. */
#define K1VIEW_GLOBALS_SIZE  25u

typedef struct {
    uint32_t written;      /* blocks that changed flash */
    uint32_t unchanged;    /* blocks equal to what was there (skipped) */
    uint32_t dropped;      /* bytes aimed at holes / calibration */
    bool     settings_dirty;
    bool     vfo_dirty;
} k1_view_stats_t;

/* A programming session started: snapshot the state into the shadows. */
void k1_view_open(void);
void k1_view_read(uint16_t addr, uint8_t *buf, uint8_t len);
void k1_view_write(uint16_t addr, const uint8_t *buf, uint8_t len);
/* Apply what the session wrote into the settings/VFO shadows and save the
 * blob.  Returns true when something was saved.  Channel, name and attribute
 * writes went to flash already. */
bool k1_view_commit(void);
const k1_view_stats_t *k1_view_stats(void);

/* The K1 settings bytes 0xA000-0xA177 from / into the running state. */
void k1_settings_pack(uint8_t out[K1VIEW_SETTINGS_SIZE]);
void k1_settings_unpack(const uint8_t in[K1VIEW_SETTINGS_SIZE]);

/* The gSetting_* subset the settings blob persists (settings.c, extra v6). */
void k1_view_globals_pack(uint8_t out[K1VIEW_GLOBALS_SIZE]);
void k1_view_globals_unpack(const uint8_t in[K1VIEW_GLOBALS_SIZE]);

#endif
