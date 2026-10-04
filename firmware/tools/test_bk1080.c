/* Host test for the BK1080 register layer, without a radio.
 *
 * The I2C bit-bang (`i2c_bus.c`) is replaced with a recording stub, so this
 * checks the thing a bring-up cannot check by watching a silent bus: the exact
 * bytes the part will see -- the fixed device id, the (reg << 1) | R/W control
 * word, the word order, the master's ACK bits -- plus the tuning arithmetic and
 * the seek read-modify-writes.
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_bk1080.c App/driver/bk1080.c \
 *       -o /tmp/test_bk1080 && /tmp/test_bk1080
 *
 * It is not a substitute for the radio: it cannot tell whether the part answers
 * at all, only that we ask correctly.  Nothing on this bus has run on hardware.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/bk1080.h"
#include "driver/i2c_bus.h"

/* ------------------------------------------------------------- bus stub --- */

#define MAX_WR 96
#define MAX_RD 8
#define MAX_TXN 64

struct txn {
    uint8_t  wr[MAX_WR];
    unsigned wr_len;
    uint8_t  rd[MAX_RD];
    unsigned rd_len;
    int      ack[MAX_RD];
    unsigned ack_len;
    int      stopped;
};

static struct txn txns[MAX_TXN];
static unsigned   ntxn;

static uint8_t  rx_data[MAX_RD];
static unsigned rx_len, rx_pos;

static void bus_reset(void)
{
    ntxn = 0;
    memset(txns, 0, sizeof txns);
    rx_len = rx_pos = 0;
    memset(rx_data, 0, sizeof rx_data);
}

/* Queue the bytes the part will return for the next read(s). */
static void queue_read(uint8_t hi, uint8_t lo)
{
    rx_data[rx_len++] = hi;
    rx_data[rx_len++] = lo;
}

void i2c_bus_init(void) { }
void i2c_bus_start(void) { memset(&txns[ntxn], 0, sizeof txns[0]); }

void i2c_bus_stop(void)
{
    txns[ntxn].stopped = 1;
    if (ntxn + 1u < MAX_TXN)
        ntxn++;
}

bool i2c_bus_write_byte(uint8_t value)
{
    if (txns[ntxn].wr_len < MAX_WR)
        txns[ntxn].wr[txns[ntxn].wr_len++] = value;
    return true;
}

uint8_t i2c_bus_read_byte(void)
{
    uint8_t v = (rx_pos < rx_len) ? rx_data[rx_pos++] : 0xffu;

    if (txns[ntxn].rd_len < MAX_RD)
        txns[ntxn].rd[txns[ntxn].rd_len++] = v;
    return v;
}

void i2c_bus_send_ack(bool ack)
{
    if (txns[ntxn].ack_len < MAX_RD)
        txns[ntxn].ack[txns[ntxn].ack_len++] = ack ? 1 : 0;
}

/* The K1 API's `BK1080_Init`/`BK1080_SetFrequency` pace the part with
 * SYSTEM_DelayMs (driver/system.h).  A stub keeps the test instant. */
void SYSTEM_DelayMs(uint32_t Delay) { (void)Delay; }

/* ---------------------------------------------------------------- checks --- */

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

static void check_hex(uint32_t got, uint32_t want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL  %s: got 0x%X, want 0x%X\n", what, got, want);
    } else {
        printf("  ok    %s (0x%X)\n", what, want);
    }
}

static int txn_wr(const struct txn *t, unsigned i, uint8_t want)
{
    return i < t->wr_len && t->wr[i] == want;
}

/* -------------------------------------------------------------- framing --- */

