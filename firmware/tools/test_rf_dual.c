/* Host test for the dual-transceiver coordinator (App/driver/rf_dual.c).
 *
 * `rf_dual_choose` is pure, and `rf_dual_apply`'s tuning is checked against a
 * recording bus stub.  No hardware.
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_rf_dual.c \
 *       App/driver/rf_dual.c App/driver/bk4815.c -o /tmp/test_rf_dual
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "driver/rf_dual.h"
#include "driver/rf_bus.h"
#include "driver/rx.h"

/* ------------------------------------------------------------- bus stub --- */

static unsigned writes;

/* Recorded two-byte register writes, so the receive-entry sequence can be
 * checked by register rather than only by count. */
static uint8_t  rec_reg[64];
static uint16_t rec_val[64];
static unsigned rec_n;

void rf_bus_init(void) { }
void rf_bus_assert(uint32_t cs) { (void)cs; }
void rf_bus_release(uint32_t cs) { (void)cs; }
void rf_bus_delay(void) { }
void rf_bus_bit_out(int bit) { (void)bit; }
int  rf_bus_bit_in(void) { return 0; }
void rf_bus_write(uint32_t cs, uint8_t addr, const uint8_t *data, unsigned len)
{
    (void)cs;
    writes++;
    if (len == 2u && rec_n < (unsigned)(sizeof rec_reg)) {
        rec_reg[rec_n] = (uint8_t)(addr >> 1);
        rec_val[rec_n] = (uint16_t)((data[0] << 8) | data[1]);
        rec_n++;
    }
}
uint16_t rf_bus_read(uint32_t cs, uint8_t addr) { (void)cs; (void)addr; return 0; }

static void rec_reset(void) { rec_n = 0; }

static int rec_has(uint8_t reg, uint16_t value)
{
    unsigned i;

    for (i = 0; i < rec_n; i++)
        if (rec_reg[i] == reg && rec_val[i] == value)
            return 1;
    return 0;
}

/* ---------------------------------------------------------------- checks --- */

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) { fails++; printf("  FAIL %s\n", what); }
    else      printf("  ok   %s\n", what);
}

int main(void)
{
    rf_dual_roles_t r;
    unsigned        i;

    printf("rf dual\n");

    /* One VFO per chip: both tuned, whichever is selected. */
    r = rf_dual_choose(true, false, 0u);
    check(r.bk4829_vfo == 0 && r.bk4815_vfo == 1,
          "A=4829 B=4815, select A -> BK4829 on A, BK4815 on B");
    r = rf_dual_choose(true, false, 1u);
    check(r.bk4829_vfo == 0 && r.bk4815_vfo == 1,
          "A=4829 B=4815, select B -> same two tunings");

    /* Both VFOs on the BK4815: the selected one is still tuned. */
    r = rf_dual_choose(false, false, 0u);
    check(r.bk4829_vfo == -1 && r.bk4815_vfo == 0,
          "both 4815, select A -> BK4815 tuned to A");
    r = rf_dual_choose(false, false, 1u);
    check(r.bk4829_vfo == -1 && r.bk4815_vfo == 1,
          "both 4815, select B -> BK4815 tuned to B");

    /* Both VFOs on the BK4829: only the selected one. */
    r = rf_dual_choose(true, true, 0u);
    check(r.bk4829_vfo == 0 && r.bk4815_vfo == -1,
          "both 4829, select A -> BK4829 tuned to A only");

    /* Apply: a BK4815 role tunes it; none leaves it alone. */
    r = rf_dual_choose(false, false, 1u);
    writes = 0;
    rf_dual_apply(&r, 43350000u);
    check(writes == 4 && rf_dual_bk4815_active(),
          "BK4815 role tunes it (tune + band register)");

    r = rf_dual_choose(true, true, 0u);
    writes = 0;
    rf_dual_apply(&r, 0u);
    check(writes == 0 && !rf_dual_bk4815_active(),
          "no BK4815 role leaves it untouched");

    /* The stock's per-VFO receive entry (FUN_08016CEC), checked by register. */
    rec_reset();
    bk4815_receive_entry(true);
    check(rec_has(0x16, 0xb200) && rec_has(0x2b, 0x403f) &&
          rec_has(0x6d, 0x6618) && rec_has(0x73, 0x81d0),
          "BK4815 receive entry: the demod/AF block");
    check(rec_has(0x01, 0x8044), "wide channel -> 0x01 = 0x8044 (25 kHz)");
    check(rec_n >= 2u && rec_reg[rec_n - 2u] == 0x70 && rec_val[rec_n - 2u] == 0x0000 &&
          rec_reg[rec_n - 1u] == 0x70 && rec_val[rec_n - 1u] == 0xa000,
          "BK4815 receive entry ends with the 0x70 restart pair");

    rec_reset();
    bk4815_receive_entry(false);
    check(rec_has(0x01, 0x0044), "narrow channel -> 0x01 = 0x0044 (12.5 kHz)");

    /* Squelch marks: level 8 must reproduce the pairs measured on the radio,
     * level 0..9 must be monotone, and level 0 is the caller's "always open". */
    check(rx_squelch_open_mark(false, false, 8u) == 0xCFu &&
          rx_squelch_close_mark(false, false, 8u) == 0xB4u,
          "BK4829 level 8 = the measured pair 0xCF/0xB4");
    check(rx_squelch_open_mark(true, true, 8u) == 30u &&
          rx_squelch_close_mark(true, true, 8u) == 24u,
          "BK4815 UHF level 8 = the measured pair 30/24");
    check(rx_squelch_open_mark(true, false, 8u) == 20u &&
          rx_squelch_close_mark(true, false, 8u) == 14u,
          "BK4815 VHF level 8 = the measured pair 20/14");
    for (i = 2u; i <= 9u; i++) {
        const uint16_t prev = rx_squelch_open_mark(false, false, (uint8_t)(i - 1u));
        const uint16_t now  = rx_squelch_open_mark(false, false, (uint8_t)i);
        check(now > prev, "BK4829 opening mark rises with the squelch level");
        check(rx_squelch_close_mark(false, false, (uint8_t)i) < now,
              "BK4829 closing mark stays below the opening mark");
    }
    check(rx_squelch_open_mark(true, false, 1u) > 0u &&
          rx_squelch_open_mark(true, false, 1u) < rx_squelch_open_mark(true, false, 8u),
          "BK4815 VHF level 1 opens earlier than level 8");

    printf("\n%d failed\n", fails);
    return fails ? 1 : 0;
}
