/* Host test for the beeper's tone math, without a radio.
 *
 * The DMA/DAC layer can only be proven on the radio (does it sound, and on
 * which pin).  What *can* be pinned here is the timer reload that sets the
 * tone: DMA plays the 64-entry sine at TIM4's update rate, so the frequency is
 * timer_hz / (ARR + 1) / BEEPER_SINE_LEN and `beeper_arr()` is its inverse.  A
 * wrong divide or an off-by-one there is a wrong note, and it costs a second to
 * check.
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

/* The timer's clock: APB1, the prescaler is 1 (board_pins.h). */
#define TIMER_HZ 48000000u

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

/* The tone a reload actually produces. */
static uint32_t tone_hz(uint16_t freq_hz)
{
    return TIMER_HZ / (beeper_arr(freq_hz, TIMER_HZ) + 1u) / BEEPER_SINE_LEN;
}

int main(void)
{
    /* The reload is timer_hz / (freq * table_len) - 1. */
    check(beeper_arr(1000u, TIMER_HZ) == 749u,  "1 kHz -> ARR 749");
    check(beeper_arr(2000u, TIMER_HZ) == 374u,  "2 kHz -> ARR 374");
    check(beeper_arr(500u,  TIMER_HZ) == 1499u, "500 Hz -> ARR 1499");
    check(beeper_arr(400u,  TIMER_HZ) == 1874u, "400 Hz -> ARR 1874");
    check(beeper_arr(600u,  TIMER_HZ) == 1249u, "600 Hz -> ARR 1249");

    /* The K1's beep table, and the tone each reload produces. */
    check(tone_hz(1000u) == 1000u, "1 kHz round-trips exactly");
    check(tone_hz(500u)  == 500u,  "500 Hz round-trips exactly");
    check(tone_hz(400u)  == 400u,  "400 Hz round-trips exactly");
    check(tone_hz(880u) >= 878u && tone_hz(880u) <= 882u,
          "880 Hz lands within 0.3% (the reload is not an integer)");

    /* A real sine, not a square or a sign error: the DMA source swings both
     * ways about the DAC's midpoint and is symmetric. */
    {
        int min = 32767, max = -32768;
        long sum = 0;
        unsigned i;

        for (i = 0; i < BEEPER_SINE_LEN; i++) {
            const int v = beeper_sine[i];
            if (v < min) min = v;
            if (v > max) max = v;
            sum += v;
        }

        check(min < -1000 && max > 1000, "the table swings both ways");
        check(max - min >= 3800 && max - min <= 4096, "peak-to-peak is the table's span");
        check(sum > -4000 && sum < 4000, "a whole table is symmetric about the midpoint");
    }

    printf("\n%u checks, %u failed\n", checks, failed);
    return failed ? 1 : 0;
}
