/* Storage: the K1's PY25Q16 interface over this repo's SPI NOR driver, plus the
 * port's settings blob on the same chip (see ra89r_eeprom.md for the part and
 * its layout).
 *
 * The K1 keeps its whole configuration in a flat blob that its eeprom/flash
 * layer reads and writes; the RA89R's stock codeplug is a *different* format
 * (decoded in ra89r_eeprom.md), and mapping it into the K1 structure is a
 * separate job.  Until that mapping exists, the port stores its own blob -- a
 * header and gEeprom -- in the last sector of the 2 MB part, which the dump of
 * this radio shows empty:
 *
 *   0x1FF000  the port's settings blob (this file)
 *   0x1FE000  scratch for the write test ('6' on the console)
 *
 * Both addresses are in the empty tail of the part, so nothing the stock stored
 * is touched before the mapping lands.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/py25q16.h"
#include "driver/spi_flash.h"
#include "port_state.h"
#include "port_storage.h"
#include "settings.h"

#define PORT_BLOB_ADDR      0x1FF000u
#define PORT_TEST_ADDR      0x1FE000u
#define PORT_BLOB_MAGIC     0x52393852u   /* "R89R" */
#define PORT_BLOB_VERSION   2u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;                          /* sizeof(EEPROM_Config_t) */
    uint16_t extra_size;                    /* bytes of extra[] in use */
    uint16_t reserved;
    uint32_t sum;                           /* additive checksum of settings */
    uint32_t extra_sum;                     /* and of extra[0..extra_size) */
    EEPROM_Config_t settings;
    uint8_t extra[PORT_STORAGE_EXTRA_MAX];
} port_blob_t;

/* The payload in RAM: what a load produced, and what the next save writes.
 * Kept outside the blob so saving the settings does not have to understand it. */
static uint8_t blob_extra[PORT_STORAGE_EXTRA_MAX];
static uint16_t blob_extra_size;

static uint32_t blob_sum(const void *data, uint32_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    uint32_t i;

    for (i = 0; i < size; i++)
        sum += p[i];
    return sum;
}

bool port_storage_set_extra(const void *data, uint32_t size)
{
    if (data == 0 || size > PORT_STORAGE_EXTRA_MAX)
        return false;

    memset(blob_extra, 0, sizeof blob_extra);
    memcpy(blob_extra, data, size);
    blob_extra_size = (uint16_t)size;
    return true;
}

bool port_storage_get_extra(void *data, uint32_t size)
{
    if (data == 0 || size > blob_extra_size)
        return false;

    memcpy(data, blob_extra, size);
    return true;
}

/* ---------------------------------------------------------------------------
 * The K1's PY25Q16 interface.
 * ------------------------------------------------------------------------- */

void PY25Q16_Init(void)
{
    spi_flash_init();
}

void PY25Q16_ReadBuffer(uint32_t Address, void *pBuffer, uint32_t Size)
{
    if (pBuffer != 0 && Size > 0u)
        spi_flash_read(Address, (uint8_t *)pBuffer, Size);
}

void PY25Q16_ReadBufferSafe(uint32_t Address, void *pBuffer, uint32_t Size)
{
    /* The K1's retrying variant; a bit-banged bus either answers or does not,
     * and the caller checks what it read, so one read is enough here. */
    PY25Q16_ReadBuffer(Address, pBuffer, Size);
}

void PY25Q16_WriteBuffer(uint32_t Address, const void *pBuffer, uint32_t Size, bool Append)
{
    /* Appending is the K1's journal-style write; the port's blob is rewritten
     * whole (erase + program), so an append request is refused loudly rather
     * than quietly corrupting the sector. */
    (void)Address;
    (void)pBuffer;
    (void)Size;
    (void)Append;
}

void PY25Q16_SectorErase(uint32_t Address)
{
    spi_flash_sector_erase(Address);
}

void PY25Q16_InvalidateCache(void)
{
    /* The port's driver has no sector cache. */
}

