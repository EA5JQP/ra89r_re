/* External SPI NOR flash -- the radio's "EEPROM".
 *
 * What the CPS calls the EEPROM (channels, channel names, band ranges, settings,
 * DTMF/2-tone/5-tone tables, voice prompts) is this chip, not the MCU's flash:
 * see ra89r_findings.md, "CPS (programming software) -- band ranges are
 * CPS/EEPROM data" for the offset map.
 *
 * It is a Puya PY25Q16HB -- 16 Mbit / 2 MB, per a photo of the board; the JEDEC
 * id agrees (0x852015, capacity byte 0x15) and 0x90 answers 0x8514 -- on SPI1's
 * *remapped* pins, driven by hand: PA15 chip select (exactly as the stock does
 * it -- a plain GPIO, not the SPI peripheral's NSS), PB3 clock, PB4 MISO,
 * PB5 MOSI.  SPI mode 0: the clock idles low and both sides sample on the rising
 * edge.
 *
 * The stock identifies it in FUN_08018C7C (command 0x90) but compares the reply
 * with 0xEF16, a Winbond 32 Mbit id, so its own check does not match this
 * board's part; this driver reads 0x90 and 0x9F and takes the capacity from the
 * JEDEC id rather than assuming a part.  Data reads use 0x03, which every SPI
 * NOR accepts (the stock uses 0x0B in FUN_08018C0C).
 */
#ifndef DRIVER_SPI_FLASH_H
#define DRIVER_SPI_FLASH_H

#include <stdint.h>
#include <stdbool.h>

/* Configure the bus and deselect the chip. */
void spi_flash_init(void);

/* Read the two identities.  Either pointer may be NULL.  Returns false when the
 * bus reads back all ones, which is what an absent, unpowered or mis-wired chip
 * looks like: nothing is pulling MISO low. */
bool spi_flash_id(uint16_t *man_dev, uint32_t *jedec);

/* Size in bytes implied by a JEDEC id (0 when the capacity byte is not a known
 * shape).  The byte is log2 of the size: 0x15 -> 2 MB (this board's P25Q16),
 * 0x16 -> 4 MB (the part the stock's 0xEF16 check expects). */
uint32_t spi_flash_size(uint32_t jedec);

/* Read len bytes from a 24-bit address (command 0x03, no dummy byte). */
void spi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

#endif /* DRIVER_SPI_FLASH_H */
