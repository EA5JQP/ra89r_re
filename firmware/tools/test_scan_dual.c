/* Host tests for the dual-transceiver scan policy (App/app/scan_dual.c).
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_scan_dual.c \
 *       App/app/scan_dual.c -o /tmp/test_scan_dual
 *
 * scan_dual.c is deliberately device-header free and gEeprom-free, so this
 * links nothing else: the lane assignment and the per-lane RSSI state can be
 * checked on a PC, without a radio and without the K1 scan engine.
 */
#include <stdio.h>
#include <string.h>

#include "app/scan_dual.h"

static int failures;

static void check(bool ok, const char *what)
{
    printf("[%s] %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

static scan_candidate_t cand(uint32_t ordinal, uint32_t freq, uint16_t channel, bool mem)
{
    scan_candidate_t c;

    memset(&c, 0, sizeof c);
    c.ordinal           = ordinal;
    c.frequency_10hz    = freq;
    c.channel           = channel;
    c.is_memory_channel = mem;
    return c;
}

int main(void)
{
    /* Above the stock's split, even ordinals are the BK4829 and odd the
     * BK4815, so consecutive candidates interleave across the two lanes. */
    {
        scan_candidate_t c = cand(0, 14500000u, 0, false);
        check(scan_dual_assign_candidate(&c) == SCAN_LANE_BK4829, "ordinal 0 -> BK4829");
        c = cand(1, 14500000u, 0, false);
        check(scan_dual_assign_candidate(&c) == SCAN_LANE_BK4815, "ordinal 1 -> BK4815");
        c = cand(2, 14500000u, 0, false);
        check(scan_dual_assign_candidate(&c) == SCAN_LANE_BK4829, "ordinal 2 -> BK4829");
        c = cand(3, 14500000u, 0, false);
        check(scan_dual_assign_candidate(&c) == SCAN_LANE_BK4815, "ordinal 3 -> BK4815");
    }

    /* At or below the split, every candidate stays on the validated BK4829, so
     * none is silently lost to an unproven BK4815 band. */
    {
        scan_candidate_t c = cand(1, 13400000u, 0, false);   /* exactly the split */
        check(scan_dual_assign_candidate(&c) == SCAN_LANE_BK4829,
              "134 MHz ordinal 1 -> BK4829");
        c = cand(3, 10000000u, 0, false);
        check(scan_dual_assign_candidate(&c) == SCAN_LANE_BK4829,
              "10 MHz ordinal 3 -> BK4829");
    }

    /* A memory-list candidate is split by the same rule, whatever its channel. */
    {
        scan_candidate_t c = cand(5, 43000000u, 42u, true);
        check(scan_dual_assign_candidate(&c) == SCAN_LANE_BK4815,
              "list ordinal 5 -> BK4815");
        c = cand(6, 43000000u, 43u, true);
        check(scan_dual_assign_candidate(&c) == SCAN_LANE_BK4829,
              "list ordinal 6 -> BK4829");
    }

    /* Assignment is pure and never renumbers: a wrap back to a low ordinal
     * still maps by parity, and a NULL candidate is safe. */
    check(scan_dual_assign_candidate(NULL) == SCAN_LANE_BK4829, "NULL candidate -> BK4829");

    /* Reset leaves both lanes unseeded and clears the counter. */
    {
        scan_dual_state_t st;

        scan_dual_reset(&st);
        check(st.lanes[0].noise_floor == SCAN_LANE_FLOOR_UNSET,
              "reset leaves lane 0 floor unset");
        check(st.lanes[1].noise_floor == SCAN_LANE_FLOOR_UNSET,
              "reset leaves lane 1 floor unset");
        check(st.next_ordinal == 0u, "reset clears the ordinal counter");
    }

    /* Each lane learns its own floor from its own RSSI scale; a reading in one
     * lane never moves the other lane's floor. */
    {
        scan_dual_state_t st;

        scan_dual_reset(&st);
        (void)scan_dual_rssi_candidate(&st.lanes[0], 50u, 150u, 20u, 20u, 10u);
        check(st.lanes[0].noise_floor == 50u, "lane 0 floor learns down");
        check(st.lanes[1].noise_floor == SCAN_LANE_FLOOR_UNSET,
              "lane 1 floor stays unset after lane 0 samples");

        (void)scan_dual_rssi_candidate(&st.lanes[1], 90u, 150u, 20u, 20u, 10u);
        check(st.lanes[1].noise_floor == 90u, "lane 1 floor learns its own scale");
        check(st.lanes[0].noise_floor == 50u, "lane 0 floor is untouched by lane 1");
    }

    /* Candidate test: a reading well above the learned floor and the squelch
     * mark is a candidate; a reading at the floor is not. */
    {
        scan_dual_state_t st;

        scan_dual_reset(&st);
        check(scan_dual_rssi_candidate(&st.lanes[0], 100u, 150u, 20u, 20u, 10u) == false,
              "seed reading below squelch is not a candidate");
        check(st.lanes[0].noise_floor == 100u, "seed reading sets the floor");
        check(scan_dual_rssi_candidate(&st.lanes[0], 220u, 150u, 20u, 20u, 10u) == true,
              "reading above floor and squelch is a candidate");
        check(scan_dual_rssi_candidate(&st.lanes[0], 100u, 150u, 20u, 20u, 10u) == false,
              "reading at the floor is not a candidate");
    }

    /* Hit arbitration: the lower original ordinal wins when both lanes hit. */
    {
        scan_lane_chip_t sel;

        check(scan_dual_choose_hit(false, 0, false, 0, &sel) == false,
              "no lane hit -> no hit");
        check(scan_dual_choose_hit(true, 4u, false, 0, &sel) == true &&
              sel == SCAN_LANE_BK4829, "only BK4829 hit -> BK4829");
        check(scan_dual_choose_hit(false, 0, true, 5u, &sel) == true &&
              sel == SCAN_LANE_BK4815, "only BK4815 hit -> BK4815");
        check(scan_dual_choose_hit(true, 4u, true, 5u, &sel) == true &&
              sel == SCAN_LANE_BK4829, "both hit, 4829 earlier -> BK4829");
        check(scan_dual_choose_hit(true, 7u, true, 6u, &sel) == true &&
              sel == SCAN_LANE_BK4815, "both hit, 4815 earlier -> BK4815");
        check(scan_dual_choose_hit(true, 6u, true, 6u, &sel) == true &&
              sel == SCAN_LANE_BK4829, "tie -> BK4829");
        check(scan_dual_choose_hit(true, 4u, false, 0, NULL) == true,
              "NULL selected is safe");
    }

    /* The diagnostics snapshot mirrors the lane state. */
    {
        scan_dual_state_t st;
        scan_dual_stats_t stats;

        scan_dual_reset(&st);
        st.active = true;
        st.mode   = 1u;
        st.lanes[0].candidates          = 6u;
        st.lanes[0].last_frequency_10hz = 14500000u;
        st.lanes[0].last_rssi           = 40u;
        st.lanes[1].candidates          = 5u;
        st.lanes[1].last_frequency_10hz = 14600000u;
        st.lanes[1].last_rssi           = 12u;
        st.selected_hit                 = SCAN_LANE_BK4815;

        scan_dual_get_stats(&st, &stats);
        check(stats.active && stats.mode == 1u, "stats: active and mode");
        check(stats.candidates[0] == 6u && stats.candidates[1] == 5u,
              "stats: per-lane candidate counts");
        check(stats.last_frequency_10hz[0] == 14500000u &&
              stats.last_frequency_10hz[1] == 14600000u,
              "stats: per-lane last frequency");
        check(stats.last_rssi[0] == 40u && stats.last_rssi[1] == 12u,
              "stats: per-lane last RSSI");
        check(stats.selected_hit == SCAN_LANE_BK4815, "stats: selected hit");

        scan_dual_get_stats(NULL, &stats);
        check(!stats.active && stats.selected_hit == SCAN_LANE_NONE,
              "stats: NULL state is safe");
    }

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall scan_dual checks passed\n");
    return 0;
}
