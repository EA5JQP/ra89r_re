/* Storage: the K1's PY25Q16 interface over this repo's SPI NOR driver, plus the
 * port's settings blob on the same chip (see docs/ra89r_eeprom.md for the part and
 * its layout).
 *
 * The K1 keeps its whole configuration in a flat blob that its eeprom/flash
 * layer reads and writes; the RA89R's stock codeplug is a *different* format
 * (decoded in docs/ra89r_eeprom.md), and mapping it into the K1 structure is a
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
#include "frequencies.h"
#include "misc.h"
#include "driver/py25q16.h"
#include "settings.h"

#define BLOB_ADDR      0x1FF000u
#define TEST_ADDR      0x1FE000u
#define BLOB_MAGIC     0x52393852u   /* "R89R" */
#define BLOB_VERSION   2u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;                          /* sizeof(EEPROM_Config_t) */
    uint16_t extra_size;                    /* bytes of extra[] in use */
    uint16_t reserved;
    uint32_t sum;                           /* additive checksum of settings */
    uint32_t extra_sum;                     /* and of extra[0..extra_size) */
    EEPROM_Config_t settings;
    uint8_t extra[STORAGE_EXTRA_MAX];
} blob_t;

/* The payload in RAM: what a load produced, and what the next save writes.
 * Kept outside the blob so saving the settings does not have to understand it. */
static uint8_t blob_extra[STORAGE_EXTRA_MAX];
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

bool storage_set_extra(const void *data, uint32_t size)
{
    if (data == 0 || size > STORAGE_EXTRA_MAX)
        return false;

    memset(blob_extra, 0, sizeof blob_extra);
    memcpy(blob_extra, data, size);
    blob_extra_size = (uint16_t)size;
    return true;
}

bool storage_get_extra(void *data, uint32_t size)
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
    const uint8_t *src = (const uint8_t *)pBuffer;
    static uint8_t sector[SPI_FLASH_SECTOR_SIZE];

    (void)Append;   /* the port's regions are direct-addressed, not journaled */

    if (src == 0 || Size == 0u || !storage_writable(Address, Size))
        return;     /* the stock's regions are read-only (docs/ra89r_port.md) */

    /* Read-modify-write the sector(s): the K1 updates a few bytes at a time and
     * NOR needs a whole-sector erase. */
    {
        uint32_t done = 0;

        while (done < Size) {
            const uint32_t addr = Address + done;
            const uint32_t base = addr & ~(SPI_FLASH_SECTOR_SIZE - 1u);
            const uint32_t off  = addr - base;
            uint32_t n = SPI_FLASH_SECTOR_SIZE - off;

            if (n > Size - done)
                n = Size - done;

            spi_flash_read(base, sector, sizeof sector);
            memcpy(sector + off, src + done, n);
            spi_flash_sector_erase(base);
            spi_flash_program(base, sector, sizeof sector);
            done += n;
        }
    }
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

uint32_t storage_size(void)
{
    uint32_t jedec = 0;

    spi_flash_id(0, &jedec);
    return spi_flash_size(jedec);
}

bool storage_id(uint16_t *man_dev, uint32_t *jedec)
{
    return spi_flash_id(man_dev, jedec);
}

void storage_init(void)
{
    spi_flash_init();
}

/* Returns true when the blob was there, valid and loaded into gEeprom; false
 * leaves gEeprom as the caller set it (each screen then shows its default). */
bool storage_load_settings(void)
{
    blob_t blob;

    memset(&blob, 0, sizeof blob);
    spi_flash_read(BLOB_ADDR, (uint8_t *)&blob, sizeof blob);

    if (blob.magic != BLOB_MAGIC ||
        blob.version != BLOB_VERSION ||
        blob.size != (uint16_t)sizeof(EEPROM_Config_t) ||
        blob.extra_size > STORAGE_EXTRA_MAX)
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
    SETTINGS_FixupVfoPointers();
    return true;
}

