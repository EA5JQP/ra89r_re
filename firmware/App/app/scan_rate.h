/* Scan throughput counter (console 'J' and its benchmark).
 *
 * Candidate count means RSSI probes completed; elapsed time is wall time since
 * the scan started, so scheduler cadence and the bit-banged bus work are
 * included.  Deliberately device-header free so it links on a PC
 * (tools/test_scan_rate.c).
 */
#ifndef APP_SCAN_RATE_H
#define APP_SCAN_RATE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t started_ms;
    uint32_t candidates;
    bool     started;
} scan_candidate_rate_t;

typedef struct {
    uint32_t candidates;
    uint32_t elapsed_ms;
    uint32_t candidates_per_second;
} scan_candidate_rate_stats_t;

void scan_candidate_rate_reset(scan_candidate_rate_t *counter, uint32_t now_ms);
void scan_candidate_rate_add(scan_candidate_rate_t *counter, uint32_t candidates);
void scan_candidate_rate_snapshot(const scan_candidate_rate_t *counter,
                                  uint32_t now_ms,
                                  scan_candidate_rate_stats_t *out);

#endif
