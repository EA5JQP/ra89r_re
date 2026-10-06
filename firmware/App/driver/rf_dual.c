/* Dual-transceiver coordinator -- see rf_dual.h. */
#include "driver/rf_dual.h"

#include "driver/bk4815.h"

static bool s_secondary_active;

rf_dual_roles_t rf_dual_choose(bool a_4829, bool b_4829, uint8_t rx_vfo)
{
    rf_dual_roles_t r;

    r.primary   = -1;
    r.secondary = -1;

    if (a_4829 && !b_4829) {
        r.primary   = 0;
        r.secondary = 1;
    } else if (!a_4829 && b_4829) {
        r.primary   = 1;
        r.secondary = 0;
    } else if (a_4829 && b_4829) {
        /* One BK4829 cannot hold two frequencies; the receiver keeps it. */
        r.primary = (rx_vfo > 1u) ? 0 : (int8_t)rx_vfo;
    }
    /* Neither is the BK4829: no transmit-capable part, so nothing is active. */

    return r;
}

void rf_dual_apply(const rf_dual_roles_t *roles, uint32_t sec_rx_freq_10hz)
{
    if (roles != 0 && roles->secondary >= 0 && sec_rx_freq_10hz != 0u) {
        /* Receive mode: register 0x70 = 0xA000. */
        bk4815_set_frequency(sec_rx_freq_10hz, false);
        s_secondary_active = true;
    } else {
        s_secondary_active = false;
    }
}

bool rf_dual_secondary_active(void)
{
    return s_secondary_active;
}

uint16_t rf_dual_secondary_rssi(void)
{
    return s_secondary_active ? bk4815_read_rssi() : 0u;
}