static void test_framing(void)
{
    printf("framing\n");

    /* Read register 0x0a: 0x80, (0x0a << 1) | 1, then hi/lo, ACK then NACK. */
    bus_reset();
    queue_read(0x40, 0xab);
    check_hex(bk1080_read_reg(BK1080_REG_RSSI), 0x40ab, "read reg 0x0a returns the word");
    check(ntxn == 1, "one transaction");
    check(txn_wr(&txns[0], 0, 0x80), "read starts with the fixed device id 0x80");
    check_hex(txns[0].wr[1], 0x15, "control word is (0x0a << 1) | 1");
    check(txns[0].rd_len == 2, "two bytes read (one word)");
    check(txns[0].rd[0] == 0x40 && txns[0].rd[1] == 0xab, "high byte first");
    check(txns[0].ack_len == 2 && txns[0].ack[0] == 1 && txns[0].ack[1] == 0,
          "ACK after the first byte, NACK after the last");
    check(txns[0].stopped, "the read is bracketed by a stop");

    /* Write register 0x03: 0x80, 0x03 << 1, value MSB first. */
    bus_reset();
    bk1080_write_reg(BK1080_REG_CHANNEL, 0x80f0);
    check(ntxn == 1, "one write transaction");
    check(txn_wr(&txns[0], 0, 0x80), "write starts with the fixed device id 0x80");
    check_hex(txns[0].wr[1], 0x06, "control word is (0x03 << 1)");
    check(txns[0].wr_len == 4 && txns[0].wr[2] == 0x80 && txns[0].wr[3] == 0xf0,
          "value goes out MSB first");
    check(txns[0].stopped, "the write is bracketed by a stop");

    /* Register 0x32 would encode as (0x32 << 1) = 0x64 -- one 7-bit register. */
    bus_reset();
    bk1080_write_reg(0x32u, 0x285c);
    check_hex(txns[0].wr[1], 0x64, "reg 0x32 control word is 0x64");
}

/* ---------------------------------------------------------- init/config --- */

