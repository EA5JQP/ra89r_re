/* BK4829 RF transceiver -- the shared 3-wire bus and the stock's register setup.
 *
 * Two BK481x parts are fitted and share one bit-banged bus (see ra89r_rf.md):
 *
 *   PA12  clock        (`BK_SCL_PIN`)
 *   PB12  data         (`BK_SDA_PIN`, output to send, released to read)
 *   PB8   chip select  BK4829 (`BK4829_CS_PIN`)
 *   PB13  chip select  BK4815 (`BK4815_CS_PIN`, not driven by this driver yet)
 *
 * A register access is: select low, one byte whose bit 7 is the read flag and
 * whose bits 6..0 are the register, then one 16-bit word MSB first.  That is
 * what the stock's own primitives do --
 *
 *   0x08021FF4  write: clock low, select low, `reg & 0x7f`, two value bytes,
 *               select high
 *   0x080180F0  read: `reg | 0x80`, then 16 bits in
 *   0x08017D6C  shift a byte out over PB12 with PA12 as the clock
 *   0x08017FE4  shift 16 bits in
 *   0x0801DCF0  / 0x0801DCB8  switch PB12 between output and input
 *   0x08009772  detects: register 0 must read 0x4829
 *
 * -- and the UV-K1/K5V3 `bk4829.c` bit-bang is the same shape (SCL low, eight
 * bits MSB first with a short delay, then the word), which is where this
 * driver's layout comes from.
 *
 * The configuration is the stock's boot sequence verbatim (FUN_08006B78, 39
 * writes), lifted from the image; ra89r_rf.md has it and the differences
 * against the K1's own BK4819/BK4829 sequences.
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

#endif /* DRIVER_BK4829_H */
