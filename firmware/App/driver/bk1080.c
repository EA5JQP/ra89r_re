/* BK1080 FM receiver -- see bk1080.h for the framing and the evidence.
 *
 * Every constant here comes from the stock image and is named by function in
 * docs/ra89r_bk1080.md; the datasheet (docs/BK1080.pdf) supplies the field
 * names.  Nothing has been exercised on the radio. */
#include "driver/bk1080.h"

#include "driver/i2c_bus.h"

/* The stock's power-up block (`FUN_08007124`): 68 bytes written to register 0,
 * which the part takes as registers 0x00..0x21 in one transfer.  In the image
 * the bytes are RAM, not flash: the scatter table at 0x08027728 decompresses
 * 844 bytes from flash 0x08027748 to RAM 0x20000000 (`FUN_0800483A`), and the
 * block pointer 0x20000088 (the literal `FUN_08007124` loads) is offset 0x88
 * into that.  The values are read out big-endian, so register 1 = 0x1080 (the
 * chip id), register 2 = 0x0201 (ENABLE + SEEKUP), register 5 = 0x0a5d.  They
 * are not mapped to public fields beyond that; the block is the vendor's
 * register image, and several of the registers it writes are read-only. */
static const uint8_t bk1080_block[] = {
    0x00, 0x08, 0x10, 0x80, 0x02, 0x01, 0x00, 0x00,
    0x40, 0xc0, 0x0a, 0x5d, 0x00, 0x2e, 0x02, 0xff,
    0x5b, 0x11, 0x00, 0x00, 0x41, 0x1e, 0x00, 0x00,
    0xce, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x31, 0x97, 0x00, 0x00, 0x13, 0xff, 0x98, 0x52,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x51, 0xe1, 0x28, 0xdc, 0x26, 0x45, 0x00, 0xe4,
    0x1c, 0xd8, 0x3a, 0x50, 0xea, 0xf0, 0x30, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

/* The two register-0x32 writes that follow the block in `FUN_08007124`.  Register
 * 0x32 is outside the public map (table 11 stops at 0x1f), so these are the
 * vendor's internal registers; the stock sends both, 100 delay loops apart. */
static const uint16_t bk1080_reg32[] = { 0x285cu, 0x28dcu };

void bk1080_init(void)
{
    i2c_bus_init();
}

/* ---------------------------------------------------------------- framing --- */

uint16_t bk1080_read_reg(uint8_t reg)
{
    uint8_t hi, lo;

    i2c_bus_start();
    (void)i2c_bus_write_byte(BK1080_ADDR);
    (void)i2c_bus_write_byte((uint8_t)(((reg & 0x7fu) << 1) | 1u));

    hi = i2c_bus_read_byte();
    i2c_bus_send_ack(true);            /* ACK after every byte but the last */
    lo = i2c_bus_read_byte();
    i2c_bus_send_ack(false);           /* NACK ends the read */

    i2c_bus_stop();
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

void bk1080_write_reg(uint8_t reg, uint16_t value)
{
    i2c_bus_start();
    (void)i2c_bus_write_byte(BK1080_ADDR);
    (void)i2c_bus_write_byte((uint8_t)((reg & 0x7fu) << 1));
    (void)i2c_bus_write_byte((uint8_t)(value >> 8));
    (void)i2c_bus_write_byte((uint8_t)value);
    i2c_bus_stop();
}

void bk1080_write_block(uint8_t reg, const uint8_t *bytes, unsigned len)
{
    unsigned i;

    i2c_bus_start();
    (void)i2c_bus_write_byte(BK1080_ADDR);
    (void)i2c_bus_write_byte((uint8_t)((reg & 0x7fu) << 1));
    for (i = 0; i < len; i++)
        (void)i2c_bus_write_byte(bytes[i]);
    i2c_bus_stop();
}

uint16_t bk1080_read_id(void)
{
    return bk1080_read_reg(BK1080_REG_ID);
}

/* ------------------------------------------------------------ init/config --- */

/* The stock's `FUN_0802422A(100)` between the block and the register-0x32
 * writes: a (100 + 1) x 21 cycle busy-wait.  Kept as a loop so the ordering is
 * faithful without depending on the target clock. */
static void config_delay(void)
{
    volatile unsigned i;

    for (i = 0; i < 2100u; i++)
        ;
}

void bk1080_configure(void)
{
    unsigned i;

    bk1080_write_block(0u, bk1080_block, (unsigned)sizeof bk1080_block);

    for (i = 0; i < bk1080_config_reg32_writes(); i++) {
        config_delay();
        bk1080_write_reg(0x32u, bk1080_reg32[i]);
    }
}

const uint8_t *bk1080_config_block(unsigned *len)
{
    if (len)
        *len = (unsigned)sizeof bk1080_block;
    return bk1080_block;
}

unsigned bk1080_config_reg32_writes(void)
{
    return (unsigned)(sizeof bk1080_reg32 / sizeof bk1080_reg32[0]);
}

void bk1080_config_reg32_entry(unsigned i, uint16_t *value)
{
    if (i >= bk1080_config_reg32_writes())
        return;
    if (value)
        *value = bk1080_reg32[i];
}

/* -------------------------------------------------------------- tuning ----- */

/* `FUN_08006F9C`, verbatim arithmetic: the frequency in 100 kHz units is the
 * channel plus the band's base.  Below 76 MHz the stock uses BAND = 11 (base
 * 64), otherwise BAND = 01 (base 76); SPACE is 100 kHz and VOLUME is maximum
 * in both.  SEEKTH is 0x0a.  Then CHAN, and TUNE|CHAN. */
void bk1080_set_frequency(uint32_t freq_10hz)
{
    uint32_t ch100 = freq_10hz / 10000u;
    uint16_t base, band, chan;

    if (ch100 < 760u) {
        base = 640u;
        band = 0xc0u;                  /* BAND = 11, 64-76 MHz */
    } else {
        base = 760u;
        band = 0x40u;                  /* BAND = 01, 76-108 MHz */
    }

    band = (uint16_t)(band | 0x1fu);   /* SPACE = 01 (100 kHz), VOLUME = 0xf */
    chan = (uint16_t)(ch100 - base);

    bk1080_write_reg(BK1080_REG_SYSCFG2, (uint16_t)(0x0a00u | band));
    bk1080_write_reg(BK1080_REG_CHANNEL, chan);
    bk1080_write_reg(BK1080_REG_CHANNEL, (uint16_t)(BK1080_CHANNEL_TUNE | chan));
}

bool bk1080_wait_tune(unsigned timeout)
{
    while (timeout-- > 0u) {
        if (bk1080_seek_complete())
            return true;
    }
    return false;
}

/* `FUN_0800687C`: READCHAN from register 0x0b, BAND from register 0x05, then the
 * band's base in 100 kHz units.  The order of the two reads is the stock's. */
uint32_t bk1080_get_frequency(void)
{
    uint16_t readchan = (uint16_t)(bk1080_read_reg(BK1080_REG_READCHAN) & 0x03ffu);
    uint16_t band = (uint16_t)((bk1080_read_reg(BK1080_REG_SYSCFG2) >> BK1080_BAND_SHIFT) & 0x3u);
    uint16_t base = (band == 0u) ? 875u
                  : (band == 1u || band == 2u) ? 760u
                  : 640u;

    return (uint32_t)(readchan + base) * 10000u;
}

/* --------------------------------------------------------- status / seek --- */

uint16_t bk1080_read_status(void)
{
    return bk1080_read_reg(BK1080_REG_RSSI);
}

uint8_t bk1080_get_rssi(void)
{
    return (uint8_t)(bk1080_read_status() & BK1080_STATUS_RSSI_MASK);
}

bool bk1080_seek_complete(void)
{
    return (bk1080_read_status() & BK1080_STATUS_STC) != 0u;
}

bool bk1080_seek_failed(void)
{
    return (bk1080_read_status() & BK1080_STATUS_SFBL) != 0u;
}

/* `FUN_0800693C`: register 0x07's low nibble is SNR[3:0]. */
uint8_t bk1080_get_snr(void)
{
    return (uint8_t)(bk1080_read_reg(BK1080_REG_TEST1) & 0x000fu);
}

/* `FUN_08006952`: read register 2, set SEEK | SEEKUP | SKMODE.  SEEKUP is always
 * set here, so the stock only ever seeks up; SKMODE = 1 stops at the band edge
 * instead of wrapping. */
void bk1080_seek_up(void)
{
    uint16_t v = bk1080_read_reg(BK1080_REG_POWER);

    v |= (uint16_t)(BK1080_POWER_SEEK | BK1080_POWER_SEEKUP | BK1080_POWER_SKMODE);
    bk1080_write_reg(BK1080_REG_POWER, v);
}

/* `FUN_08006982`: clear SEEK, which also clears STC and SF/BL. */
void bk1080_clear_seek(void)
{
    uint16_t v = bk1080_read_reg(BK1080_REG_POWER);

    v &= (uint16_t)~BK1080_POWER_SEEK;
    bk1080_write_reg(BK1080_REG_POWER, v);
}

/* `FUN_080069A6`: clear TUNE, which clears STC. */
void bk1080_clear_tune(void)
{
    uint16_t v = bk1080_read_reg(BK1080_REG_CHANNEL);

    v &= (uint16_t)~BK1080_CHANNEL_TUNE;
    bk1080_write_reg(BK1080_REG_CHANNEL, v);
}
