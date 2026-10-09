/* Host-only helpers for the preview tools (see host_hw.c). */
#ifndef HOST_HW_H
#define HOST_HW_H

#include "driver/keypad.h"

/* The key the host's keypad reader reports; KEY_INVALID = nothing pressed. */
void host_set_key(KEY_Code_t key);

/* The PTT2 line (PB9) the boot-mode check reads: true = held. */
void host_set_ptt2(bool pressed);

/* How many sector erases the stand-in flash has seen.  A regression check for
 * the one thing that must not happen on a timer: a save that rewrites the blob
 * on every 10 ms slice. */
unsigned host_flash_erase_count(void);

/* The host's model of the amplifier enable (PC13): the beeper's DAC tone only
 * reaches the speaker through it, so a beep must drive it on.  `host_beeper_*`
 * count beeper_play() calls and how many of them saw the path on (see
 * tools/host/host_beeper.c). */
bool host_audio_path_is_on(void);
unsigned host_beeper_play_count(void);
unsigned host_beeper_plays_path_on(void);
void host_beeper_reset_counts(void);

/* The last value passed to the host's `BK4819_SetAF` (the K1 `BK4819_AF_Type_t`
 * value, or -1 before any call).  The FM feature must mute the RF chip's AF
 * (`BK4819_AF_MUTE` = 0) while it owns the audio; see docs/ra89r_bk1080.md. */
int host_bk4819_last_af(void);

/* The host's scan-source override state (driver/rx.h's rx_scan_source_t as an
 * int): the dual-scan lifecycle check confirms it returns to DEFAULT on stop. */
int host_rx_scan_source(void);

#endif /* HOST_HW_H */
