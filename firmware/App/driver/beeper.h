/* The beeper -- a synthesised tone on the DAC, GPIOA pin 4 (`DAC_OUT1`).
 *
 * The stock's key beep is not a square wave on a spare pin: it is a tone the
 * MCU synthesises and puts out of the DAC, which is why it can beep without
 * disturbing the RF chip.  `docs/ra89r_beeper.md` has the stock's own path
 * (TIM4 plus a waveform generator); this driver keeps the same shape -- TIM4
 * steps a phase accumulator through a sine table, and the update ISR writes the
 * next sample to the DAC -- but with a plain sine at a 16 kHz sample rate
 * rather than the stock's note-length table.
 *
 * This header is deliberately free of any MCU include: the tone math is the
 * part a wrong shift turns into a wrong note, and it is host-tested in
 * `tools/test_beeper.c`.  The hardware half is `driver/beeper.c`.
 */
#ifndef DRIVER_BEEPER_H
#define DRIVER_BEEPER_H

#include <stdint.h>

/* A 64-entry sine, +/-2000 about the DAC's 12-bit midpoint (2048). */
#define BEEPER_SINE_LEN    64u
#define BEEPER_PHASE_FRAC  8u
#define BEEPER_PHASE_MASK  (((uint32_t)BEEPER_SINE_LEN << BEEPER_PHASE_FRAC) - 1u)

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

/* Per-sample phase increment, in table-index * 2^BEEPER_PHASE_FRAC units, so a
 * cycle takes `beeper_osc_step() * (BEEPER_SINE_LEN << FRAC)` samples and the
 * tone is `freq_hz` at `sample_hz`. */
static inline uint32_t beeper_osc_step(uint16_t freq_hz, uint32_t sample_hz)
{
    return (((uint32_t)freq_hz * BEEPER_SINE_LEN) << BEEPER_PHASE_FRAC) / sample_hz;
}

/* Advance the accumulator and return the sample at the new phase. */
static inline int16_t beeper_osc_next(uint32_t *phase, uint32_t step)
{
    *phase = (*phase + step) & BEEPER_PHASE_MASK;
    return beeper_sine[*phase >> BEEPER_PHASE_FRAC];
}

/* Bring up the DAC and TIM4; safe to call twice. */
void beeper_init(void);

/* Play `freq_hz` for `duration_ms`, blocking (the K1 asks for one beep at a
 * time).  A frequency of 0, or an uninitialised driver, is silent. */
void beeper_play(uint16_t freq_hz, uint16_t duration_ms);

void beeper_stop(void);

#endif /* DRIVER_BEEPER_H */
