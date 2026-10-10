/* The spectrum's chip policy (see docs/ra89r_spectrum.md).
 *
 * The spectrum can sweep on the BK4829, on the BK4815, or on both (splitting the
 * range between them).  This module owns the host-testable decisions: which chip
 * measures a given swept step, how the BK4815's 7-bit RSSI is normalized into the
 * BK4829's scale, and how the setting is stored in the spectrum's EEPROM byte.
 *
 * Device-header free and driver-free on purpose, so it links on a PC
 * (tools/test_spectrum.c).  The RF calls themselves live in app/spectrum.c,
 * which already includes the chip drivers.
 */
#ifndef APP_SPECTRUM_RF_H
#define APP_SPECTRUM_RF_H

#include <stdint.h>

typedef enum {
    SPECTRUM_CHIP_4829 = 0,   /* every step on the BK4829 */
    SPECTRUM_CHIP_4815 = 1,   /* every step on the BK4815 */
    SPECTRUM_CHIP_BOTH = 2    /* alternate the chip per step */
} spectrum_chip_t;

/* Which chip measures step `index` for `setting`.  BOTH alternates: even index
 * -> BK4829, odd -> BK4815, so a sweep covers the range in about half the
 * BK4829 steps' worth of work. */
spectrum_chip_t spectrum_rf_step_chip(spectrum_chip_t setting, uint16_t index);

/* Normalize a chip's raw RSSI into the BK4829's 9-bit scale: the BK4815's 7-bit
 * value is multiplied by 4 (saturating at 0xFFFF); the BK4829's is unchanged.
 * Call with the already-resolved chip (never SPECTRUM_CHIP_BOTH). */
uint16_t spectrum_rf_normalize_rssi(spectrum_chip_t chip, uint16_t raw);

/* The persisted byte for the setting (the spectrum's Data[4]); an out-of-range
 * chip encodes to 0 and an out-of-range byte decodes to BK4829. */
uint8_t         spectrum_rf_encode_chip(spectrum_chip_t chip);
spectrum_chip_t spectrum_rf_decode_chip(uint8_t byte);

#endif
