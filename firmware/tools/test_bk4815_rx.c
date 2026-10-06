/* Host test for the BK4815 receive service (App/driver/bk4815_rx.c).
 *
 * The bus is a recording stub, so this checks the exact bytes the part will
 * see for a known frequency against the stock's `FUN_0801703c`.  It cannot
 * tell whether the chip then receives -- only the radio can (see the plan's
 * Task 5).
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_bk4815_rx.c \
 *       App/driver/bk4815.c App/driver/bk4815_rx.c -o /tmp/test_bk4815_rx
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "driver/bk4815.h"
#include "driver/bk4815_rx.h"
#include "driver/rf_bus.h"

/* ------------------------------------------------------------- bus stub --- */

struct xfer {
    int      is_read;
    uint32_t cs;
    uint8_t  addr;
    uint8_t  data[8];
    unsigned len;
};

static struct xfer log_[16];
static unsigned    log_len;

void rf_bus_init(void) { }
void rf_bus_assert(uint32_t cs) { (void)cs; }
void rf_bus_release(uint32_t cs) { (void)cs; }
void rf_bus_delay(void) { }
void rf_bus_bit_out(int bit) { (void)bit; }
int  rf_bus_bit_in(void) { return 0; }

void rf_bus_write(uint32_t cs, uint8_t addr, const uint8_t *data, unsigned len)
{
    struct xfer *x = &log_[log_len++];

    x->is_read = 0;
    x->cs = cs;
    x->addr = addr;
    x->len = (len > sizeof x->data) ? sizeof x->data : len;
    memcpy(x->data, data, x->len);
}

uint16_t rf_bus_read(uint32_t cs, uint8_t addr)
{
    struct xfer *x = &log_[log_len++];

    x->is_read = 1;
    x->cs = cs;
    x->addr = addr;
    return 0;
}

/* ---------------------------------------------------------------- checks --- */

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) { fails++; printf("  FAIL %s\n", what); }
    else      printf("  ok   %s\n", what);
}

static int xfer_is(unsigned i, uint8_t addr)
{
    return i < log_len && log_[i].cs == BK4815_CS_PIN && log_[i].addr == addr;
}

int main(void)
{
    printf("bk4815 rx\n");

    /* 145.75 MHz: <= 187 MHz -> index 3, divider 24, word 0x8689D89D. */
    log_len = 0;
    bk4815_rx_tune(14575000u);

    check(xfer_is(0, (uint8_t)(4u << 1)) && log_[0].len == 2 &&
          log_[0].data[0] == 0xB1 && log_[0].data[1] == 0xC1,
          "reg 4 = (3<<7)|0xB041 for 145.75 MHz");
    check(xfer_is(1, (uint8_t)(BK4815_REG_OPCTRL << 1)) && log_[1].len == 6 &&
          log_[1].data[0] == 0xA0 && log_[1].data[1] == 0x00 &&
          log_[1].data[2] == 0x86 && log_[1].data[3] == 0x89 &&
          log_[1].data[4] == 0xD8 && log_[1].data[5] == 0x9D,
          "0x70 block = A000 8689 D89D (RX + freq word)");
    check(xfer_is(2, (uint8_t)(BK4815_REG_CAL << 1)) && log_[2].len == 4 &&
          log_[2].data[0] == 0xFF && log_[2].data[1] == 0xDF &&
          log_[2].data[2] == 0xA0 && log_[2].data[3] == 0x37,
          "0x7E block = FFDF A037 (index-3 calibration)");

    /* 433.5 MHz: > 383 MHz -> index 0, divider 8. */
    log_len = 0;
    bk4815_rx_tune(43350000u);
    check(xfer_is(0, (uint8_t)(4u << 1)) && log_[0].data[0] == 0xB0 &&
          log_[0].data[1] == 0x41,
          "reg 4 = (0<<7)|0xB041 for 433.5 MHz");
    check(xfer_is(2, (uint8_t)(BK4815_REG_CAL << 1)) &&
          log_[2].data[0] == 0xFF && log_[2].data[1] == 0xF5 &&
          log_[2].data[2] == 0x35 && log_[2].data[3] == 0x68,
          "0x7E block = FFF5 3568 (index-0 calibration)");

    printf("\n%d failed\n", fails);
    return fails ? 1 : 0;
}
