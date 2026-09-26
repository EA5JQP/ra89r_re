/* Port shim (NOT part of the K1 tree).
 *
 * Some imported K1 sources include "py32f0xx.h", the vendor device header of
 * the PY32F071 the K1 runs on.  The RA89R is a PY32F403, whose SDK ships
 * "py32f4xx.h" (which selects py32f403xD.h from PY32F403xD).  The peripherals
 * the application layer touches are the same Cortex-M4F ones with the same
 * register layout for GPIO/RCC/SPI/USART -- where they differ, the port keeps
 * its own driver (App/driver/) instead of compiling the K1's, and only the
 * header name has to resolve.
 */
#ifndef PY32F0XX_H
#define PY32F0XX_H

#include "py32f4xx.h"

#endif /* PY32F0XX_H */
