/* The bit-banged 3-wire bus shared by the two BK481x RF transceivers.
 *
 *   PA12  clock  (`BK_SCL_PIN`)  -- idles low
 *   PB12  data   (`BK_SDA_PIN`)  -- driven to send, released to read
 *   PB8   select BK4829 (`BK4829_CS_PIN`)
 *   PB13  select BK4815 (`BK4815_CS_PIN`)
 *
 * All four are on GPIOB except the clock, which is on GPIOA; both selects are
 * active low and are driven by hand.  This is where "which pin" stops and
 * "which part" begins: this layer knows nothing about either chip's register
 * framing, so the register layers above it (`bk4829.c`, `bk4815.c`) compile and
 * run on a PC against a recording stub -- see `tools/test_rf.c`.
 *
 * Timing follows the stock rather than a datasheet guess (ra89r_rfpath.md):
 * a byte goes out with the data set while the clock is low and the rising edge
 * latching it (stock `FUN_08017D6C`), and 16 bits come in sampled while the
 * clock is low, followed by the rise (stock `FUN_08017FE4`) -- note the
 * opposite phase, which is easy to get wrong.
 */
#ifndef DRIVER_RF_BUS_H
#define DRIVER_RF_BUS_H

#include <stdint.h>

/* Bring up the bus pins and park both selects high with the clock low. */
void rf_bus_init(void);

/* Select `cs`, send one address byte, then `len` bytes from `data`, and release
 * the select.  `cs` is one of the `*_CS_PIN` masks in board_pins.h. */
void rf_bus_write(uint32_t cs, uint8_t addr, const uint8_t *data, unsigned len);

/* Select `cs`, send one address byte, release the data line and clock 16 bits
 * in.  Returns the word the part shifted out. */
uint16_t rf_bus_read(uint32_t cs, uint8_t addr);

#endif /* DRIVER_RF_BUS_H */
