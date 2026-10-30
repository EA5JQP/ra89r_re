/* The spectrum's chip policy (see docs/ra89r_spectrum.md).
 *
 * The spectrum sweeps on the BK4829 or on the BK4815.  There is no "both": the
 * two chips share one set of 0x33 receive-path pins, and the chip they are not
 * set for hears next to nothing (radio 2026-10-28, docs/ra89r_scan.md -- the
 * reason the scanner's FAST BOTH was removed).  This module owns the
 * host-testable decisions: how the BK4815's 7-bit RSSI is normalized into the
 * BK4829's scale, and how the setting is stored in the
 * spectrum's EEPROM byte.
 *
 * Device-header free and driver-free on purpose, so it links on a PC
 * (tools/test_spectrum.c).  The RF calls themselves live in app/spectrum.c,
 * which already includes the chip drivers.
 */
#ifndef APP_SPECTRUM_RF_H
#define APP_SPECTRUM_RF_H

#include <stdint.h>

typedef enum {
    SPECTRUM_CHIP_4829 = 0,   /* sweep, peak and listen on the BK4829 */
    SPECTRUM_CHIP_4815 = 1    /* sweep, peak and listen on the BK4815 */
} spectrum_chip_t;

/* The other chip: what the in-screen toggle selects next. */
spectrum_chip_t spectrum_rf_next_chip(spectrum_chip_t chip);

/* Normalize a chip's raw RSSI into the BK4829's 9-bit scale: the BK4815's 7-bit
 * 0x44 value is masked and multiplied by 4 (127 * 4 = 508), the same scaling
 * the FAST scanner's BK4815 lane uses; the BK4829's is unchanged. */
uint16_t spectrum_rf_normalize_rssi(spectrum_chip_t chip, uint16_t raw);

/* The persisted byte for the setting (the spectrum's Data[4]); an out-of-range
 * chip encodes to 0 and any byte but 1 decodes to BK4829 -- including 2, the
 * removed "Both" setting, and 0xFF, an erased store. */
uint8_t         spectrum_rf_encode_chip(spectrum_chip_t chip);
spectrum_chip_t spectrum_rf_decode_chip(uint8_t byte);

#endif
