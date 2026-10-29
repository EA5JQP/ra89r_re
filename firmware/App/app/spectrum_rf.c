/* The spectrum's chip policy.  See App/app/spectrum_rf.h and docs/ra89r_spectrum.md.
 *
 * Device-header free: the decisions here are pure so they can be checked on a PC
 * (tools/test_spectrum.c).
 */
#include "app/spectrum_rf.h"

spectrum_chip_t spectrum_rf_next_chip(spectrum_chip_t chip)
{
    return (chip == SPECTRUM_CHIP_4815) ? SPECTRUM_CHIP_4829 : SPECTRUM_CHIP_4815;
}

uint16_t spectrum_rf_normalize_rssi(spectrum_chip_t chip, uint16_t raw)
{
    if (chip != SPECTRUM_CHIP_4815)
        return raw;

    return (uint16_t)((raw & 0x7Fu) * 4u);
}

uint32_t spectrum_rf_rssi_settle_us(spectrum_chip_t chip)
{
    return (chip == SPECTRUM_CHIP_4815) ? 350u : 0u;
}

uint8_t spectrum_rf_encode_chip(spectrum_chip_t chip)
{
    return (chip == SPECTRUM_CHIP_4815) ? (uint8_t)SPECTRUM_CHIP_4815
                                        : (uint8_t)SPECTRUM_CHIP_4829;
}

spectrum_chip_t spectrum_rf_decode_chip(uint8_t byte)
{
    return (byte == (uint8_t)SPECTRUM_CHIP_4815) ? SPECTRUM_CHIP_4815
                                                 : SPECTRUM_CHIP_4829;
}
