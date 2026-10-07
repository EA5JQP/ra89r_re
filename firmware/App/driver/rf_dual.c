/* Dual-transceiver coordinator -- see rf_dual.h. */
#include "driver/rf_dual.h"

#include "driver/bk4815.h"

static bool s_bk4815_active;

rf_dual_roles_t rf_dual_choose(bool a_4829, bool b_4829, uint8_t rx_vfo)
{
    rf_dual_roles_t r;
    const uint8_t   sel   = (rx_vfo > 1u) ? 0u : rx_vfo;
    const uint8_t   other = (uint8_t)(sel ^ 1u);
    const bool      sel_4829   = (sel   == 0u) ? a_4829 : b_4829;
    const bool      other_4829 = (other == 0u) ? a_4829 : b_4829;

    r.bk4829_vfo = -1;
    r.bk4815_vfo = -1;

    /* The selected VFO's chip is always tuned to it. */
    if (sel_4829)
        r.bk4829_vfo = (int8_t)sel;
    else
        r.bk4815_vfo = (int8_t)sel;

    /* The other VFO only if it names the other chip (one part cannot hold two
     * frequencies). */
    if (other_4829 != sel_4829) {
        if (other_4829)
            r.bk4829_vfo = (int8_t)other;
        else
            r.bk4815_vfo = (int8_t)other;
    }

    return r;
}

void rf_dual_apply(const rf_dual_roles_t *roles, uint32_t f4815)
{
    if (roles != 0 && roles->bk4815_vfo >= 0 && f4815 != 0u) {
        /* Receive mode: register 0x70 = 0xA000. */
        bk4815_set_frequency(f4815, false);
        /* The band register 0x75 follows the tuned frequency (0x0A above
         * 280 MHz, else 0x11), the same split pa_select_band uses for the
         * BK4829 path.  It is otherwise left stale by a BK4815-only setup. */
        bk4815_write_reg(0x75u, (f4815 >= 28000000u) ? 0x0Au : 0x11u);
        s_bk4815_active = true;
    } else {
        s_bk4815_active = false;
    }
}

bool rf_dual_bk4815_active(void)
{
    return s_bk4815_active;
}

uint16_t rf_dual_bk4815_rssi(void)
{
    return s_bk4815_active ? bk4815_read_rssi() : 0u;
}
