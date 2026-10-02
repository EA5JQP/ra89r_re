/* Host stand-in for the beeper driver (see NOTICE).
 *
 * The previews link App/audio.c, which calls beeper_play() on every beep; the
 * DAC and TIM4 are not modelled by the host device-header double, and a preview
 * is silent anyway, so this answers in the driver's place.  The tone math that
 * is worth checking on a PC lives in driver/beeper.h and is tested by
 * tools/test_beeper.c directly.
 *
 * It does count calls and whether the amplifier enable was on when they
 * happened: the DAC tone only reaches the speaker through that amp, so a beep
 * that does not turn it on is silent on the radio -- a fault this catches
 * (tools/preview_k1.c).
 */
#include "driver/beeper.h"
#include "host_hw.h"

static unsigned s_plays;
static unsigned s_plays_path_on;

void beeper_init(void) { }

void beeper_play(uint16_t freq_hz, uint16_t duration_ms)
{
    (void)freq_hz;
    (void)duration_ms;

    s_plays++;
    if (host_audio_path_is_on())
        s_plays_path_on++;
}

void beeper_stop(void) { }

unsigned host_beeper_play_count(void) { return s_plays; }
unsigned host_beeper_plays_path_on(void) { return s_plays_path_on; }

void host_beeper_reset_counts(void)
{
    s_plays = 0;
    s_plays_path_on = 0;
}
