/* Host test for the two RF register layers, without a radio.
 *
 * This replaces the bus with a recording stub, so it checks the thing a
 * bring-up cannot check by looking at the screen: the exact bytes each part
 * will see, and whether the identity handshake is wired the way the stock's own
 * detect is (register 0 -> 0x4829 / 0x4816).
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_rf.c App/driver/bk4829.c \
 *       App/driver/bk4815.c -o /tmp/test_rf && /tmp/test_rf
 *
 * It is not a substitute for the radio: it cannot tell whether the part answers
 * at all, only that we ask correctly.  `R` on the console is the other half.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "driver/bk4819.h"
#include "driver/bk4829.h"
#include "driver/bk4815.h"
#include "driver/rf_bus.h"

/* ------------------------------------------------------------- bus stub --- */

#define MAX_XFER 64

struct xfer {
    int      is_read;               /* 0 = write, 1 = read */
    uint32_t cs;
    uint8_t  addr;
    uint8_t  data[MAX_XFER];
    unsigned len;                   /* bytes written (writes only) */
    uint16_t answer;                /* what a read returns */
};

static struct xfer log_[512];
static unsigned    log_len;
static int         bit_log[256];
static unsigned    bit_len;
static uint16_t    id_bk4829 = BK4829_ID;
static uint16_t    id_bk4815 = BK4815_ID;
static uint16_t    other_bk4829;
static uint16_t    other_bk4815;

static void log_reset(void)
{
    log_len = 0;
    bit_len = 0;
    memset(log_, 0, sizeof log_);
}

/* The manual-mode pieces bk4819.c uses for its frame fragments. */
void rf_bus_init(void) { }

void rf_bus_assert(uint32_t cs) { (void)cs; }
void rf_bus_release(uint32_t cs) { (void)cs; }
void rf_bus_delay(void) { }

void rf_bus_bit_out(int bit)
{
    if (bit_len < sizeof bit_log / sizeof bit_log[0])
        bit_log[bit_len++] = bit ? 1 : 0;
}

int rf_bus_bit_in(void) { return 0; }

void rf_bus_write(uint32_t cs, uint8_t addr, const uint8_t *data, unsigned len)
{
    struct xfer *x = &log_[log_len++];

    x->is_read = 0;
    x->cs = cs;
    x->addr = addr;
    x->len = (len > MAX_XFER) ? MAX_XFER : len;
    memcpy(x->data, data, x->len);
}

uint16_t rf_bus_read(uint32_t cs, uint8_t addr)
{
    struct xfer *x = &log_[log_len++];
    uint16_t v;

    x->is_read = 1;
    x->cs = cs;
    x->addr = addr;

    if (cs == BK4829_CS_PIN)
        v = (addr & 0x7fu) == BK4829_REG_ID ? id_bk4829 : other_bk4829;
    else
        v = ((addr >> 1) & 0x7fu) == BK4815_REG_ID ? id_bk4815 : other_bk4815;

    x->answer = v;
    return v;
}

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

static int xfer_is(const struct xfer *x, uint32_t cs, uint8_t addr)
{
    return x->cs == cs && x->addr == addr;
}

/* The identity handshake: this is the "version" read the bring-up relies on. */
static void test_identity(void)
{
    printf("identity\n");

    log_reset();
    check(bk4829_detect(), "bk4829 detect true when reg 0 reads 0x4829");
    check(log_len == 1 && log_[0].is_read, "bk4829 detect is one read");

    log_reset();
    check(bk4815_detect(), "bk4815 detect true when reg 0 reads 0x4816");
    check(log_len == 1 && log_[0].is_read, "bk4815 detect is one read");

    id_bk4829 = 0x1111;
    log_reset();
    check(!bk4829_detect(), "bk4829 detect false on a wrong id");
    id_bk4829 = BK4829_ID;

    id_bk4815 = 0x2222;
    log_reset();
    check(!bk4815_detect(), "bk4815 detect false on a wrong id");
    id_bk4815 = BK4815_ID;
}

/* Framing: the two parts do not share an address encoding, and this is where a
 * copy-paste between them would show up. */
