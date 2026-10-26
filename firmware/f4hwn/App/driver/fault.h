/* Fault handlers that print a diagnosis on the UART console.
 *
 * During bring-up a crash and a dead panel look identical -- both are "nothing
 * on the screen".  These handlers turn the first case into readable output:
 * the stacked registers plus the Cortex-M fault status words, then the CPU is
 * parked in a loop.
 *
 * The startup file provides weak aliases for every vector, so defining the
 * handler here is enough; no registration is needed.
 */
#ifndef DRIVER_FAULT_H
#define DRIVER_FAULT_H

#include <stdint.h>

/* Called from the naked handlers below with the exception stack frame. */
void fault_report(uint32_t *frame, uint32_t exc_return);

#endif /* DRIVER_FAULT_H */
