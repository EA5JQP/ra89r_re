/* BK4815 receive service -- see bk4815_rx.h.
 *
 * Transcribed from the stock's `FUN_0801703c` (docs/ra89r_bk4815.md):
 *
 *   idx  = the VCO divider index from the frequency (FUN_0800ec70's thresholds)
 *   reg4 = (idx << 7) | 0xB041        -- bits 8:7 pick the divider 8/16/12/24
 *   word = (freq_10hz / 100000.0) * divider * 645277.53846154
 *   reg 0x70 = 0xA000 (RX)            -- 0xE000 is TX, not used here
 *   reg 0x71 = word >> 16, reg 0x72 = word & 0xffff
 *   regs 0x7E/0x7F = a per-band 32-bit calibration word
 *
 * The stock writes 0x70..0x72 as one 6-byte block and 0x7E/0x7F as a 4-byte
 * block, both under one select pulse (`FUN_08021F78`), which is what the
 * block-write calls below do.
 */
#include "driver/bk4815_rx.h"

#include "driver/bk4815.h"

/* The stock's `FUN_0800ec70` thresholds, in 10 Hz units: 187 / 270 / 383 MHz.
 * `<= 187 MHz -> 3`, `187..270 -> 1`, `270..383 -> 2`, `> 383 -> 0`. */
static uint8_t vco_index(uint32_t freq_10hz)
{
    if (freq_10hz <= 18700000u)
        return 3u;
    if (freq_10hz <= 27000000u)
        return 1u;
    if (freq_10hz <= 38300000u)
        return 2u;
    return 0u;
}

/* VCO divider per index (register 4 bits 8:7) and the per-band calibration
 * word written to 0x7E/0x7F, both from `FUN_0801703c`. */
static const uint16_t vco_div[4] = { 8u, 16u, 12u, 24u };
static const uint32_t calib[4]   = { 0xfff53568u, 0xffea6ad0u,
                                     0xffefd01cu, 0xffdfa037u };

void bk4815_rx_tune(uint32_t freq_10hz)
{
    const uint8_t  idx  = vco_index(freq_10hz);
    const uint32_t word = (uint32_t)(((double)freq_10hz / 100000.0)
                                     * (double)vco_div[idx]
                                     * 645277.53846154);
    uint8_t blk[6];
    uint8_t tail[4];

    bk4815_write_reg(BK4815_REG_VCO, (uint16_t)(((uint16_t)idx << 7) | 0xB041u));

    blk[0] = 0xA0u;                     /* reg 0x70 = 0xA000, receive */
    blk[1] = 0x00u;
    blk[2] = (uint8_t)(word >> 24);
    blk[3] = (uint8_t)(word >> 16);     /* reg 0x71 */
    blk[4] = (uint8_t)(word >> 8);
    blk[5] = (uint8_t)word;             /* reg 0x72 */
    bk4815_write_block(BK4815_REG_OPCTRL, blk, sizeof blk);

    tail[0] = (uint8_t)(calib[idx] >> 24);
    tail[1] = (uint8_t)(calib[idx] >> 16);
    tail[2] = (uint8_t)(calib[idx] >> 8);
    tail[3] = (uint8_t)calib[idx];
    bk4815_write_block(BK4815_REG_CAL, tail, sizeof tail);
}

uint16_t bk4815_rx_meter(void)
{
    return (uint16_t)(bk4815_read_reg(0x44u) & 0x7fu);
}
