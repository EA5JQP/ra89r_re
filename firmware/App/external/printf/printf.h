/* Port shim (NOT part of the K1 tree).
 *
 * The imported K1 sources include "external/printf/printf.h", the embedded
 * printf that project vendors.  The RA89R firmware links newlib-nano
 * (-specs=nano.specs), whose <stdio.h> provides the same entry points
 * (sprintf/snprintf/vsnprintf), so the import is a one-line mapping and the
 * imported files stay untouched.
 */
#ifndef EXTERNAL_PRINTF_PRINTF_H
#define EXTERNAL_PRINTF_PRINTF_H

#include <stdio.h>

#endif
