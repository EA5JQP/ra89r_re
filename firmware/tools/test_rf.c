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
static uint16_t    id_bk4829 = BK4829_ID;
static uint16_t    id_bk4815 = BK4815_ID;
static uint16_t    other_bk4829;
static uint16_t    other_bk4815;

static void log_reset(void)
{
    log_len = 0;
    memset(log_, 0, sizeof log_);
}

void rf_bus_init(void) { }

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

int main(void)
{
    printf("rf register-layer test (stub bus, no radio)\n\n");

    test_identity();
    test_framing();
    test_bk4829_config();
    test_bk4815_config();

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