static void test_configure(void)
{
    unsigned block_len = 0;
    const uint8_t *block = bk1080_config_block(&block_len);

    static const uint8_t expected[] = {
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
    uint16_t v32 = 0;

    printf("init configuration\n");

    check_hex(block_len, sizeof expected, "the block is 68 bytes");
    check(memcmp(block, expected, sizeof expected) == 0,
          "the block matches the decompressed stock RAM at 0x20000088");

    bus_reset();
    bk1080_configure();
    check(ntxn == 3, "the config is the block plus two register writes");

    check(txns[0].wr_len == 2u + 68u, "the block is one transfer of 68 bytes");
    check(txn_wr(&txns[0], 0, 0x80) && txn_wr(&txns[0], 1, 0x00),
          "the block goes to register 0 (control word 0)");
    check(memcmp(&txns[0].wr[2], expected, sizeof expected) == 0,
          "the block bytes are unchanged on the wire");
    check(txns[0].stopped, "the block transfer is bracketed by a stop");

    check(txn_wr(&txns[1], 1, 0x64) && txns[1].wr[2] == 0x28 && txns[1].wr[3] == 0x5c,
          "then reg 0x32 = 0x285c");
    check(txn_wr(&txns[2], 1, 0x64) && txns[2].wr[2] == 0x28 && txns[2].wr[3] == 0xdc,
          "then reg 0x32 = 0x28dc");

    check(bk1080_config_reg32_writes() == 2, "two register-0x32 writes");
    bk1080_config_reg32_entry(0, &v32);
    check_hex(v32, 0x285c, "reg-0x32 entry 0");
    bk1080_config_reg32_entry(1, &v32);
    check_hex(v32, 0x28dc, "reg-0x32 entry 1");
    v32 = 0xFFFF;
    bk1080_config_reg32_entry(2, &v32);
    check_hex(v32, 0xFFFF, "out-of-range entry leaves the value alone");
}

/* ---------------------------------------------------------------- tuning --- */

static void check_tune(uint32_t freq_10hz, uint16_t want_reg5,
                       uint16_t want_chan, const char *what)
{
    bus_reset();
    bk1080_set_frequency(freq_10hz);

    check(ntxn == 3, what);
    check_hex(txns[0].wr[1], 0x0a, "  register 5 control word");
    check_hex((txns[0].wr[2] << 8) | txns[0].wr[3], want_reg5, "  register 5 value");
    check_hex(txns[1].wr[1], 0x06, "  register 3 control word (channel)");
    check_hex((txns[1].wr[2] << 8) | txns[1].wr[3], want_chan, "  register 3 channel");
    check_hex((txns[2].wr[2] << 8) | txns[2].wr[3],
              (uint16_t)(BK1080_CHANNEL_TUNE | want_chan), "  register 3 TUNE|channel");
}

static void test_tuning(void)
{
    printf("tuning\n");

    /* 100.0 MHz -> 1000 in 100 kHz units, band 01 (base 76), chan 240. */
    check_tune(10000000u, 0x0a5fu, 0x00f0u, "100.0 MHz");
    /* 87.5 MHz -> 875, band 01 (base 76), chan 115. */
    check_tune(8750000u, 0x0a5fu, 0x0073u, "87.5 MHz");
    /* 70.0 MHz -> 700 < 760, band 11 (base 64), chan 60. */
    check_tune(7000000u, 0x0adfu, 0x003cu, "70.0 MHz");

    /* Readback: READCHAN + band base, in the stock's 10 Hz units. */
    bus_reset();
    queue_read(0x00, 0xf0);            /* reg 0x0b = 240 */
    queue_read(0x00, 0x5f);            /* reg 0x05, band = 01 */
    check_hex(bk1080_get_frequency(), 10000000u, "100.0 MHz reads back");
    check(ntxn == 2, "the readback is two register reads");
    check_hex(txns[0].wr[1], 0x17, "reg 0x0b read control word (0x0b << 1) | 1");

    bus_reset();
    queue_read(0x00, 0x00);            /* reg 0x0b = 0 */
    queue_read(0x00, 0x00);            /* band = 00, base 87.5 */
    check_hex(bk1080_get_frequency(), 8750000u, "band 0 base is 87.5 MHz");

    bus_reset();
    queue_read(0x00, 0x3c);            /* reg 0x0b = 60 */
    queue_read(0x00, 0xc0);            /* band = 11, base 64 */
    check_hex(bk1080_get_frequency(), 7000000u, "band 3 base is 64 MHz");
}

/* --------------------------------------------------------- status / seek --- */

static void test_status(void)
{
    printf("status and seek\n");

    bus_reset();
    queue_read(0x40, 0xab);
    check(bk1080_seek_complete(), "STC (bit 14) is reported");
    bus_reset();
    queue_read(0x20, 0xab);
    check(bk1080_seek_failed(), "SF/BL (bit 13) is reported");
    bus_reset();
    queue_read(0x40, 0xab);
    check_hex(bk1080_get_rssi(), 0xab, "RSSI is the low byte");

    bus_reset();
    queue_read(0x12, 0x34);
    check_hex(bk1080_get_snr(), 0x4, "SNR is register 0x07's low nibble");
    check_hex(txns[0].wr[1], 0x0f, "reg 0x07 read control word (0x07 << 1) | 1");

    /* seek up: reg2 |= SEEK | SEEKUP | SKMODE. */
    bus_reset();
    queue_read(0x00, 0x01);
    bk1080_seek_up();
    check(ntxn == 2, "seek_up is a read-modify-write");
    check_hex((txns[1].wr[2] << 8) | txns[1].wr[3], 0x0701, "seek_up sets 0x0700");

    /* clear seek: reg2 &= ~SEEK. */
    bus_reset();
    queue_read(0x07, 0x01);
    bk1080_clear_seek();
    check_hex((txns[1].wr[2] << 8) | txns[1].wr[3], 0x0601, "clear_seek clears bit 8");

    /* clear tune: reg3 &= ~TUNE. */
    bus_reset();
    queue_read(0x80, 0xf0);
    bk1080_clear_tune();
    check_hex((txns[1].wr[2] << 8) | txns[1].wr[3], 0x00f0, "clear_tune clears bit 15");
}

/* --------------------------------------------- the K1/F4HWN driver API --- */

/* The K1 API shares the framing above; these checks pin its own register image
 * and its 100 kHz arithmetic, which is what app/fm.c drives. */
static void test_k1_api(void)
{
    printf("K1/F4HWN API\n");

    check_hex(BK1080_GetFreqLoLimit(0), 875, "K1 band 0 low limit");
    check_hex(BK1080_GetFreqLoLimit(1), 760, "K1 band 1 low limit");
    check_hex(BK1080_GetFreqLoLimit(3), 640, "K1 band 3 low limit");
    check_hex(BK1080_GetFreqHiLimit(1), 1080, "K1 band 1 high limit");
    check_hex(BK1080_GetFreqHiLimit(3), 760, "K1 band 3 high limit");

    /* Mute: register 2 = 0x4201, unmute = 0x0201. */
    bus_reset();
    BK1080_Mute(true);
    check(ntxn == 1 && txn_wr(&txns[0], 1, 0x04), "BK1080_Mute(true) writes reg 2");
    check_hex((txns[0].wr[2] << 8) | txns[0].wr[3], 0x4201, "  mute value");
    bus_reset();
    BK1080_Mute(false);
    check_hex((txns[0].wr[2] << 8) | txns[0].wr[3], 0x0201, "  unmute value");

    /* A register read is the same word read as the stock path. */
    bus_reset();
    queue_read(0x12, 0x34);
    check_hex(BK1080_ReadRegister(BK1080_REG_07), 0x1234, "BK1080_ReadRegister reg 0x07");
    check_hex(txns[0].wr[1], 0x0f, "  control word (0x07 << 1) | 1");

    /* Deviation: reg 0x07 / 16, and the base frequency is stored. */
    bus_reset();
    queue_read(0x12, 0x34);
    BK1080_GetFrequencyDeviation(1000);
    check_hex(BK1080_BaseFrequency, 1000, "BK1080_BaseFrequency is stored");
    check_hex(BK1080_FrequencyDeviation, 0x1234 / 16, "BK1080_FrequencyDeviation = reg7 / 16");

    /* BK1080_SetFrequency(1000, 1) -- 100.0 MHz in the K1's 100 kHz units:
     * band bits 01 into reg 5 (0x0A1F -> 0x0A5F), channel 240, then TUNE. */
    bus_reset();
    queue_read(0x0a, 0x1f);            /* reg 5 read */
    BK1080_SetFrequency(1000, 1);
    check(ntxn == 4, "set frequency: read+write reg 5, then two reg 3 writes");
    check_hex(txns[0].wr[1], 0x0b, "  reads reg 5 first (control (0x05<<1)|1)");
    check_hex((txns[1].wr[2] << 8) | txns[1].wr[3], 0x0a5f, "  reg 5 band = 01");
    check_hex((txns[2].wr[2] << 8) | txns[2].wr[3], 0x00f0, "  reg 3 channel 240");
    check_hex((txns[3].wr[2] << 8) | txns[3].wr[3], 0x80f0, "  reg 3 TUNE|channel");

    /* The RA89R stock power-up image: the 68-byte block, two register-0x32
     * writes, reg 5 = 0x0A1F, then the tune.  `BK1080_Init` applies the stock
     * image on every call, as the stock re-writes it each time FM is enabled. */
    bus_reset();
    queue_read(0x0a, 0x1f);            /* reg 5 read inside SetFrequency */
    BK1080_Init(1000, 1);
    check(ntxn == 8, "stock init: block + 2 reg 0x32 + reg 5 + 4 tune");
    check(txn_wr(&txns[0], 1, 0x00), "  block starts at reg 0");
    check_hex((txns[0].wr[2] << 8) | txns[0].wr[3], 0x0008, "  reg 0 = 0x0008");
    check_hex((txns[0].wr[4] << 8) | txns[0].wr[5], 0x1080, "  reg 1 = chip id 0x1080");
    check_hex((txns[1].wr[2] << 8) | txns[1].wr[3], 0x285c, "  reg 0x32 = 0x285c");
    check_hex((txns[2].wr[2] << 8) | txns[2].wr[3], 0x28dc, "  reg 0x32 = 0x28dc");
    check_hex((txns[3].wr[2] << 8) | txns[3].wr[3], 0x0a1f, "  then reg 5 = 0x0A1F");
    check_hex((txns[7].wr[2] << 8) | txns[7].wr[3], 0x80f0,
              "  and the 100.0 MHz tune (reg 3 TUNE|240)");

    /* Power-down: BK1080_Init0() writes reg 2 = 0x0241. */
    bus_reset();
    BK1080_Init0();
    check(ntxn == 1 && txn_wr(&txns[0], 1, 0x04), "BK1080_Init0() writes reg 2");
    check_hex((txns[0].wr[2] << 8) | txns[0].wr[3], 0x0241, "  power-down value");
}

int main(void)
{
    printf("bk1080 register-layer test (stub bus, no radio)\n\n");

    test_framing();
    test_configure();
    test_tuning();
    test_status();
    test_k1_api();

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
