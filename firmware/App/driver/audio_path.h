/* The audio-path / amplifier enable line.
 *
 * The K1 firmware drives a GPIO of its own in this role
 * (`GPIO_PIN_AUDIO_PATH`, PA8 there) through `AUDIO_AudioPathOn()` /
 * `AUDIO_AudioPathOff()`, and the port replaces those two macros with the
 * callback this module provides (see `BK4819_SetAudioPathCallback`).
 *
 * On this board the line is `PC13`.  What is established: the stock *raises* it
 * from its T/R path (`FUN_080177A8`, gated by two codeplug bits, both clear on
 * this radio, so it takes the unconditional high branch) while its RF bring-up
 * clears it (`FUN_08009C9C`); receive audio is audible with it high; and
 * dropping it during a transmission did not change what a second receiver heard.
 *
 * What is *not* established is what it switches -- an amplifier enable, an
 * analogue path switch or something else entirely -- so this driver asserts it
 * the way the working bench did and nothing more is read into it.  See
 * `ra89r_rffeatures.md` and `ra89r_led.md`.
 */
#ifndef DRIVER_AUDIO_PATH_H
#define DRIVER_AUDIO_PATH_H

#include <stdbool.h>

/* Configure the line as an output and assert it, the state receive and transmit
 * were validated in. */
void audio_path_init(void);

/* Drive it: nonzero = the state that carries audio.  This is the callback the
 * K1 `BK4819_*` layer calls around its transmit and receive transitions. */
void audio_path_drive(int on);

bool audio_path_is_on(void);

#endif /* DRIVER_AUDIO_PATH_H */
