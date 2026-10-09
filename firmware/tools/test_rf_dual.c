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

/* ------------------------------------------------------------- bus stub --- */

static unsigned writes;

void rf_bus_init(void) { }
void rf_bus_assert(uint32_t cs) { (void)cs; }
void rf_bus_release(uint32_t cs) { (void)cs; }
void rf_bus_delay(void) { }
void rf_bus_bit_out(int bit) { (void)bit; }
int  rf_bus_bit_in(void) { return 0; }
void rf_bus_write(uint32_t cs, uint8_t addr, const uint8_t *data, unsigned len)
{
    (void)cs; (void)addr; (void)data; (void)len;
    writes++;
}
uint16_t rf_bus_read(uint32_t cs, uint8_t addr) { (void)cs; (void)addr; return 0; }

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

    printf("\n%d failed\n", fails);
    return fails ? 1 : 0;
}
