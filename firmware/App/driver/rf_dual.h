/* Dual-transceiver coordinator.
 *
 * The RA89R carries two BK481x transceivers on one bus.  This decides which
 * VFO is served by which part and keeps the second (BK4815) tuned, so both can
 * be active for their own VFO.  The BK4829 stays the primary -- it is the one
 * the K1 `BK4819_*` layer drives, and the only one with a validated transmit
 * path -- and the BK4815 is a receive-only secondary.
 *
 * Device-header free and gEeprom-free on purpose: `rf_dual_choose` is pure and
 * host-testable, and the caller resolves `AUTO` and reads the VFOs.
 *
 * Transmit is always the BK4829, at the selected TX VFO's frequency: it is the
 * only part with a validated transmit path (`driver/tx.c`), so a VFO whose
 * receive transceiver is the BK4815 still transmits through the BK4829.  The
 * per-VFO choice selects the *receive* part; it does not move transmit onto an
 * unproven path.
 */
#ifndef DRIVER_RF_DUAL_H
#define DRIVER_RF_DUAL_H

#include <stdbool.h>
#include <stdint.h>

/* Which VFO (0/1) each part serves, or -1 for "not active". */
typedef struct {
    int8_t primary;     /* the VFO on the BK4829 (RX and TX) */
    int8_t secondary;   /* the VFO on the BK4815 (RX only) */
} rf_dual_roles_t;

/* Resolve the roles from whether each VFO's transceiver is the BK4829
 * (`a_4829`/`b_4829`) and the VFO the receiver currently follows.  When both
 * VFOs resolve to the BK4829 only the receiver's VFO is active; when neither
 * does there is no transmit-capable part and nothing is active. */
rf_dual_roles_t rf_dual_choose(bool a_4829, bool b_4829, uint8_t rx_vfo);

/* Apply the roles: tune the BK4815 to `sec_rx_freq_10hz` (10 Hz units) when a
 * secondary exists, else leave it alone.  Records whether the secondary is
 * active for `rf_dual_secondary_active`. */
void rf_dual_apply(const rf_dual_roles_t *roles, uint32_t sec_rx_freq_10hz);

bool     rf_dual_secondary_active(void);
uint16_t rf_dual_secondary_rssi(void);

#endif /* DRIVER_RF_DUAL_H */
