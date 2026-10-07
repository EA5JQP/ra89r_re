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
 * Evidence (stock image, see docs/ra89r_bk4815.md):
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

/* The synthesizer/tone registers the stock's own routines touch, named against
 * the BK4815N datasheet's decimal register table (see docs/ra89r_bk4815.md):
 *   0x04 (4)    VCO-to-LO divider, bits 8:7 (0:8, 1:16, 2:12, 3:24)
 *   0x22 (34)   SELCALL tone frequency, (freq/18466)*65536
 *   0x43 (67)   FM demod: SNR indicator
 *   0x44 (68)   RSSI/SNR: RSSI indicator (bits 6:0)
 *   0x70 (112)  operation control, 0xA000 RX / 0xE000 TX
 *   0x71 (113)  high 16 bits of the channel frequency word
 *   0x72 (114)  low 16 bits of the channel frequency word
 *   0x7E (126)  per-band calibration, written with 0x7F as one 32-bit block
 */
#define BK4815_REG_VCO       0x04u
#define BK4815_REG_TONE      0x22u
#define BK4815_REG_AF        0x49u   /* AF output: 0x1A02 off / 0x9A02 on */
#define BK4815_REG_OPCTRL    0x70u
#define BK4815_REG_FREQ_HI   0x71u
#define BK4815_REG_FREQ_LO   0x72u
#define BK4815_REG_CAL       0x7eu

/* Register 0x49's AF-enable values, from the stock's AF source switch
 * (`FUN_08015F48` enables with 0x9A02, `FUN_0801CCE8` restores 0x1A02). */
#define BK4815_AF_ON   0x9a02u
#define BK4815_AF_OFF  0x1a02u

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

/* Read one table entry back out, so a caller can verify what was written
 * without keeping a second copy of the table.  `i` must be less than
 * bk4815_config_writes(). */
void bk4815_config_entry(unsigned i, uint8_t *reg, uint16_t *value);

/* The 36-byte block the stock sends to registers 2..19 (flash 0x08024E40). */
const uint8_t *bk4815_config_block(unsigned *len);

/* ------------------------------------------------------------ synthesizer ---
 *
 * The BK4815N tunes through a fractional-N synthesizer, not through the
 * BK4829's 0x38/0x39 pair: the channel word goes to registers 0x71 (high 16)
 * and 0x72 (low 16), under the operation-control register 0x70 (0xA000 RX /
 * 0xE000 TX).  The stock's writer is FUN_0801703C, reached from the per-mode
 * configs FUN_08016CEC (RX) and FUN_080171D0 (TX); it computes
 *
 *     word = (freq_10Hz / 100000) * Ndiv * 2^24 / 26
 *          = freq_Hz * Ndiv * 2^24 / 26e6
 *
 * i.e. the datasheet's `Ndiv x fwanted / 26MHz x 2^24` with the 26 MHz crystal
 * reference, and writes the per-VCO-band divider Ndiv (8/12/16/24) into
 * register 4 bits 8:7.  The datasheet's RX form subtracts the IF; the stock's
 * writer does not, and neither does this one -- do not treat the word as
 * validated on the radio (docs/ra89r_bk4815.md).
 *
 * `freq_10hz` is the channel frequency in the codeplug's 10 Hz units, the same
 * value the BK4829 path feeds to its 0x38/0x39. */

/* VCO divider band, the stock's FUN_0800EC70: 3 for <= 187 MHz, 1 for
 * 187..270, 2 for 270..383, 0 above 383 MHz. */
uint8_t bk4815_vco_band(uint32_t freq_10hz);

/* The VCO-to-LO divider for a band: 8 / 16 / 12 / 24 for bands 0/1/2/3. */
uint8_t bk4815_vco_divider(uint8_t band);

/* The 32-bit fractional-N word the synthesizer takes at 0x71/0x72. */
uint32_t bk4815_frequency_word(uint32_t freq_10hz, uint8_t band);

/* The stock's FUN_08005C34 SELCALL/tone word: (tone_hz / 18466) * 65536,
 * written to register 0x22.  18466 is 0x4822, the datasheet's divisor. */
uint16_t bk4815_tone_word(uint32_t tone_hz);

/* Program the channel frequency: register 4 (VCO divider), then the 6-byte
 * operation-control + frequency block at 0x70, then the per-band calibration
 * block at 0x7E -- the three writes FUN_0801703C makes, in its order.  `tx`
 * picks 0xE000 (transmit) over 0xA000 (receive) in register 0x70. */
void bk4815_set_frequency(uint32_t freq_10hz, bool tx);

/* Write one SELCALL/tone frequency to register 0x22. */
void bk4815_set_tone(uint32_t tone_hz);

/* The chip's RSSI indicator: register 0x44, bits 6:0. */
uint16_t bk4815_read_rssi(void);

/* Enable or mute the chip's AF output (register 0x49).  The stock switches this
 * as the receive audio source when the BK4815 is the selected transceiver. */
void bk4815_set_af(bool on);

#endif /* DRIVER_BK4815_H */