static void test_framing(void)
{
    printf("framing\n");

    log_reset();
    (void)bk4829_read_reg(0x00);
    check(log_len == 1 && xfer_is(&log_[0], BK4829_CS_PIN, 0x80),
          "bk4829 read reg 0 sends 0x80 on PB8");

    log_reset();
    (void)bk4829_read_reg(0x33);
    check(log_len == 1 && xfer_is(&log_[0], BK4829_CS_PIN, 0xB3),
          "bk4829 read reg 0x33 sends 0x33|0x80");

    log_reset();
    bk4829_write_reg(0x7d, 0xE958);
    check(log_len == 1 && xfer_is(&log_[0], BK4829_CS_PIN, 0x7D),
          "bk4829 write reg 0x7d sends 0x7d (no read flag)");
    check(log_[0].len == 2 && log_[0].data[0] == 0xE9 && log_[0].data[1] == 0x58,
          "bk4829 write sends the value MSB first");

    log_reset();
    (void)bk4815_read_reg(0x00);
    check(log_len == 1 && xfer_is(&log_[0], BK4815_CS_PIN, 0x01),
          "bk4815 read reg 0 sends 0x01 on PB13");

    log_reset();
    (void)bk4815_read_reg(0x75);
    check(log_len == 1 && xfer_is(&log_[0], BK4815_CS_PIN, 0xEB),
          "bk4815 read reg 0x75 sends (0x75<<1)|1");

    log_reset();
    bk4815_write_reg(0x40, 0x8000);
    check(log_len == 1 && xfer_is(&log_[0], BK4815_CS_PIN, 0x80),
          "bk4815 write reg 0x40 sends 0x40<<1");
    check(log_[0].len == 2 && log_[0].data[0] == 0x80 && log_[0].data[1] == 0x00,
          "bk4815 write sends the value MSB first");
}

/* The BK4829's boot sequence: the parts a wrong transcription would break. */
static void test_bk4829_config(void)
{
    unsigned i, writes;
    int saw_id_reset = 0, saw_7d = 0;
    uint16_t v7d = 0;

    printf("bk4829 boot configuration\n");

    log_reset();
    bk4829_configure();
    writes = bk4829_config_writes();

    check_hex(writes, 39, "39 register values");
    check(log_len == writes, "one transfer per table entry");
    check(xfer_is(&log_[0], BK4829_CS_PIN, 0x00) && log_[0].data[0] == 0x80,
          "first write is reg 0 = 0x8000 (the reset the stock starts with)");
    check(xfer_is(&log_[1], BK4829_CS_PIN, 0x00) && log_[1].data[0] == 0x00,
          "second write is reg 0 = 0x0000");
    check(xfer_is(&log_[writes - 1], BK4829_CS_PIN, 0x47),
          "last write is reg 0x47");

    for (i = 0; i < writes; i++) {
        if (log_[i].addr == 0x00 && log_[i].data[0] == 0x00 && log_[i].data[1] == 0x00)
            saw_id_reset++;
        if (log_[i].addr == 0x7d) {
            saw_7d++;
            v7d = (uint16_t)((log_[i].data[0] << 8) | log_[i].data[1]);
        }
    }
    check(saw_id_reset == 1, "reg 0 reset appears once after the 0x8000");
    check(saw_7d == 1, "reg 0x7d appears once (the tweak is folded in)");
    check_hex(v7d, 0xE958, "reg 0x7d carries the stock's effective value");
}

/* The BK4815's boot sequence: the block must be one select pulse, not 18. */
static void test_bk4815_config(void)
{
    unsigned block_len = 0, writes, i;
    const uint8_t *block = bk4815_config_block(&block_len);
    static const uint8_t expected[] = {
        0x6f, 0xc0, 0x0e, 0x3d, 0xb0, 0x41, 0xf7, 0x70, 0xf2, 0x74, 0x08, 0xf0,
        0xff, 0x33, 0xc3, 0xfa, 0xa2, 0xa3, 0x88, 0x00, 0x06, 0x03, 0x09, 0xfd,
        0x58, 0x17, 0x90, 0xa3, 0x88, 0xf9, 0x58, 0x00, 0x41, 0x5c, 0x08, 0xa0,
    };

    printf("bk4815 boot configuration\n");

    check_hex(block_len, sizeof expected, "the register-2 block is 36 bytes");
    check(memcmp(block, expected, sizeof expected) == 0,
          "the block matches the stock image at 0x08024E40");

    log_reset();
    bk4815_configure();
    writes = bk4815_config_writes();

    check_hex(writes, 33, "33 single writes after the block");
    check(log_len == writes + 1, "the block is one transfer, not 18");
    check(log_len > 1 && log_[0].len == 36 && xfer_is(&log_[0], BK4815_CS_PIN, 0x04),
          "the block goes out at address 2<<1 with all 36 bytes in one select");
    check(xfer_is(&log_[1], BK4815_CS_PIN, 0xE0) && log_[1].data[0] == 0xA0,
          "first single write is reg 0x70 = 0xa000");
    check(xfer_is(&log_[writes], BK4815_CS_PIN, 0x18),
          "last single write is reg 0x0c");

    for (i = 0; i < log_len; i++) {
        if (log_[i].cs != BK4815_CS_PIN) {
            check(0, "every transfer is on the BK4815 select");
            return;
        }
    }
    check(bk4815_ram_sourced_writes() == 3,
          "three writes are known to be placeholders (0x4c, 0x55, 0x62)");
}

/* The accessors the console's read-back verification walks.  If these are wrong
 * the verification lies, so they get their own check. */
