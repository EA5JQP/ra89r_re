/* BK4815 RF transceiver -- identity, framing and the stock's boot setup.
 *
 * This is the second of the two BK481x parts on the shared 3-wire bus: chip
 * select `PB13` (`BK4815_CS_PIN`), clock `PA12` and data `PB12` shared with the
 * BK4829.  The bus itself is `rf_bus.c`; the BK4829's own framing is in
 * `bk4829.h`.
 *
 * Framing differs from the BK4829's, and that is the easy thing to get wrong:
 * the address byte carries the register shifted left with the read flag in
 * bit 0 --
 *
 *     write:  (reg & 0x7f) << 1
 *     read:  ((reg & 0x7f) << 1) | 1
 *
 * -- where the BK4829 sends the register unshifted with the read flag in bit 7.
 * Both then transfer one 16-bit word MSB first.
 *
 * Evidence (stock image, see ra89r_bk4815.md):
 *   0x08021F78  write core: `(reg & 0x7f) << 1`, then len bytes, select PB13
 *   0x08018060  read:       `((reg & 0x7f) << 1) | 1`, then 16 bits in
 *   0x08009758  detect:     register 0 must read 0x4816
 *   0x08006A0C  boot init:  a 36-byte block into registers 2..19, then 33 writes
 */
#ifndef DRIVER_BK4815_H
#define DRIVER_BK4815_H

#include <stdint.h>
#include <stdbool.h>

/* Register 0 is the chip id: the stock's detect reads it and expects 0x4816. */
#define BK4815_REG_ID 0x00u
#define BK4815_ID     0x4816u

/* True when register 0 reads back the BK4815 id. */
bool bk4815_detect(void);

/* One register access, in this chip's own framing. */
uint16_t bk4815_read_reg(uint8_t reg);
void bk4815_write_reg(uint8_t reg, uint16_t value);

/* Write `len` bytes starting at one register address, which the part takes as a
 * block under a single select pulse -- how the stock loads the table below. */
void bk4815_write_block(uint8_t reg, const uint8_t *bytes, unsigned len);

/* Replay the stock's boot configuration (FUN_08006A0C) in its original order. */
void bk4815_configure(void);

/* Register writes the boot configuration consists of: 1 block plus the singles. */
unsigned bk4815_config_writes(void);

/* How many of those singles the stock takes from its own RAM (calibration) and
 * this firmware therefore cannot reproduce -- it sends 0 for them. */
unsigned bk4815_ram_sourced_writes(void);

/* The 36-byte block the stock sends to registers 2..19 (flash 0x08024E40). */
const uint8_t *bk4815_config_block(unsigned *len);

#endif /* DRIVER_BK4815_H */
