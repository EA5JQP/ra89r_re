/* Scan throughput counter.  See App/app/scan_rate.h.
 *
 * Device-header free, so it is checked on a PC (tools/test_scan_rate.c).
 */
#include "app/scan_rate.h"

#include <string.h>

void scan_candidate_rate_reset(scan_candidate_rate_t *counter, uint32_t now_ms)
{
    if (counter == 0)
        return;

    counter->started_ms = now_ms;
    counter->candidates = 0;
    counter->started    = true;
}

void scan_candidate_rate_add(scan_candidate_rate_t *counter, uint32_t candidates)
{
    if (counter == 0 || !counter->started)
        return;

    if (UINT32_MAX - counter->candidates < candidates)
        counter->candidates = UINT32_MAX;
    else
        counter->candidates += candidates;
}

void scan_candidate_rate_snapshot(const scan_candidate_rate_t *counter,
                                  uint32_t now_ms,
                                  scan_candidate_rate_stats_t *out)
{
    uint64_t rate;

    if (out == 0)
        return;

    memset(out, 0, sizeof *out);
    if (counter == 0 || !counter->started)
        return;

    out->candidates = counter->candidates;
    /* Unsigned subtraction also handles the SysTick millisecond counter wrap. */
    out->elapsed_ms = now_ms - counter->started_ms;
    if (out->elapsed_ms == 0u)
        return;

    rate = ((uint64_t)counter->candidates * 1000u) / out->elapsed_ms;
    out->candidates_per_second = (rate > UINT32_MAX) ? UINT32_MAX : (uint32_t)rate;
}