static void test_accessors(void)
{
    uint8_t reg = 0;
    uint16_t value = 0;
    unsigned n;

    printf("table accessors\n");

    n = bk4829_config_writes();
    bk4829_config_entry(0, &reg, &value);
    check_hex(reg, 0x00, "bk4829 entry 0 reg");
    check_hex(value, 0x8000, "bk4829 entry 0 value");

    bk4829_config_entry(n - 1, &reg, &value);
    check_hex(reg, 0x47, "bk4829 last entry reg");
    check_hex(value, 0x6042, "bk4829 last entry value");

    reg = 0xFF;
    value = 0xFFFF;
    bk4829_config_entry(n, &reg, &value);          /* out of range: untouched */
    check_hex(reg, 0xFF, "bk4829 out-of-range entry leaves reg alone");
    check_hex(value, 0xFFFF, "bk4829 out-of-range entry leaves value alone");

    n = bk4815_config_writes();
    bk4815_config_entry(0, &reg, &value);
    check_hex(reg, 0x70, "bk4815 entry 0 reg");
    check_hex(value, 0xA000, "bk4815 entry 0 value");
    bk4815_config_entry(n - 1, &reg, &value);
    check_hex(reg, 0x0C, "bk4815 last entry reg");
    check_hex(value, 0x0A03, "bk4815 last entry value");
}

/* The K1-compatible layer: same bus, the K1's register sequences. */
static void test_k1_interface(void)
{
    unsigned i;
    static const int expect_a5[8] = { 1, 0, 1, 0, 0, 1, 0, 1 };

    printf("k1-compatible bk4819 layer\n");

    /* Its init must start with the same register-0 reset pair the stock uses. */
    log_reset();
    BK4819_Init();
    check(xfer_is(&log_[0], BK4829_CS_PIN, 0x00) && log_[0].data[0] == 0x80,
          "BK4819_Init starts with reg 0 = 0x8000 (as the stock does)");
    check(xfer_is(&log_[1], BK4829_CS_PIN, 0x00) && log_[1].data[0] == 0x00,
          "and then reg 0 = 0x0000");

    /* 145.7500 MHz in 10 Hz units, split across 0x38/0x39 exactly as the stock
     * writes it (FUN_08017158 does the same with no scaling). */
    log_reset();
    BK4819_SetFrequency(14575000u);
    check(log_len == 2, "SetFrequency is two register writes");
    check(xfer_is(&log_[0], BK4829_CS_PIN, 0x38) && log_[0].data[0] == 0x65 &&
          log_[0].data[1] == 0x98, "reg 0x38 = 0x6598 (low word)");
    check(xfer_is(&log_[1], BK4829_CS_PIN, 0x39) && log_[1].data[0] == 0x00 &&
          log_[1].data[1] == 0xDE, "reg 0x39 = 0x00DE (high word)");

    /* RSSI is the stock's register too. */
    other_bk4829 = 0x7BCD;
    log_reset();
    check_hex(BK4819_GetRSSI(), 0x7BCD & 0x01FF, "GetRSSI masks reg 0x67 to 9 bits");
    check(log_len == 1 && xfer_is(&log_[0], BK4829_CS_PIN, 0xE7),
          "GetRSSI reads reg 0x67 with the read flag");

    /* The 0x30/0x47 write caches must suppress a repeated write. */
    log_reset();
    BK4819_WriteRegister(BK4819_REG_30, 0x1234);
    BK4819_WriteRegister(BK4819_REG_30, 0x1234);
    check(log_len == 1, "a repeated reg 0x30 write is cached away");
    log_reset();
    BK4819_WriteRegister(BK4819_REG_47, 0x5678);
    BK4819_WriteRegister(BK4819_REG_47, 0x9999);
    check(log_len == 2, "a changed reg 0x47 write is not");

    /* Frame fragment: MSB first, and only the bits. */
    log_reset();
    BK4819_WriteU8(0xA5);
    check(bit_len == 8, "WriteU8 shifts 8 bits");
    for (i = 0; i < 8; i++)
        if (bit_log[i] != expect_a5[i])
            break;
    check(i == 8, "WriteU8 sends 0xA5 MSB first");

    /* The gEeprom replacement: gains come from the driver's own setter. */
    log_reset();
    BK4819_SetRxAudioGains(5, 3);
    BK4819_SetRxAudioGain();
    check(log_len == 1 && xfer_is(&log_[0], BK4829_CS_PIN, 0x48),
          "SetRxAudioGain writes reg 0x48");
    check(log_[0].data[0] == 0xB0 && log_[0].data[1] == 0x53,
          "with the driver's gains packed in ((11<<12)|(5<<4)|3)");
}

int main(void)
{
    printf("rf register-layer test (stub bus, no radio)\n\n");

    test_identity();
    test_framing();
    test_bk4829_config();
    test_bk4815_config();
    test_accessors();
    test_k1_interface();

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
