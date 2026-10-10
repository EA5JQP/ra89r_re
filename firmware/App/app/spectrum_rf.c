/* The spectrum's chip policy.  See App/app/spectrum_rf.h and docs/ra89r_spectrum.md.
 *
 * Device-header free: the decisions here are pure so they can be checked on a PC
 * (tools/test_spectrum.c).
 */
#include "app/spectrum_rf.h"

spectrum_chip_t spectrum_rf_step_chip(spectrum_chip_t setting, uint16_t index)
{
    if (setting == SPECTRUM_CHIP_BOTH)
        return (index & 1u) ? SPECTRUM_CHIP_4815 : SPECTRUM_CHIP_4829;

    return (setting == SPECTRUM_CHIP_4815) ? SPECTRUM_CHIP_4815 : SPECTRUM_CHIP_4829;
}

uint16_t spectrum_rf_normalize_rssi(spectrum_chip_t chip, uint16_t raw)
{
    if (chip != SPECTRUM_CHIP_4815)
        return raw;

    /* 127 * 4 ~= 508, inside the BK4829's 9-bit range. */
    return (raw > (uint16_t)(0xFFFFu / 4u)) ? 0xFFFFu : (uint16_t)(raw * 4u);
}

uint32_t spectrum_rf_rssi_settle_us(spectrum_chip_t chip)
{
    return (chip == SPECTRUM_CHIP_4815) ? 350u : 0u;
}

uint8_t spectrum_rf_encode_chip(spectrum_chip_t chip)
{
    if (chip == SPECTRUM_CHIP_4815 || chip == SPECTRUM_CHIP_BOTH)
        return (uint8_t)chip;
    return (uint8_t)SPECTRUM_CHIP_4829;
}

spectrum_chip_t spectrum_rf_decode_chip(uint8_t byte)
{
    if (byte == (uint8_t)SPECTRUM_CHIP_4815)
        return SPECTRUM_CHIP_4815;
    if (byte == (uint8_t)SPECTRUM_CHIP_BOTH)
        return SPECTRUM_CHIP_BOTH;
    return SPECTRUM_CHIP_4829;
}
