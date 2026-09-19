/* Pin watcher -- a diagnostic for finding out what the keypad is wired to.
 *
 * The stock application reads only seven single-bit GPIO inputs and never
 * touches a port register, so the radio's 20 buttons are not a matrix that this
 * firmware can see statically (see ra89r_findings.md, "Keypad").  This is the
 * empirical counterpart: it parks every pin the application does not need as an
 * input with a pull-up and reports, over the console, which of them change when
 * a button is pressed -- and it can also perform an open-drain row sweep, which
 * is what a key matrix responds to.
 *
 * Everything here is deliberately destructive to the pins it touches: the
 * panel's pins (PA8-PA11, PB15) and the console's (PB6/PB7) are left alone, but
 * the RF/PMIC/flash buses are released while the watcher is armed, so the radio
 * stops doing anything useful until `w` is pressed again or it is reset.
 */
#ifndef DRIVER_PINWATCH_H
#define DRIVER_PINWATCH_H

#include <stdbool.h>

/* Park the candidate pins (input, pull-up) and print a first snapshot.
 * Idempotent: arming twice just re-parks and re-prints. */
void pinwatch_arm(void);

/* Put the pins back to a sane idle state and stop reporting. */
void pinwatch_stop(void);

bool pinwatch_is_active(void);

/* Call from the main loop: prints a line whenever any watched line changes. */
void pinwatch_poll(void);

/* One open-drain sweep: every candidate is pulled low in turn (open drain, so it
 * can never fight a driver that holds the line high) while the others are read
 * with a pull-up.  A pressed button shows up as "driver A2 also pulls B5 low".
 * Repeats `passes` times so a button can be held while it runs. */
void pinwatch_sweep(int passes);

#endif /* DRIVER_PINWATCH_H */