bool storage_save_settings(void)
{
    blob_t blob;

    memset(&blob, 0, sizeof blob);
    blob.magic = BLOB_MAGIC;
    blob.version = BLOB_VERSION;
    blob.size = (uint16_t)sizeof(EEPROM_Config_t);
    blob.extra_size = blob_extra_size;
    blob.settings = gEeprom;
    blob.sum = blob_sum(&blob.settings, sizeof blob.settings);
    memcpy(blob.extra, blob_extra, sizeof blob_extra);
    blob.extra_sum = blob_sum(blob.extra, blob.extra_size);

    spi_flash_sector_erase(BLOB_ADDR);
    spi_flash_program(BLOB_ADDR, (const uint8_t *)&blob, sizeof blob);

    /* Read it back: a program that did not take is the failure this has to
     * report, not a return code. */
    {
        blob_t check;

        memset(&check, 0, sizeof check);
        spi_flash_read(BLOB_ADDR, (uint8_t *)&check, sizeof check);
        return check.magic == BLOB_MAGIC && check.sum == blob.sum &&
               check.extra_sum == blob.extra_sum;
    }
}

/* The write test docs/ra89r_eeprom.md has been carrying as "pending": erase the
 * scratch sector, program a pattern, read it back.  Returns true when every byte
 * came back, which is the first thing to run on a radio whose write path has
 * never been exercised. */
