/* The audio-path / amplifier enable line.
 *
 * The K1 firmware drives a GPIO of its own in this role
 * (`GPIO_PIN_AUDIO_PATH`, PA8 there) through `AUDIO_AudioPathOn()` /
 * `AUDIO_AudioPathOff()`, and the port replaces those two macros with the
 * callback this module provides (see `BK4819_SetAudioPathCallback`).
 *
 * On this board the line is `PC13`. The stock drives it high while BT is off or
 * unlinked; while linked it follows codeplug settings byte 9 bit 5 (Speak
 * Switch) in `FUN_080177A8`. The user's required behavior is stricter: while a
 * BT link is active, no K1/RF/beeper caller may re-enable the local output. The
 * BT service therefore holds this path off for the whole link; disconnect
 * releases the hold. Host tests prove arbitration against later GPIO requests,
 * but the physical route still requires radio validation. A prior console test
 * found no PC13 effect on mic-source selection; do not treat it as a mic route.
 * See `docs/ra89r_bluetooth.md` and `docs/ra89r_rffeatures.md`.
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

/* BT-exclusive output policy: while enabled, suppress every local-speaker
 * request (including K1 RX callbacks and beeps) until the BT link is gone. */
void audio_path_set_bt_exclusive(bool enabled);

bool audio_path_is_on(void);

#endif /* DRIVER_AUDIO_PATH_H */
