/* Host test for the dual-transceiver coordinator (App/driver/rf_dual.c).
 *
 * `rf_dual_choose` is pure, and `rf_dual_apply`'s tuning is checked against a
 * recording bus stub.  No hardware.
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_rf_dual.c \
 *       App/driver/rf_dual.c App/driver/bk4815.c -o /tmp/test_rf_dual
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "frequencies.h"
#include "driver/bk4815.h"
#include "driver/rf_dual.h"
#include "driver/rf_bus.h"
#include "driver/rx.h"

/* ------------------------------------------------------------- bus stub --- */

static unsigned writes;

/* Recorded two-byte register writes, so the receive-entry sequence can be
 * checked by register rather than only by count. */
static uint8_t  rec_reg[64];
static uint16_t rec_val[64];
static unsigned rec_n;

void rf_bus_init(void) { }
void rf_bus_assert(uint32_t cs) { (void)cs; }
void rf_bus_release(uint32_t cs) { (void)cs; }
void rf_bus_delay(void) { }
void rf_bus_bit_out(int bit) { (void)bit; }
int  rf_bus_bit_in(void) { return 0; }
void rf_bus_write(uint32_t cs, uint8_t addr, const uint8_t *data, unsigned len)
{
    (void)cs;
    writes++;
    if (len == 2u && rec_n < (unsigned)(sizeof rec_reg)) {
        rec_reg[rec_n] = (uint8_t)(addr >> 1);
        rec_val[rec_n] = (uint16_t)((data[0] << 8) | data[1]);
        rec_n++;
    }
}
uint16_t rf_bus_read(uint32_t cs, uint8_t addr) { (void)cs; (void)addr; return 0; }

static void rec_reset(void) { rec_n = 0; }

static int rec_has(uint8_t reg, uint16_t value)
{
    unsigned i;

    for (i = 0; i < rec_n; i++)
        if (rec_reg[i] == reg && rec_val[i] == value)
            return 1;
    return 0;
}

/* ---------------------------------------------------------------- checks --- */

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) { fails++; printf("  FAIL %s\n", what); }
    else      printf("  ok   %s\n", what);
}

