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
#define BK4815_REG_AF        0x49u   /* AUDIO: volume + bit15 = hard mute */
#define BK4815_REG_OPCTRL    0x70u
#define BK4815_REG_FREQ_HI   0x71u
#define BK4815_REG_FREQ_LO   0x72u
#define BK4815_REG_CAL       0x7eu

/* Receive sub-audio (datasheet registers, decimal in brackets): */
#define BK4815_REG_DEMOD     0x42u   /* [66] bit 8 CTCSS rx, bit 7 CDCSS rx */
#define BK4815_REG_CTC_SPEED 0x59u   /* [89] detection speed */
#define BK4815_REG_CTC_SYM   0x5bu   /* [91] symbol address 15:13 + frequency 12:0 */
#define BK4815_REG_CTC_CTRL  0x5cu   /* [92] HPF bypass, lost, found address, count */
#define BK4815_REG_CTC_LOSS  0x5du   /* [93] loss threshold, phase */
#define BK4815_REG_IRQ_MASK  0x73u   /* [115] */
#define BK4815_REG_IRQ_FLAGS 0x74u   /* [116] write 1 to clear */

#define BK4815_IRQ_CTCSS_FOUND (1u << 8)
#define BK4815_IRQ_CTCSS_LOST  (1u << 7)
#define BK4815_CTC_CTRL_LOST   (1u << 13)   /* 0x5C: the found symbol is lost */

#define BK4815_REG_CDC_STATUS 0x5eu  /* [94] found address, sync threshold, phase, lost */
#define BK4815_REG_CDC_CODE   0x5fu  /* [95] high 11 bits; [96] = 0x60 the low 12 */
#define BK4815_REG_CDC_SEARCH 0x61u  /* [97] codes to search 15:8, RX code address 6:0 */
#define BK4815_REG_CDC_MATCH  0x62u  /* [98] match count 3:1, slice enable 0 */

#define BK4815_IRQ_CDCSS_FOUND (1u << 5)
#define BK4815_IRQ_CDCSS_LOST  (1u << 4)
#define BK4815_CDC_STATUS_INVERSE (1u << 2)  /* 0x5E: found code inverted */
#define BK4815_CDC_STATUS_LOST    (1u << 1)  /* 0x5E: CDCSS loss */

/* 0x73 on receive: the stock's 0x81D0 (link loss, CTCSS found/lost, CDCSS
 * lost); a DCS channel also needs the CDCSS-found enable (bit 5), which the
 * stock -- reading INTN rather than the flags -- does not set. */
#define BK4815_IRQ_MASK_RX     0x81d0u
#define BK4815_IRQ_MASK_RX_DCS (BK4815_IRQ_MASK_RX | BK4815_IRQ_CDCSS_FOUND)

/* Register 0x49 (REG73 in the datasheet): bit 15 is "hard mute" (1 = muted).
 * The stock's AF switch writes 0x9A02 to mute (`FUN_08015F48`) and 0x1A02 to
 * unmute (`FUN_0801CCE8`), and its boot config leaves 0x1A02 (unmuted). */
#define BK4815_AF_ON   0x1a02u   /* hard mute clear */
#define BK4815_AF_OFF  0x9a02u   /* hard mute set */

/* 0x0C, the per-block power-down word (datasheet Table 4, 1 = powered down).
 * The boot/idle word 0x0A03 receives and demodulates -- 0x44 and the 0x49
 * audio level both follow a carrier -- but keeps bit 11 (RX Audio) set, and
 * the part's audio never reaches the speaker; the datasheet's RX word 0x0603
 * (bit 10, TX Audio, set) is silent too.  Only the stock's lane-0 word 0x0203,
 * both bits clear, is heard (radio 2026-10-30, tinySA FM tone on 446.01875:
 * 0x0A03 noise, 0x0603 noise, 0x0203 tone, 0x0A03 noise again). */
#define BK4815_PWR_RX_AUDIO 0x0203u
#define BK4815_PWR_RX_QUIET 0x0a03u

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

/* The stock's per-VFO receive entry (FUN_08016CEC, see bk4815.h): the demod/
 * baseband/AF block it writes after the tune, including the channel-spacing
 * bit in register 0x01 (`wide` = 25 kHz, else 12.5 kHz) and the `0x70`
 * receive restart pair.  Called on a lane (re)configuration, not per scan
 * probe. */