bool storage_write_test(uint32_t *bad_offset)
{
    uint8_t pattern[256];
    uint8_t readback[256];
    uint32_t i;

    for (i = 0; i < sizeof pattern; i++)
        pattern[i] = (uint8_t)(0xA5u ^ i);

    spi_flash_sector_erase(TEST_ADDR);
    spi_flash_program(TEST_ADDR, pattern, sizeof pattern);
    memset(readback, 0, sizeof readback);
    spi_flash_read(TEST_ADDR, readback, sizeof readback);

    for (i = 0; i < sizeof pattern; i++) {
        if (readback[i] != pattern[i]) {
            if (bad_offset != 0)
                *bad_offset = TEST_ADDR + i;
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * The K1 application's EEPROM image (see driver/py25q16.h).
 *
 * The K1 code reads and writes this through the PY25Q16_* calls above, at the
 * addresses it always used.  The port only has to (a) allow writes inside the
 * image, and (b) fill it once from the stock, because on this radio the K1's
 * original addresses were erased.
 * ------------------------------------------------------------------------- */

/* The two writable regions: the K1 image band and the tail the port's own blob
 * and test scratch live in.  Everything else is the stock's, read-only. */
#define TAIL_BASE 0x1FE000u
#define TAIL_END  0x200000u

/* The stock calibration window the import reads (docs/ra89r_calibration.md). */
#define STOCK_POWER_BASE 0x3000u        /* 3 bytes/row: Low, Mid, High */
#define STOCK_RSSI_BASE  0x3860u        /* 2 bytes/row: RSSI1, RSSI9 */

/* The power page's row grid: the CPS's model-2 window is 401 rows of 2 MHz from
 * 100 MHz.  The one assumption the import makes; flagged in the doc so a bench
 * sweep can confirm it. */
#define STOCK_POWER_START_MHZ 100u
#define STOCK_POWER_STEP_MHZ  2u
#define STOCK_POWER_ROWS      401u

/* A marker just before the calibration block, in the same erased sector. */
#define K1_IMAGE_MAGIC_ADDR 0x100B8u
#define K1_IMAGE_MAGIC      0x31544B52u
#define K1_IMAGE_SECTOR     0x10000u

/* Calibration field offsets from K1_IMAGE_CAL_BASE. */
#define CAL_RSSI_A   0x00u   /* bands 3-6, 4 x u16 */
#define CAL_RSSI_B   0x08u   /* bands 0-2, 4 x u16 */
#define CAL_TX       0x10u   /* 7 bands x 16 B (level*3 within) */
#define CAL_BATTERY  0x80u   /* 6 x u16 */
#define CAL_VOX1     0x90u
#define CAL_VOX0     0xA8u
#define CAL_MISC     0xC8u

bool storage_writable(uint32_t addr, uint32_t size)
{
    if (size == 0u)
        return false;
    if (addr >= K1_IMAGE_BASE && addr + size <= K1_IMAGE_END)
        return true;
    if (addr >= TAIL_BASE && addr + size <= TAIL_END)
        return true;
    return false;
}

/* One power byte from the stock's per-frequency page.  `level` = 0/1/2 =
 * Low/Mid/High. */
static uint8_t stock_power(uint32_t freq10, uint8_t level)
{
    uint32_t mhz = freq10 / 100000u;    /* 10 Hz units -> MHz */
    uint32_t row;
    uint8_t  bytes[3];

    if (mhz < STOCK_POWER_START_MHZ)
        mhz = STOCK_POWER_START_MHZ;
    row = (mhz - STOCK_POWER_START_MHZ) / STOCK_POWER_STEP_MHZ;
    if (row >= STOCK_POWER_ROWS)
        row = STOCK_POWER_ROWS - 1u;

    PY25Q16_ReadBuffer(STOCK_POWER_BASE + row * 3u, bytes, sizeof bytes);
    return bytes[level & 3u];
}

/* Four RSSI thresholds from the stock's RSSI1/RSSI9 endpoints. */
static void stock_rssi(uint16_t out[4])
{
    uint8_t row[2];

    PY25Q16_ReadBuffer(STOCK_RSSI_BASE, row, sizeof row);
    out[0] = row[0];
    out[3] = row[1];
    out[1] = (uint16_t)(row[0] + (row[1] - row[0]) / 3u);
    out[2] = (uint16_t)(row[0] + 2u * (row[1] - row[0]) / 3u);
}

static void build_k1_calibration(uint8_t cal[K1_IMAGE_CAL_SIZE])
{
    static const uint16_t battery[6] = { 1900, 2000, 2100, 2300, 2400, 2300 };
    uint16_t rssi[4];
    unsigned band, level, point;

    memset(cal, 0, K1_IMAGE_CAL_SIZE);

    /* TX power: the K1 wants, per band and per power level (low/mid/high), the
     * value at the band's lower/mid/upper frequency.  The stock page is
     * per-frequency {Low,Mid,High}, so this is a straight transposition. */
    for (band = 0; band < BAND_N_ELEM; band++) {
        const uint32_t lo = frequencyBandTable[band].lower;
        const uint32_t hi = frequencyBandTable[band].upper;
        const uint32_t f[3] = { lo, (lo + hi) / 2u, hi };

        for (level = 0; level < 3u; level++)
            for (point = 0; point < 3u; point++)
                cal[CAL_TX + band * 16u + level * 3u + point] =
                    stock_power(f[point], (uint8_t)level);
    }

    stock_rssi(rssi);
    for (level = 0; level < 4u; level++) {
        cal[CAL_RSSI_A + level * 2u] = (uint8_t)(rssi[level] & 0xFFu);
        cal[CAL_RSSI_A + level * 2u + 1u] = (uint8_t)(rssi[level] >> 8);
        cal[CAL_RSSI_B + level * 2u] = (uint8_t)(rssi[level] & 0xFFu);
        cal[CAL_RSSI_B + level * 2u + 1u] = (uint8_t)(rssi[level] >> 8);
    }

    /* Fields with no stock source: the K1's own sensible fallbacks. */
    for (level = 0; level < 6u; level++) {
        cal[CAL_BATTERY + level * 2u] = (uint8_t)(battery[level] & 0xFFu);
        cal[CAL_BATTERY + level * 2u + 1u] = (uint8_t)(battery[level] >> 8);
        cal[CAL_VOX1 + level * 2u] = 0x00u;
        cal[CAL_VOX1 + level * 2u + 1u] = 0x01u;
        cal[CAL_VOX0 + level * 2u] = 0x00u;
        cal[CAL_VOX0 + level * 2u + 1u] = 0x02u;
    }

    cal[CAL_MISC + 6u] = 58u;   /* volume gain */
    cal[CAL_MISC + 7u] = 8u;    /* DAC gain */
}

void storage_import_k1(void)
{
    static uint8_t cal[K1_IMAGE_CAL_SIZE];
    const uint8_t magic[4] = { (uint8_t)K1_IMAGE_MAGIC,
                               (uint8_t)(K1_IMAGE_MAGIC >> 8),
                               (uint8_t)(K1_IMAGE_MAGIC >> 16),
                               (uint8_t)(K1_IMAGE_MAGIC >> 24) };
    uint32_t present = 0;

    spi_flash_init();
    PY25Q16_ReadBuffer(K1_IMAGE_MAGIC_ADDR, &present, sizeof present);
    if (present == K1_IMAGE_MAGIC)
        return;                     /* already imported */

    build_k1_calibration(cal);

    spi_flash_sector_erase(K1_IMAGE_SECTOR);
    spi_flash_program(K1_IMAGE_MAGIC_ADDR, magic, sizeof magic);
    spi_flash_program(K1_IMAGE_CAL_BASE, cal, sizeof cal);
}
