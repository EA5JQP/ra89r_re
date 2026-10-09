/* Dual-transceiver scan policy (see docs/ra89r_scan.md).
 *
 * The scanner produces one ordered stream of candidates.  In "Both" mode those
 * candidates are split between the two RF parts: the BK4829 takes even
 * ordinals, the BK4815 odd, and each lane keeps its own RSSI/noise-floor state
 * because the two chips report RSSI on different scales.
 *
 * This module owns only the host-testable decisions -- lane assignment and the
 * per-lane candidate test.  The RF work and the K1 scan lifecycle stay in
 * app/chFrScanner.c.  It is deliberately device-header free and gEeprom-free so
 * it links on a PC (tools/test_scan_dual.c).
 */
#ifndef APP_SCAN_DUAL_H
#define APP_SCAN_DUAL_H

#include <stdbool.h>
#include <stdint.h>

/* The stock hands the BK4815 the range above this split; at or below it the
 * BK4829 is the validated part, so a candidate there never goes to the BK4815
 * lane and is never lost to an unproven band. */
#define SCAN_DUAL_BK4815_MIN_FREQUENCY_10HZ 13400000u

typedef enum {
    SCAN_LANE_BK4829 = 0,
    SCAN_LANE_BK4815 = 1,
    SCAN_LANE_NONE   = 2
} scan_lane_chip_t;

/* One item the scanner will visit, in K1 order.  `ordinal` increases by one per
 * produced candidate; the lanes interleave on it. */
typedef struct {
    uint32_t ordinal;
    uint32_t frequency_10hz;
    uint16_t channel;          /* MR channel, or 0 for a frequency candidate */
    uint8_t  band;
    bool     is_memory_channel;
} scan_candidate_t;

/* Which chip scans this candidate in "Both" mode.  "Default" mode does not use
 * this.  Even eligible ordinals are the BK4829, odd the BK4815. */
scan_lane_chip_t scan_dual_assign_candidate(const scan_candidate_t *candidate);

/* One lane's fast-RSSI state.  The BK4829 reports a 9-bit RSSI (0x67) and the
 * BK4815 a 7-bit one (0x44), so floors and thresholds never cross lanes. */
typedef struct {
    uint16_t noise_floor;
    uint32_t last_frequency_10hz;
    uint32_t candidates;
    uint16_t last_rssi;
} scan_lane_state_t;

/* A lane with no floor learned yet. */
#define SCAN_LANE_FLOOR_UNSET 0xFFFFu

/* Update one lane's floor from a reading and report whether it is a candidate.
 * The first reading seeds the floor.  `squelch_open` is the lane's own squelch
 * mark; when it is 0 the caller has disabled the lane.  The margins are in the
 * lane's own RSSI units. */
bool scan_dual_rssi_candidate(scan_lane_state_t *lane, uint16_t rssi,
                              uint16_t squelch_open,
                              uint16_t noise_margin, uint16_t squelch_margin,
                              uint16_t weak_margin);

typedef struct {
    scan_lane_state_t lanes[2];
    uint32_t          next_ordinal;
    uint8_t           mode;          /* scan_transceiver_mode_t */
    bool              active;
    scan_lane_chip_t  selected_hit;
} scan_dual_state_t;

void scan_dual_reset(scan_dual_state_t *state);

#endif
