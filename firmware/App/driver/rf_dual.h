/* Dual-transceiver coordinator.
 *
 * The RA89R carries two BK481x transceivers on one bus.  This decides which VFO
 * each part is tuned to.  The rule is: the **selected** receive VFO's chip is
 * always tuned to it, and the other chip is tuned to the other VFO only when the
 * two VFOs name different chips (one part cannot hold two frequencies).
 *
 * Transmit is always the BK4829, at the selected TX VFO's frequency: it is the
 * only part with a validated transmit path (`driver/tx.c`), so a VFO whose
 * receive transceiver is the BK4815 still transmits through the BK4829.
 *
 * Device-header free and gEeprom-free on purpose: `rf_dual_choose` is pure and
 * host-testable, and the caller resolves `AUTO` and reads the VFOs.
 */
#ifndef DRIVER_RF_DUAL_H
#define DRIVER_RF_DUAL_H

#include <stdbool.h>
#include <stdint.h>

/* Which VFO (0/1) each part is tuned to, or -1 for "not used". */
typedef struct {
    int8_t bk4829_vfo;
    int8_t bk4815_vfo;
} rf_dual_roles_t;

/* Resolve the roles from whether each VFO's transceiver is the BK4829
 * (`a_4829`/`b_4829`) and the selected receive VFO. */
rf_dual_roles_t rf_dual_choose(bool a_4829, bool b_4829, uint8_t rx_vfo);

/* Tune the BK4815 to `f4815` (10 Hz units) when the roles use it, else leave it
 * alone.  The BK4829's tune is the caller's (rx_set_frequency). */
void rf_dual_apply(const rf_dual_roles_t *roles, uint32_t f4815);

bool     rf_dual_bk4815_active(void);
uint16_t rf_dual_bk4815_rssi(void);

#endif /* DRIVER_RF_DUAL_H */
