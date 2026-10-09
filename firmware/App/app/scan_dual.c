/* Dual-transceiver scan policy.  See App/app/scan_dual.h and docs/ra89r_scan.md.
 *
 * Device-header free and gEeprom-free: the decisions here are pure so they can
 * be checked on a PC (tools/test_scan_dual.c).
 */
#include "app/scan_dual.h"

#include <string.h>

static uint16_t scan_dual_sat_add(uint16_t value, uint16_t add)
{
    return (value > (uint16_t)(0xFFFFu - add)) ? 0xFFFFu : (uint16_t)(value + add);
}

static uint16_t scan_dual_sat_sub(uint16_t value, uint16_t sub)
{
    return (value > sub) ? (uint16_t)(value - sub) : 0u;
}

scan_lane_chip_t scan_dual_assign_candidate(const scan_candidate_t *candidate)
{
    if (candidate == 0)
        return SCAN_LANE_BK4829;

    if (candidate->frequency_10hz <= SCAN_DUAL_BK4815_MIN_FREQUENCY_10HZ)
        return SCAN_LANE_BK4829;

    return (candidate->ordinal & 1u) ? SCAN_LANE_BK4815 : SCAN_LANE_BK4829;
}

bool scan_dual_rssi_candidate(scan_lane_state_t *lane, uint16_t rssi,
                              uint16_t squelch_open,
                              uint16_t noise_margin, uint16_t squelch_margin,
                              uint16_t weak_margin)
{
    uint16_t noise_trigger;
    uint16_t squelch_trigger;
    uint16_t rssi_with_margin;

    if (lane == 0)
        return false;

    lane->last_rssi = rssi;

    if (lane->noise_floor == SCAN_LANE_FLOOR_UNSET) {
        /* Seed the floor from the first reading.  The squelch mark decides
         * whether that reading is already a candidate. */
        lane->noise_floor = rssi;
        return squelch_open > 0u && rssi >= squelch_open;
    }

    noise_trigger   = scan_dual_sat_add(lane->noise_floor, noise_margin);
    squelch_trigger = scan_dual_sat_sub(squelch_open, squelch_margin);
    rssi_with_margin = scan_dual_sat_add(rssi, weak_margin);

    if ((rssi >= noise_trigger && rssi >= squelch_trigger) ||
        (rssi_with_margin >= noise_trigger && rssi_with_margin >= squelch_trigger))
        return true;

    if (rssi < lane->noise_floor)
        lane->noise_floor = rssi;
    else
        lane->noise_floor = (uint16_t)((7u * lane->noise_floor + rssi + 4u) >> 3);

    return false;
}

void scan_dual_reset(scan_dual_state_t *state)
{
    if (state == 0)
        return;

    memset(state, 0, sizeof *state);
    state->lanes[0].noise_floor = SCAN_LANE_FLOOR_UNSET;
    state->lanes[1].noise_floor = SCAN_LANE_FLOOR_UNSET;
    state->selected_hit         = SCAN_LANE_NONE;
}

bool scan_dual_choose_hit(bool hit_4829, uint32_t ordinal_4829,
                          bool hit_4815, uint32_t ordinal_4815,
                          scan_lane_chip_t *selected)
{
    if (!hit_4829 && !hit_4815)
        return false;

    if (hit_4829 && (!hit_4815 || ordinal_4829 <= ordinal_4815)) {
        if (selected)
            *selected = SCAN_LANE_BK4829;
    } else {
        if (selected)
            *selected = SCAN_LANE_BK4815;
    }

    return true;
}
