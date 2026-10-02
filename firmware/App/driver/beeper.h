/* The beeper -- a synthesised tone on the DAC, GPIOA pin 4 (`DAC_OUT1`).
 *
 * The stock's key beep is not a square wave on a spare pin: it is a tone the
 * MCU synthesises and puts out of the DAC, which is why it can beep without
 * disturbing the RF chip.  `docs/ra89r_beeper.md` has the stock's own path
 * (TIM4 plus a waveform generator); this driver keeps the same shape -- TIM4
 * clocks the samples and a sine table is the waveform -- but the samples reach
 * the DAC through DMA (no ISR) and the tone is set by TIM4's reload rather than
 * the stock's note-length table.
 *
 * This header is deliberately free of any MCU include: the tone math is the
 * part a wrong shift turns into a wrong note, and it is host-tested in
 * `tools/test_beeper.c`.  The hardware half is `driver/beeper.c`.
 */
#ifndef DRIVER_BEEPER_H
#define DRIVER_BEEPER_H

#include <stdint.h>

/* A 64-entry sine, +/-2000 about the DAC's 12-bit midpoint (2048).  This is the
 * DMA source; the tone comes from the timer reload (beeper_arr()). */
#define BEEPER_SINE_LEN    64u

static const int16_t beeper_sine[BEEPER_SINE_LEN] = {
        0,   196,   390,   581,   765,   943,  1111,  1269,
     1414,  1546,  1663,  1764,  1848,  1914,  1962,  1990,
     2000,  1990,  1962,  1914,  1848,  1764,  1663,  1546,
     1414,  1269,  1111,   943,   765,   581,   390,   196,
        0,  -196,  -390,  -581,  -765,  -943, -1111, -1269,
    -1414, -1546, -1663, -1764, -1848, -1914, -1962, -1990,
    -2000, -1990, -1962, -1914, -1848, -1764, -1663, -1546,
    -1414, -1269, -1111,  -943,  -765,  -581,  -390,  -196,
};

/* The TIM4 reload that makes the DMA play the table at `freq_hz`: the timer's
 * update rate is `timer_hz / (ARR + 1)` and one cycle is BEEPER_SINE_LEN
 * samples, so
 *     ARR = timer_hz / (freq_hz * BEEPER_SINE_LEN) - 1. */
static inline uint32_t beeper_arr(uint16_t freq_hz, uint32_t timer_hz)
{
    return timer_hz / ((uint32_t)freq_hz * BEEPER_SINE_LEN) - 1u;
}

/* Bring up the DAC and TIM4; safe to call twice. */
void beeper_init(void);

/* Play `freq_hz` for `duration_ms`, blocking (the K1 asks for one beep at a
 * time).  A frequency of 0, or an uninitialised driver, is silent. */
void beeper_play(uint16_t freq_hz, uint16_t duration_ms);

void beeper_stop(void);

#endif /* DRIVER_BEEPER_H */
