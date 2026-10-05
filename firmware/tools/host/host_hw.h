/* Host-only helpers for the preview tools (see host_hw.c). */
#ifndef HOST_HW_H
#define HOST_HW_H

#include <stdint.h>

#include "driver/keypad.h"

/* The key the host's keypad reader reports; KEY_INVALID = nothing pressed. */
void host_set_key(KEY_Code_t key);

/* The PTT2 line (PB9) the boot-mode check reads: true = held. */
void host_set_ptt2(bool pressed);

/* How many sector erases the stand-in flash has seen.  A regression check for
 * the one thing that must not happen on a timer: a save that rewrites the blob
 * on every 10 ms slice. */
unsigned host_flash_erase_count(void);

/* Set one synthetic stock-codeplug byte by absolute SPI-flash offset. */
void host_set_codeplug_byte(uint32_t offset, uint8_t value);

/* The host's model of the amplifier enable (PC13): the beeper's DAC tone only
 * reaches the speaker through it, so a beep must drive it on.  `host_beeper_*`
 * count beeper_play() calls and how many of them saw the path on (see
 * tools/host/host_beeper.c). */
bool host_audio_path_is_on(void);
unsigned host_beeper_play_count(void);
unsigned host_beeper_plays_path_on(void);
void host_beeper_reset_counts(void);

#endif /* HOST_HW_H */
