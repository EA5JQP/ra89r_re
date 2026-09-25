/* BK4829 RF transceiver -- see bk4829.h for the framing and the evidence. */
#include "driver/bk4829.h"

#include "board_pins.h"
#include "driver/rf_bus.h"

#define CS BK4829_CS_PIN

void bk4829_init(void)
{
    rf_bus_init();
}

uint16_t bk4829_read_reg(uint8_t reg)
{
    return rf_bus_read(CS, (uint8_t)(reg | 0x80u));
}

void bk4829_write_reg(uint8_t reg, uint16_t value)
{
    uint8_t b[2];

    b[0] = (uint8_t)(value >> 8);
    b[1] = (uint8_t)value;
    rf_bus_write(CS, (uint8_t)(reg & 0x7fu), b, 2u);
}

bool bk4829_detect(void)
{
    return bk4829_read_reg(BK4829_REG_ID) == BK4829_ID;
}

/* The stock's boot configuration: FUN_08006B78, 39 registers in 40 writes.
 *
 * Order matters.  The two register-0 writes come first, the block below in this
 * order, and the last two (0x48 then 0x47) last.  Between the 0x4c entry and
 * those two the stock calls FUN_0801BAF4, which ALWAYS re-writes register 0x7d:
 * all four of its branches fall through to
 * `FUN_080220A0(0xe940 | v, 0x7d)`, where v comes from a field at 0x20009f37
 * and the flag at 0x20000301 picks between two formulas
 *
 *     flag == 1:  v = 3 * field + 3
 *     otherwise:  v = 4 * field + 12
 *
 * so the 0xe920 the stock writes here first is dead: the chip only ever sees
 * 0xe940 | v.
 *
 * Both inputs trace back into the codeplug rather than to constants, and the
 * value this radio ends up sending is derived in ra89r_bk4829.md ("The boot
 * configuration"): the field is `buffer[10] & 7` of the 32-byte settings block
 * at EEPROM 0x2020, which reads 0x03 here, and the only instruction in the stock
 * that writes the flag writes 0.  Hence v = 4 * 3 + 12 = 0x18, and the value
 * below is 0xe940 | 0x18 = 0xe958.  Our firmware does not build that config
 * struct, so this is a derived constant rather than a computed one; 0x7d is the
 * first register to read back once the bus answers on the radio. */
static const struct {
    uint8_t  reg;
    uint16_t value;
} bk4829_config[] = {
    { 0x00, 0x8000 },
    { 0x00, 0x0000 },
    { 0x37, 0x9d1f },
    { 0x13, 0x03df },
    { 0x12, 0x03db },
    { 0x11, 0x033a },
    { 0x10, 0x0318 },
    { 0x14, 0x0210 },
    { 0x19, 0x1041 },
    { 0x1c, 0x0422 },
    { 0x1d, 0x2aab },
    { 0x1e, 0x4c58 },
    { 0x1f, 0x165a },
    { 0x25, 0x6dba },
    { 0x28, 0x0b40 },
    { 0x29, 0xaa00 },
    { 0x2a, 0x6600 },
    { 0x2c, 0x0022 },
    { 0x2f, 0x9890 },
    { 0x3a, 0x9a7c },
    { 0x3e, 0x94c6 },
    { 0x3f, 0x07fe },
    { 0x40, 0x34f0 },
    { 0x46, 0x6050 },
    { 0x48, 0xb386 },
    { 0x49, 0x2a32 },
    { 0x4a, 0x5430 },
    { 0x4d, 0xa015 },
    { 0x4e, 0x6f10 },
    { 0x4f, 0x2a28 },
    { 0x53, 0x2028 },
    { 0x73, 0x6681 },
    { 0x77, 0x88ef },
    { 0x7b, 0x73dc },
    { 0x7d, 0xe958 },   /* the stock's effective value; see the note above */
    { 0x7e, 0x303e },
    { 0x4c, 0xe520 },
    /* FUN_0801BAF4 (the per-build tweak of 0x7d) sits here in the stock; it is
     * folded into the 0x7d value above rather than replayed. */
    { 0x48, 0xb3b5 },
    { 0x47, 0x6042 },
};

unsigned bk4829_config_writes(void)
{
    return (unsigned)(sizeof bk4829_config / sizeof bk4829_config[0]);
}

void bk4829_configure(void)
{
    unsigned i;

    for (i = 0; i < bk4829_config_writes(); i++)
        bk4829_write_reg(bk4829_config[i].reg, bk4829_config[i].value);
}
