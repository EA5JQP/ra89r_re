/* Host stand-in for the beeper driver (see NOTICE).
 *
 * The previews link App/audio.c, which now calls beeper_play() on every beep;
 * the DAC and TIM4 are not modelled by the host device-header double, and a
 * preview is silent anyway, so this answers in the driver's place.  The tone
 * math that is worth checking on a PC lives in driver/beeper.h and is tested by
 * tools/test_beeper.c directly.
 */
#include "driver/beeper.h"

void beeper_init(void) { }

void beeper_play(uint16_t freq_hz, uint16_t duration_ms)
{
    (void)freq_hz;
    (void)duration_ms;
}

void beeper_stop(void) { }
