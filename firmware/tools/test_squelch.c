/* Host test for the squelch counter (driver/rx.h, header-only).
 *
 * The stock's gate is not a comparator: FUN_0801D3F0's 0..10 counter opens at
 * 7 and closes below 3.  This test drives the pure state machine with the
 * sequences the radio showed: noise around the mark, a carrier edge, removal,
 * the level dead band and the force-open cases.
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_squelch.c -o /tmp/test_squelch
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/rx.h"

static int checks;
static int failures;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

static void step_n(rx_squelch_t *s, int n, bool present, bool absent)
{
    int i;

    for (i = 0; i < n; i++)
        rx_squelch_step(s, present, absent, false);
}

static void test_initial_and_open(void)
{
    rx_squelch_t s;

    printf("initial state and the opening edge\n");
    rx_squelch_reset(&s);
    check(!s.open && s.count == 0u, "reset is closed at count 0");

    step_n(&s, 6, true, false);
    check(!s.open && s.count == 6u, "6 present polls keep the gate closed");
    rx_squelch_step(&s, true, false, false);
    check(s.open && s.count == 7u, "the 7th present poll opens (count 7)");
}

static void test_hold_and_close(void)
{
    rx_squelch_t s;

    printf("between the marks (hold) and the closing edge\n");
    rx_squelch_reset(&s);
    step_n(&s, 7, true, false);
    check(s.open, "open first");

    step_n(&s, 10, false, false);
    check(s.open && s.count == 7u, "a reading between the marks holds the counter");

    step_n(&s, 4, false, true);
    check(s.open && s.count == 3u, "4 absent polls from 7 keep it open at count 3");
    rx_squelch_step(&s, false, true, false);
    check(!s.open && s.count == 2u, "the 5th absent poll closes (count 2)");
}

static void test_noise_does_not_chatter(void)
{
    rx_squelch_t s;
    int i;

    printf("noise around the mark must not chatter\n");
    rx_squelch_reset(&s);
    for (i = 0; i < 200; i++) {
        rx_squelch_step(&s, (i & 1) != 0, (i & 1) == 0, false);
        if (s.open) {
            printf("  opened at poll %d\n", i);
            break;
        }
    }
    check(!s.open, "alternating present/absent never opens");

    rx_squelch_reset(&s);
    for (i = 0; i < 100; i++) {
        step_n(&s, 3, true, false);
        step_n(&s, 3, false, true);
    }
    check(!s.open, "3-present / 3-absent bursts never open");

    rx_squelch_reset(&s);
    step_n(&s, 6, true, false);
    step_n(&s, 1, false, true);
    check(!s.open, "a 6-poll burst does not open; the counter falls back");
}

static void test_close_from_full(void)
{
    rx_squelch_t s;

    printf("closing from a full counter\n");
    rx_squelch_reset(&s);
    step_n(&s, 10, true, false);
    check(s.open && s.count == 10u, "10 present polls saturate the counter");

    step_n(&s, 7, false, true);
    check(s.open && s.count == 3u, "7 absent polls leave it open at 3");
    rx_squelch_step(&s, false, true, false);
    check(!s.open && s.count == 2u, "the 8th closes");
}

static void test_force_open(void)
{
    rx_squelch_t s;

    printf("SQL 0 / BK4815 AF override\n");
    rx_squelch_reset(&s);
    step_n(&s, 3, false, true);
    rx_squelch_step(&s, false, true, true);
    check(s.open && s.count == 10u, "force-open opens immediately at count 10");

    step_n(&s, 7, false, true);
    check(s.open && s.count == 3u, "release leaves the counter at 3 (still open)");
    rx_squelch_step(&s, false, true, false);
    check(!s.open, "it closes on the next absent poll after release");
}

static void test_scan_route(void)
{
    printf("scan route (which chip a scan's full tune lands on)\n");
    check(!rx_scan_route_is_4815(RX_SCAN_SOURCE_DEFAULT, false),
          "no override, BK4829 VFO -> BK4829");
    check(rx_scan_route_is_4815(RX_SCAN_SOURCE_DEFAULT, true),
          "no override, BK4815 VFO -> BK4815 (was never retuned)");
    check(rx_scan_route_is_4815(RX_SCAN_SOURCE_BK4815, false),
          "BK4815 hit on a BK4829 VFO -> BK4815");
    check(!rx_scan_route_is_4815(RX_SCAN_SOURCE_BK4829, true),
          "BK4829 hit on a BK4815 VFO -> BK4829");
}

static void test_scan_mark_scale(void)
{
    /* The scanner's BK4829 mark from the measured priors, SQL 5: it must sit
     * above the band's noise and well under a -30 dBm carrier (187 VHF,
     * 205+ UHF) -- the old floor-unknown fallback (0xCF) did not. */
    const uint16_t vhf = rx_squelch_open_mark_from_floor(false, false, 5u,
                                                         RX_FLOOR_PRIOR_4829_FM_VHF);
    const uint16_t uhf = rx_squelch_open_mark_from_floor(false, true, 5u,
                                                         RX_FLOOR_PRIOR_4829_FM_UHF);
    const uint16_t am  = rx_squelch_open_mark_from_floor(false, false, 5u,
                                                         RX_FLOOR_PRIOR_4829_AM_VHF);
    const uint16_t b15 = rx_squelch_open_mark_from_floor(true, true, 5u,
                                                         RX_FLOOR_PRIOR_4815);

    printf("scanner marks from the priors (SQL 5)\n");
    check(vhf > RX_FLOOR_PRIOR_4829_FM_VHF && vhf < 187u, "BK4829 VHF FM mark between floor and carrier");
    check(uhf > RX_FLOOR_PRIOR_4829_FM_UHF && uhf < 187u, "BK4829 UHF FM mark between floor and carrier");
    check(am  > RX_FLOOR_PRIOR_4829_AM_VHF && am  < 195u, "BK4829 AM mark between floor and carrier");
    check(b15 > RX_FLOOR_PRIOR_4815 && b15 < 47u,         "BK4815 mark between floor and carrier");
}

int main(void)
{
    printf("squelch counter test (the stock's 0..10 shape)\n\n");

    test_initial_and_open();
    test_hold_and_close();
    test_noise_does_not_chatter();
    test_close_from_full();
    test_force_open();
    test_scan_route();
    test_scan_mark_scale();

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
