/* Host-only helpers for the preview tools (see host_hw.c). */
#ifndef HOST_HW_H
#define HOST_HW_H

#include "driver/keypad.h"

/* The key the host's keypad reader reports; KEY_INVALID = nothing pressed. */
void host_set_key(KEY_Code_t key);

/* How many sector erases the stand-in flash has seen.  A regression check for
 * the one thing that must not happen on a timer: a save that rewrites the blob
 * on every 10 ms slice. */
unsigned host_flash_erase_count(void);

#endif /* HOST_HW_H */
