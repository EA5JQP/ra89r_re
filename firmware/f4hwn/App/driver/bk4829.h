/* BK4829 RF transceiver -- register framing and the stock's boot setup.
 *
 * Two BK481x parts are fitted and share one bit-banged 3-wire bus, which lives
 * in `rf_bus.c`:
 *
 *   PA12  clock        (`BK_SCL_PIN`)
 *   PB12  data         (`BK_SDA_PIN`, output to send, released to read)
 *   PB8   chip select  BK4829 (`BK4829_CS_PIN`)
 *   PB13  chip select  BK4815 (`BK4815_CS_PIN`) -- see `bk4815.h`
 *
 * A BK4829 register access is: select low, one address byte whose bit 7 is the
 * read flag and whose bits 6..0 are the register, then one 16-bit word MSB
 * first.  That is what the stock's own primitives do --
 *
 *   0x08021FF4  write: clock low, select low, `reg & 0x7f`, two value bytes,
 *               select high
 *   0x080180F0  read: `reg | 0x80`, then 16 bits in
 *   0x08009772  detect: register 0 must read 0x4829, else the stock takes its
 *               error path and leaves the part unconfigured
 *
 * -- and the UV-K1/K5V3 `bk4829.c` bit-bang is the same shape, which is where
 * this driver's layout came from.  Note that the BK4815 frames differently: it
 * sends the register shifted left with the read flag in bit 0.
 *
 * The configuration is the stock's boot sequence (FUN_08006B78, 39 registers in
 * 40 writes) lifted from the image; docs/ra89r_bk4829.md has it, the one derived
 * value in it (register 0x7d) and the differences against the K1's own
 * BK4819/BK4829 sequences.  Nothing here has run on the radio yet.
 */
#ifndef DRIVER_BK4829_H
#define DRIVER_BK4829_H

#include <stdint.h>
#include <stdbool.h>

/* Register 0 is the chip id: the stock's detect reads it and expects 0x4829. */
#define BK4829_REG_ID 0x00u
#define BK4829_ID     0x4829u

/* Configure the bus pins and park the bus. */
void bk4829_init(void);

/* True when register 0 reads back the BK4829 id. */
bool bk4829_detect(void);

/* One register access.  `read_reg` shifts out `reg | 0x80` and reads 16 bits. */
uint16_t bk4829_read_reg(uint8_t reg);
void bk4829_write_reg(uint8_t reg, uint16_t value);

/* Replay the stock's boot configuration in its original order. */
void bk4829_configure(void);

/* How many register writes that is (for the console). */
unsigned bk4829_config_writes(void);

/* Read one table entry back out, so a caller can verify what was written
 * without keeping a second copy of the table.  `i` must be less than
 * bk4829_config_writes(). */
void bk4829_config_entry(unsigned i, uint8_t *reg, uint16_t *value);

#endif /* DRIVER_BK4829_H */