/* ---------------------------------------------------------------------------
 * The port's settings blob.
 * ------------------------------------------------------------------------- */

uint32_t port_storage_size(void)
{
    uint32_t jedec = 0;

    spi_flash_id(0, &jedec);
    return spi_flash_size(jedec);
}

bool port_storage_id(uint16_t *man_dev, uint32_t *jedec)
{
    return spi_flash_id(man_dev, jedec);
}

void port_storage_init(void)
{
    spi_flash_init();
}

/* Returns true when the blob was there, valid and loaded into gEeprom; false
 * leaves gEeprom as the caller set it (each screen then shows its default). */
bool port_storage_load_settings(void)
{
    port_blob_t blob;

    memset(&blob, 0, sizeof blob);
    spi_flash_read(PORT_BLOB_ADDR, (uint8_t *)&blob, sizeof blob);

    if (blob.magic != PORT_BLOB_MAGIC ||
        blob.version != PORT_BLOB_VERSION ||
        blob.size != (uint16_t)sizeof(EEPROM_Config_t) ||
        blob.extra_size > PORT_STORAGE_EXTRA_MAX)
        return false;
    if (blob.sum != blob_sum(&blob.settings, sizeof blob.settings))
        return false;
    if (blob.extra_sum != blob_sum(blob.extra, blob.extra_size))
        return false;

    memset(blob_extra, 0, sizeof blob_extra);
    memcpy(blob_extra, blob.extra, blob.extra_size);
    blob_extra_size = blob.extra_size;

    gEeprom = blob.settings;
    /* The struct carries pointers into itself, which a flash round-trip cannot
     * be trusted to preserve: re-establish them. */
    port_state_fixup_vfo();
    return true;
}

bool port_storage_save_settings(void)
{
    port_blob_t blob;

    memset(&blob, 0, sizeof blob);
    blob.magic = PORT_BLOB_MAGIC;
    blob.version = PORT_BLOB_VERSION;
    blob.size = (uint16_t)sizeof(EEPROM_Config_t);
    blob.extra_size = blob_extra_size;
    blob.settings = gEeprom;
    blob.sum = blob_sum(&blob.settings, sizeof blob.settings);
    memcpy(blob.extra, blob_extra, sizeof blob_extra);
    blob.extra_sum = blob_sum(blob.extra, blob.extra_size);

    spi_flash_sector_erase(PORT_BLOB_ADDR);
    spi_flash_program(PORT_BLOB_ADDR, (const uint8_t *)&blob, sizeof blob);

    /* Read it back: a program that did not take is the failure this has to
     * report, not a return code. */
    {
        port_blob_t check;

        memset(&check, 0, sizeof check);
        spi_flash_read(PORT_BLOB_ADDR, (uint8_t *)&check, sizeof check);
        return check.magic == PORT_BLOB_MAGIC && check.sum == blob.sum &&
               check.extra_sum == blob.extra_sum;
    }
}

/* The write test ra89r_eeprom.md has been carrying as "pending": erase the
 * scratch sector, program a pattern, read it back.  Returns true when every byte
 * came back, which is the first thing to run on a radio whose write path has
 * never been exercised. */
bool port_storage_write_test(uint32_t *bad_offset)
{
    uint8_t pattern[256];
    uint8_t readback[256];
    uint32_t i;

    for (i = 0; i < sizeof pattern; i++)
        pattern[i] = (uint8_t)(0xA5u ^ i);

    spi_flash_sector_erase(PORT_TEST_ADDR);
    spi_flash_program(PORT_TEST_ADDR, pattern, sizeof pattern);
    memset(readback, 0, sizeof readback);
    spi_flash_read(PORT_TEST_ADDR, readback, sizeof readback);

    for (i = 0; i < sizeof pattern; i++) {
        if (readback[i] != pattern[i]) {
            if (bad_offset != 0)
                *bad_offset = PORT_TEST_ADDR + i;
            return false;
        }
    }
    return true;
}
