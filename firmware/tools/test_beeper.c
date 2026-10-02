/* Host test for the beeper's tone math, without a radio.
 *
 * The DAC/TIM4 layer can only be proven on the radio (does it make a sound,
 * and on which pin).  What *can* be pinned here is the oscillator the ISR
 * steps: the phase increment that turns a frequency in Hz into a sine-table
 * step, and the period it produces.  That is the part where a wrong shift or an
 * off-by-one becomes a wrong note, and it costs a second to check.
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_beeper.c -o /tmp/test_beeper
 *   /tmp/test_beeper
 *
 * `driver/beeper.h` is device-header free on purpose, so this links it directly
 * with nothing else.  The hardware half (`driver/beeper.c`) is not built here.
 */
#include <stdint.h>
#include <stdio.h>

#include "driver/beeper.h"

static unsigned checks, failed;

static void check(int ok, const char *what)
{
    checks++;
    if (ok)
        printf("ok    %s\n", what);
    else {
        failed++;
        printf("FAIL  %s\n", what);
    }
}

/* Steps until the phase comes back to 0: one full cycle, counted in samples. */
static unsigned cycle_samples(uint16_t freq_hz, uint32_t sample_hz)
{
    uint32_t phase = 0;
    const uint32_t step = beeper_osc_step(freq_hz, sample_hz);
    unsigned n;

    for (n = 1; n <= 100000u; n++) {
        (void)beeper_osc_next(&phase, step);
        if (phase == 0u)
            return n;
    }
    return 0u;
}

int main(void)
{
    /* The step is the per-sample phase increment, in table-index*2^FRAC units:
     *   freq * table_len * 2^FRAC / sample_hz. */
    check(beeper_osc_step(1000u, 16000u) == 1024u, "1 kHz at 16 kHz -> step 1024");
    check(beeper_osc_step(2000u, 16000u) == 2048u, "2 kHz -> step 2048");
    check(beeper_osc_step(500u, 16000u) == 512u, "500 Hz -> step 512");

    /* The period follows from the step: 16 kHz / freq samples per cycle. */
    check(cycle_samples(1000u, 16000u) == 16u, "1 kHz -> 16-sample cycle");
    check(cycle_samples(2000u, 16000u) == 8u, "2 kHz -> 8-sample cycle");
    check(cycle_samples(500u, 16000u) == 32u, "500 Hz -> 32-sample cycle");

    /* A real sine, not a square or a sign error: one cycle rises above the
     * midpoint, falls below it, and is symmetric about it. */
    {
        uint16_t freq;
        int min = 32767, max = -32768;
        long sum = 0;
        unsigned n;
        uint32_t phase = 0;
        const uint32_t step = beeper_osc_step(1000u, 16000u);

        for (n = 0; n < 16u; n++) {
            const int v = beeper_osc_next(&phase, step);
            if (v < min)
                min = v;
            if (v > max)
                max = v;
            sum += v;
        }

        check(min < -1000 && max > 1000, "a cycle swings both ways");
        check(max - min >= 3800 && max - min <= 4096, "peak-to-peak is the table's span");
        check(sum > -4000 && sum < 4000, "a whole cycle is symmetric about the midpoint");
        (void)freq;
    }

    printf("\n%u checks, %u failed\n", checks, failed);
    return failed ? 1 : 0;
}
