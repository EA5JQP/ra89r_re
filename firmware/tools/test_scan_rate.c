/* Host test for the scan throughput counter (App/app/scan_rate.c).
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_scan_rate.c \
 *       App/app/scan_rate.c -o /tmp/test_scan_rate
 */
#include <stdio.h>
#include <string.h>

#include "app/scan_rate.h"

static int failures;

static void check(bool ok, const char *what)
{
    printf("[%s] %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

int main(void)
{
    /* Candidate throughput uses wall-clock time since scan start, so the two
     * modes can be compared despite their different work per scheduler batch. */
    {
        scan_candidate_rate_t counter;
        scan_candidate_rate_stats_t stats;

        scan_candidate_rate_reset(&counter, 100u);
        scan_candidate_rate_add(&counter, 60u);
        scan_candidate_rate_snapshot(&counter, 1100u, &stats);
        check(stats.candidates == 60u && stats.elapsed_ms == 1000u &&
              stats.candidates_per_second == 60u,
              "rate: candidates per second over one second");

        scan_candidate_rate_add(&counter, 30u);
        scan_candidate_rate_snapshot(&counter, 2500u, &stats);
        check(stats.candidates == 90u && stats.elapsed_ms == 2400u &&
              stats.candidates_per_second == 37u,
              "rate: cumulative elapsed wall time");

        scan_candidate_rate_reset(&counter, 500u);
        scan_candidate_rate_snapshot(&counter, 500u, &stats);
        check(stats.elapsed_ms == 0u && stats.candidates_per_second == 0u,
              "rate: zero elapsed avoids divide by zero");

        scan_candidate_rate_reset(&counter, UINT32_MAX - 499u);
        scan_candidate_rate_add(&counter, 20u);
        scan_candidate_rate_snapshot(&counter, 500u, &stats);
        check(stats.elapsed_ms == 1000u && stats.candidates_per_second == 20u,
              "rate: SysTick millisecond wrap is handled");

        scan_candidate_rate_reset(NULL, 0u);
        scan_candidate_rate_add(NULL, 10u);
        scan_candidate_rate_snapshot(NULL, 10u, &stats);
        check(stats.candidates == 0u && stats.candidates_per_second == 0u,
              "rate: NULL counter is safe");
    }

    if (failures == 0)
        printf("all scan_rate checks passed\n");
    return failures ? 1 : 0;
}
