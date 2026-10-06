/* BK4815 receive service.
 *
 * The BK4815 is the second transceiver on the shared bus (see `bk4815.h`).
 * This is the narrow receive path the dual-RF coordinator needs: tune the chip
 * to a frequency and read its RSSI.  The register sequence is the stock's own
 * (`FUN_0801703c`), transcribed; the transmit path is not implemented here.
 *
 * Device-header free so it can be host-tested against the framing driver.
 */
#ifndef DRIVER_BK4815_RX_H
#define DRIVER_BK4815_RX_H

#include <stdint.h>

/* Tune the BK4815 to `freq_10hz` (10 Hz units) and put it in receive
 * (register 0x70 = 0xA000).  The VCO divider and the per-band calibration word
 * are derived from the frequency exactly as the stock's `FUN_0801703c` does. */
void bk4815_rx_tune(uint32_t freq_10hz);

/* The chip's RSSI indicator: register 0x44, bits 6:0. */
uint16_t bk4815_rx_meter(void);

#endif /* DRIVER_BK4815_RX_H */
