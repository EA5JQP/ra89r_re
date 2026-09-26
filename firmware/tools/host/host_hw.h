/* Host-only helpers for the preview tools (see host_hw.c). */
#ifndef HOST_HW_H
#define HOST_HW_H

#include "driver/keypad.h"

/* The key the host's keypad reader reports; KEY_INVALID = nothing pressed. */
void host_set_key(KEY_Code_t key);

#endif /* HOST_HW_H */
