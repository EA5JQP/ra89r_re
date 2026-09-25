/* BK4815 RF transceiver -- see bk4815.h for the framing and the evidence. */
#include "driver/bk4815.h"

#include "board_pins.h"
#include "driver/rf_bus.h"

#define CS BK4815_CS_PIN

/* The 36-byte block the stock sends to registers 2..19 in one select pulse
 * (FUN_08021F78(0x08024E40, 2, 0x24)); the bytes are the flash contents in
 * order, MSB first, so this is 18 big-endian 16-bit words. */
static const uint8_t bk4815_block[] = {
    0x6f, 0xc0, 0x0e, 0x3d, 0xb0, 0x41, 0xf7, 0x70, 0xf2, 0x74, 0x08, 0xf0,
    0xff, 0x33, 0xc3, 0xfa, 0xa2, 0xa3, 0x88, 0x00, 0x06, 0x03, 0x09, 0xfd,
    0x58, 0x17, 0x90, 0xa3, 0x88, 0xf9, 0x58, 0x00, 0x41, 0x5c, 0x08, 0xa0,
};

/* The stock's boot configuration: FUN_08006A0C, 33 single writes after the
 * block above.
 *
 * Three of them take their value from the stock's own RAM rather than from a
 * constant -- 0x4c, 0x55 and 0x62, read from 0x2000449a/0x2000449c/0x2000449e.
 * Those are calibration-shaped fields the stock loads earlier in its boot, and
 * this firmware has no source for them, so they go out as 0 and are flagged
 * here and by the console rather than passed off as the stock's values.  Do not
 * read this table as the full stock configuration until they have a source. */
#define BK4815_RAM_SOURCED 0x0000u

static const uint8_t bk4815_ram_regs[] = { 0x4c, 0x55, 0x62 };

static const struct {
    uint8_t  reg;
    uint16_t value;
} bk4815_config[] = {
    { 0x70, 0xa000 },
    { 0x28, 0x8820 },
    { 0x29, 0x2050 },
    { 0x2c, 0x8a2f },
    { 0x2d, 0x1bc0 },
    { 0x40, 0x8000 },
    { 0x41, 0xe000 },
    { 0x44, 0x8000 },
    { 0x45, 0x67ff },
    { 0x47, 0x0a18 },
    { 0x48, 0xa002 },
    { 0x49, 0x1a02 },
    { 0x4b, 0xf606 },
    { 0x4c, BK4815_RAM_SOURCED },   /* stock: RAM 0x2000449a */
    { 0x54, 0xfc46 },
    { 0x55, BK4815_RAM_SOURCED },   /* stock: RAM 0x2000449c */
    { 0x58, 0x0208 },
    { 0x59, 0xf7a1 },
    { 0x5e, 0x8028 },
    { 0x62, BK4815_RAM_SOURCED },   /* stock: RAM 0x2000449e */
    { 0x67, 0xc31f },
    { 0x68, 0x4020 },
    { 0x6a, 0xcc31 },
    { 0x6b, 0x3415 },
    { 0x6c, 0xe927 },
    { 0x6d, 0x6618 },
    { 0x7a, 0x46a3 },
    { 0x7b, 0x0002 },
    { 0x7c, 0xf3ac },
    { 0x7d, 0x76b5 },
    { 0x7e, 0xfff5 },
    { 0x7f, 0x3568 },
    { 0x0c, 0x0a03 },
};

unsigned bk4815_ram_sourced_writes(void)
{
    return (unsigned)(sizeof bk4815_ram_regs / sizeof bk4815_ram_regs[0]);
}

unsigned bk4815_config_writes(void)
{
    return (unsigned)(sizeof bk4815_config / sizeof bk4815_config[0]);
}

void bk4815_config_entry(unsigned i, uint8_t *reg, uint16_t *value)
{
    if (i >= bk4815_config_writes())
        return;
    if (reg)
        *reg = bk4815_config[i].reg;
    if (value)
        *value = bk4815_config[i].value;
}

const uint8_t *bk4815_config_block(unsigned *len)
{
    if (len)
        *len = (unsigned)sizeof bk4815_block;
    return bk4815_block;
}

uint16_t bk4815_read_reg(uint8_t reg)
{
    return rf_bus_read(CS, (uint8_t)(((reg & 0x7fu) << 1) | 1u));
}

void bk4815_write_reg(uint8_t reg, uint16_t value)
{
    uint8_t b[2];

    b[0] = (uint8_t)(value >> 8);
    b[1] = (uint8_t)value;
    rf_bus_write(CS, (uint8_t)((reg & 0x7fu) << 1), b, 2u);
}

void bk4815_write_block(uint8_t reg, const uint8_t *bytes, unsigned len)
{
    rf_bus_write(CS, (uint8_t)((reg & 0x7fu) << 1), bytes, len);
}

bool bk4815_detect(void)
{
    return bk4815_read_reg(BK4815_REG_ID) == BK4815_ID;
}

void bk4815_configure(void)
{
    unsigned i;

    bk4815_write_block(2u, bk4815_block, (unsigned)sizeof bk4815_block);
    for (i = 0; i < bk4815_config_writes(); i++)
        bk4815_write_reg(bk4815_config[i].reg, bk4815_config[i].value);
}