/* The stock's PLL calibration (FUN_0801BF76/FUN_0801BF54): 0x0C = 0x0A03,
 * 0x0B = 0x0800/0x8800/0x9800, then the previous 0x0C word back.  Run by
 * bk4815_receive_entry(), i.e. after every tune of a VFO. */
void bk4815_pll_calibrate(void);
/* The last word written to 0x0C, by anyone (the part's own read-back is not
 * reliable: it changes bits by itself). */
uint16_t bk4815_power_word(void);
void bk4815_receive_entry(bool wide);

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

/* Set the band value in register 0x75 the way the stock's FUN_08013790 does:
 * read-modify-write, `(read & 0xFFC0) | band`, so the register's own state
 * (datasheet 117: bit 10 LO injection side, bit 9 tone-to-RX-audio, bits 8:6
 * RX-audio source) survives.  `band` is the stock's low-six-bit code: 0x09/0x11
 * the VHF values, 0x0A/0x12 the UHF ones. */
void bk4815_set_band(uint16_t band);

/* 0x75 bit 10, the LO injection side, applied at once and kept by every
 * later bk4815_set_band().  Default 1 (low-side, as the stock leaves it; see
 * bk4815_set_band()); console '^' flips it for a bench comparison. */
void bk4815_set_lo_bit10(bool set);
bool bk4815_lo_bit10(void);

/* The chip's RSSI indicator: register 0x44, bits 6:0. */
uint16_t bk4815_read_rssi(void);

/* The CTCSS detector symbol word for 0x5B (bits 12:0), from a tone in 0.1 Hz
 * (the K1's CTCSS_Options unit).  The stock's FUN_08016F28 mode 0:
 * (tone << 19) / 18466, then divided by 10 with round-half-up -- the
 * datasheet's (freq/18466)*65536*8. */
uint16_t bk4815_ctcss_rx_word(uint16_t tone_01hz);

/* Program the receive sub-audio detector the way the stock's FUN_08006D10 does
 * for a channel: with a tone (0.1 Hz units), symbol 0 = the tone, 0x5C = 0x4001
 * (HPF bypass, one symbol); with 0, the stock's no-tone setup (0x5B = 0x061A,
 * 55 Hz; 0x5C = 0x4000).  0x5D = 0x0060 and 0x42 = 0xD103 (CTCSS receive on)
 * in both.  DCS receive (the stock's 0x42 = 0xD083 branch) is not done here. */
void bk4815_set_rx_ctcss(uint16_t tone_01hz);

/* The receive DCS word for a channel, from the K1's 23-bit Golay word
 * (DCS_GetGolayCodeWord()): the stock programs the receive detector with the
 * code's *inverted* word (FUN_08016FE6 -> FUN_0801C576(code, 0, 0) ->
 * FUN_080085B0(code, 0); polarity 1 is the K1's word, polarity 0 its
 * complement -- checked for all 104 codes, 2026-11-02). */
uint32_t bk4815_dcs_rx_word(uint32_t k1_word);

/* 0x5F / 0x60 for a 23-bit word, FUN_0801C576's layout: 0xF000 | the high 11
 * bits, 0xF000 | the low 12 bits. */
void bk4815_dcs_code_regs(uint32_t word, uint16_t *hi, uint16_t *lo);

/* Program the receive DCS detector the way the stock's FUN_08006D10 DCS branch
 * does (0x59 = 0xF7A1, 0x61 = 0x0100, the code, 0x5C = 0x4000, 0x5D = 0x0060,
 * 0x42 = 0xD083), plus 0x62 = 0x0003: the stock's boot value from the factory
 * block (byte 0x3880 + 0x0C = 0x03 -- slice on, 2 matches), which the port's
 * boot table cannot source and writes as 0.  `word` is the word to program,
 * i.e. bk4815_dcs_rx_word()'s result. */
void bk4815_set_rx_dcs(uint32_t word);

/* The interrupt flags (0x74) and their write-1-to-clear. */
uint16_t bk4815_read_irq_flags(void);
void bk4815_clear_irq_flags(uint16_t bits);

/* Enable or mute the chip's AF output (register 0x49).  The stock switches this
 * as the receive audio source when the BK4815 is the selected transceiver. */
void bk4815_set_af(bool on);

#endif /* DRIVER_BK4815_H */