int main(void)
{
    rf_dual_roles_t r;
    unsigned        i;

    printf("rf dual\n");

    /* One VFO per chip: both tuned, whichever is selected. */
    r = rf_dual_choose(true, false, 0u);
    check(r.bk4829_vfo == 0 && r.bk4815_vfo == 1,
          "A=4829 B=4815, select A -> BK4829 on A, BK4815 on B");
    r = rf_dual_choose(true, false, 1u);
    check(r.bk4829_vfo == 0 && r.bk4815_vfo == 1,
          "A=4829 B=4815, select B -> same two tunings");

    /* Both VFOs on the BK4815: the selected one is still tuned. */
    r = rf_dual_choose(false, false, 0u);
    check(r.bk4829_vfo == -1 && r.bk4815_vfo == 0,
          "both 4815, select A -> BK4815 tuned to A");
    r = rf_dual_choose(false, false, 1u);
    check(r.bk4829_vfo == -1 && r.bk4815_vfo == 1,
          "both 4815, select B -> BK4815 tuned to B");

    /* Both VFOs on the BK4829: only the selected one. */
    r = rf_dual_choose(true, true, 0u);
    check(r.bk4829_vfo == 0 && r.bk4815_vfo == -1,
          "both 4829, select A -> BK4829 tuned to A only");

    /* Apply: a BK4815 role tunes it; none leaves it alone. */
    r = rf_dual_choose(false, false, 1u);
    writes = 0;
    rf_dual_apply(&r, 43350000u);
    check(writes == 4 && rf_dual_bk4815_active(),
          "BK4815 role tunes it (tune + band register)");

    r = rf_dual_choose(true, true, 0u);
    writes = 0;
    rf_dual_apply(&r, 0u);
    check(writes == 0 && !rf_dual_bk4815_active(),
          "no BK4815 role leaves it untouched");

    /* The stock's per-VFO receive entry (FUN_08016CEC), checked by register. */
    rec_reset();
    bk4815_receive_entry(true);
    check(rec_has(0x16, 0xb200) && rec_has(0x2b, 0x403f) &&
          rec_has(0x6d, 0x6618) && rec_has(0x73, 0x81d0),
          "BK4815 receive entry: the demod/AF block");
    check(rec_has(0x01, 0x8044), "wide channel -> 0x01 = 0x8044 (25 kHz)");
    check(rec_n >= 2u && rec_reg[rec_n - 2u] == 0x70 && rec_val[rec_n - 2u] == 0x0000 &&
          rec_reg[rec_n - 1u] == 0x70 && rec_val[rec_n - 1u] == 0xa000,
          "BK4815 receive entry ends with the 0x70 restart pair");

    /* The stock's PLL calibration (FUN_0801BF76/FUN_0801BF54) opens the entry:
     * 0x0C = 0x0A03, 0x0B = 0x0800/0x8800/0x9800, then the 0x0C word that was
     * in force (the audio-lane word here) goes back. */
    bk4815_write_reg(0x0C, BK4815_PWR_RX_AUDIO);
    rec_reset();
    bk4815_receive_entry(true);
    check(rec_n >= 5u &&
          rec_reg[0] == 0x0C && rec_val[0] == 0x0a03 &&
          rec_reg[1] == 0x0B && rec_val[1] == 0x0800 &&
          rec_reg[2] == 0x0B && rec_val[2] == 0x8800 &&
          rec_reg[3] == 0x0B && rec_val[3] == 0x9800 &&
          rec_reg[4] == 0x0C && rec_val[4] == BK4815_PWR_RX_AUDIO,
          "BK4815 receive entry starts with the stock's PLL calibration, 0x0C restored");
    bk4815_write_reg(0x0C, BK4815_PWR_RX_QUIET);
    rec_reset();
    bk4815_pll_calibrate();
    check(rec_n == 4u && rec_reg[3] == 0x0B && rec_val[3] == 0x9800,
          "PLL calibration with the quiet word in force leaves 0x0C = 0x0A03");

    rec_reset();
    bk4815_receive_entry(false);
    check(rec_has(0x01, 0x0044), "narrow channel -> 0x01 = 0x0044 (12.5 kHz)");

    /* The airband is a band, not a range to invent: 108.000-137.000 MHz, with
     * 137.000 being the start of the next band (the K1's BAND3_137MHz). */
    check(FREQUENCY_IsAirband(10800000u) && FREQUENCY_IsAirband(11800000u) &&
          FREQUENCY_IsAirband(13699750u),
          "108.0/118.0/136.9975 MHz are the airband");
    check(!FREQUENCY_IsAirband(10799990u) && !FREQUENCY_IsAirband(13700000u) &&
          !FREQUENCY_IsAirband(14550000u),
          "107.9999/137.0/145.5 MHz are not");

    /* The live gate: a lane's marks are its learned floor plus a level-derived
     * margin, so a fixed pair cannot hide weak signals (radio 2026-10-21: the
     * 4829 floors were 96-103 VHF / 76-79 UHF, the stock pair is 207/180). */
    check(rx_squelch_open_mark_from_floor(false, false, 4u, 80u) ==
              (uint16_t)(80u + rx_squelch_margin(false, 4u)),
          "BK4829 floor 80 at SQL 4 opens 80 + margin");
    check(rx_squelch_open_mark_from_floor(false, false, 8u, 80u) >
              rx_squelch_open_mark_from_floor(false, false, 4u, 80u),
          "a higher SQL level opens later above the same floor");
    check(rx_squelch_margin(false, 0u) < rx_squelch_margin(false, 4u) &&
              rx_squelch_margin(false, 4u) < rx_squelch_margin(false, 9u),
          "the BK4829 margin is monotone in the level");
    check(rx_squelch_margin(true, 4u) < rx_squelch_margin(false, 4u),
          "the BK4815's 7-bit scale keeps its margins smaller");
    check(rx_squelch_open_mark_from_floor(false, false, 4u, 0u) == 0xCFu,
          "an unknown floor falls back to the stock pair");

    /* The floor tracker itself (its bugs were found on the radio). */
    {
        rx_floor_t f;

        rx_floor_reset(&f);
        /* The floor starts from the measured prior, never from the samples:
         * with a carrier present at boot those samples are the carrier. */
        rx_floor_reset(&f);
        check(rx_floor_step(&f, false, RX_FLOOR_PRIOR_4829_FM_VHF, 194u, true) ==
                  RX_FLOOR_PRIOR_4829_FM_VHF,
              "the floor starts at the prior even with a carrier reading");

        /* The floor climbs to a higher noise floor, slowly (+1 every
         * RX_FLOOR_RISE_DIV closed polls), so a brief signal cannot move it. */
        {
            const uint16_t start = f.floor;

            for (i = 0; i < RX_FLOOR_RISE_DIV * 10u; i++)
                (void)rx_floor_step(&f, false, 0xFFFFu, 120u, true);
            check(f.floor == (uint16_t)(start + 10u),
                  "a higher noise floor is followed at 1 count per RISE_DIV polls");

            rx_floor_reset(&f);
            (void)rx_floor_step(&f, false, 63u, 63u, true);
            for (i = 0; i < RX_FLOOR_RISE_DIV - 1u; i++)
                (void)rx_floor_step(&f, false, 63u, 63u, true);
            check(f.floor == 63u,
                  "a brief spike does not move the floor");
        }

        /* A quieter channel is followed, but only after consecutive samples. */
        rx_floor_reset(&f);
        (void)rx_floor_step(&f, false, 96u, 96u, true);      /* prior 96 */
        (void)rx_floor_step(&f, false, 96u, 90u, true);
        check(f.floor == 90u, "a small fall is immediate");
        rx_floor_reset(&f);
        (void)rx_floor_step(&f, false, 96u, 96u, true);
        (void)rx_floor_step(&f, false, 96u, 40u, true);
        check(f.floor == 96u, "one large low sample is a transient");
        for (i = 0; i < RX_FLOOR_DROP_SAMPLES; i++)
            (void)rx_floor_step(&f, false, 96u, 40u, true);
        check(f.floor == 40u, "consecutive low samples move the floor");

        /* No learning while the gate is open. */
        rx_floor_step(&f, false, 96u, 40u, true);
        (void)rx_floor_step(&f, false, 96u, 200u, false);
        check(f.floor == 40u, "the floor is frozen while open");

        /* The per-lane bounds hold. */
        check(rx_floor_clamp(1u, true) == RX_FLOOR_MIN_4815 &&
              rx_floor_clamp(600u, false) == RX_FLOOR_MAX_4829,
              "the per-lane floor bounds hold");
    }

    /* The level-only anchors (used when no floor is known): unchanged. */
    check(rx_squelch_open_mark(false, false, 8u) == 0xCFu &&
          rx_squelch_close_mark(false, false, 8u) == 0xB4u,
          "BK4829 level 8 = the measured pair 0xCF/0xB4");
    check(rx_squelch_open_mark(true, true, 8u) == 30u &&
          rx_squelch_close_mark(true, true, 8u) == 24u,
          "BK4815 UHF level 8 = the measured pair 30/24");
    check(rx_squelch_open_mark(true, false, 8u) == 20u &&
          rx_squelch_close_mark(true, false, 8u) == 14u,
          "BK4815 VHF level 8 = the measured pair 20/14");
    for (i = 2u; i <= 9u; i++) {
        const uint16_t prev = rx_squelch_open_mark(false, false, (uint8_t)(i - 1u));
        const uint16_t now  = rx_squelch_open_mark(false, false, (uint8_t)i);
        check(now > prev, "BK4829 opening mark rises with the squelch level");
        check(rx_squelch_close_mark(false, false, (uint8_t)i) < now,
              "BK4829 closing mark stays below the opening mark");
    }
    check(rx_squelch_open_mark(true, false, 1u) > 0u &&
          rx_squelch_open_mark(true, false, 1u) < rx_squelch_open_mark(true, false, 8u),
          "BK4815 VHF level 1 opens earlier than level 8");

    /* Tone squelch: a toned channel opens only once the K1 accepted the code. */
    check(!rx_tone_gate_open(false, false, true) &&
          !rx_tone_gate_open(false, true, true),
          "tone gate: no carrier, no audio");
    check(rx_tone_gate_open(true, false, false),
          "tone gate: no tone set, the carrier alone opens");
    check(!rx_tone_gate_open(true, true, false),
          "tone gate: tone set, carrier without the code stays muted");
    check(rx_tone_gate_open(true, true, true),
          "tone gate: tone set and accepted by the K1, opens");

    /* The BK4815's receive CTCSS: the stock's word and write sequence. */
    check(bk4815_ctcss_rx_word(885u) == 2513u,
          "BK4815 CTCSS word: 88.5 Hz = 2513 (datasheet 2512.7, rounded)");
    check(bk4815_ctcss_rx_word(2541u) == 7214u,
          "BK4815 CTCSS word: 254.1 Hz = 7214 (fits 13 bits)");
    check(bk4815_ctcss_rx_word(670u) == 1902u,
          "BK4815 CTCSS word: 67.0 Hz = 1902");
    rec_reset();
    bk4815_set_rx_ctcss(885u);
    check(rec_n == 4u &&
          rec_reg[0] == 0x5b && rec_val[0] == 2513u &&
          rec_reg[1] == 0x5c && rec_val[1] == 0x4001u &&
          rec_reg[2] == 0x5d && rec_val[2] == 0x0060u &&
          rec_reg[3] == 0x42 && rec_val[3] == 0xd103u,
          "BK4815 tone set: FUN_08006D10's 0x5B, 0x5C=0x4001, 0x5D=0x60, 0x42=0xD103");
    rec_reset();
    bk4815_set_rx_ctcss(0u);
    check(rec_n == 4u &&
          rec_reg[0] == 0x5b && rec_val[0] == 0x061au &&
          rec_reg[1] == 0x5c && rec_val[1] == 0x4000u,
          "BK4815 no tone: the stock's 55 Hz symbol, 0x5C=0x4000");

    /* The tone tracker over the polled flags. */
    check(rx_4815_tone_step(false, true, false, false),
          "BK4815 tone: found sets present");
    check(!rx_4815_tone_step(true, false, true, true),
          "BK4815 tone: lost clears present");
    check(rx_4815_tone_step(true, false, false, true) &&
          !rx_4815_tone_step(false, false, false, false),
          "BK4815 tone: no flag holds the state");
    check(rx_4815_tone_step(false, true, true, false) &&
          !rx_4815_tone_step(true, true, true, true),
          "BK4815 tone: both flags, 0x5C bit 13 decides");

    /* The BK4815's receive DCS: the stock's inverted word, layout and order. */
    {
        uint16_t hi = 0u, lo = 0u;

        check(bk4815_dcs_rx_word(0x763813u) == 0x09c7ecu,
              "BK4815 DCS 023: the receive word is the K1 word inverted (0x09C7EC)");
        bk4815_dcs_code_regs(0x09c7ecu, &hi, &lo);
        check(hi == 0xf09cu && lo == 0xf7ecu,
              "BK4815 DCS 023: 0x5F/0x60 = 0xF09C/0xF7EC (FUN_0801C576's layout)");
        rec_reset();
        bk4815_set_rx_dcs(0x09c7ecu);
        check(rec_n == 6u &&
              rec_reg[0] == 0x59 && rec_val[0] == 0xf7a1u &&
              rec_reg[1] == 0x62 && rec_val[1] == 0x0003u &&
              rec_reg[2] == 0x61 && rec_val[2] == 0x0100u &&
              rec_reg[3] == 0x5c && rec_val[3] == 0x4000u &&
              rec_reg[4] == 0x5d && rec_val[4] == 0x0060u &&
              rec_reg[5] == 0x42 && rec_val[5] == 0xd083u,
              "BK4815 DCS set: 0x59, 0x62=3, 0x61=0x100, [code burst], 0x5C, 0x5D, 0x42=0xD083");
        check(rx_4815_sub_is_dcs(rx_4815_sub_dcs(0x09c7ecu)) &&
              rx_4815_sub_dcs_word(rx_4815_sub_dcs(0x09c7ecu)) == 0x09c7ecu &&
              !rx_4815_sub_is_dcs(rx_4815_sub_ctcss(2541u)) &&
              !rx_4815_sub_is_dcs(RX_4815_SUB_UNKNOWN) &&
              !rx_4815_sub_is_dcs(RX_4815_SUB_NONE),
              "BK4815 detector key: DCS / CTCSS / none / unknown stay apart");
    }

    /* The BK4829's measured-tone check (REG_68). */
    {
        static const uint16_t tones[] = { 670u, 693u, 719u, 744u, 2503u, 2541u };
        uint16_t t = 0u;

        check(!rx_ctcss_reg68_tone(0x8473u, &t), "REG_68 bit 15 set: no tone measured");
        check(rx_ctcss_reg68_tone(0x0573u, &t) && t == 675u,
              "REG_68 0x0573 = 67.5 Hz (the radio's reading of a 67.0 transmission)");
        check(!rx_ctcss_nearest_is(675u, tones, 6u, 1u),
              "67.5 Hz measured does not match a 69.3 Hz channel");
        check(rx_ctcss_nearest_is(675u, tones, 6u, 0u),
              "67.5 Hz measured matches a 67.0 Hz channel");
        check(rx_ctcss_nearest_is(698u, tones, 6u, 1u) &&
              rx_ctcss_nearest_is(2530u, tones, 6u, 5u),
              "a measurement within half a gap matches its own tone");
        check(!rx_ctcss_nearest_is(670u, tones, 0u, 0u), "empty table never matches");
    }

    printf("\n%d failed\n", fails);
    return fails ? 1 : 0;
}
