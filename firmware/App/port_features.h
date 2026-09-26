/* Feature selection for the files imported from the UV-K1/K5V3 firmware.
 *
 * The K1 tree passes its features as CMake compile definitions (its
 * `enable_feature()` helper, i.e. `-DENABLE_X`, which the compiler reads as
 * `ENABLE_X 1`) and its sources use both `#ifdef ENABLE_X` and `#if ENABLE_X`.
 * Keeping that switch in one header, force-included by the build
 * (`-include App/port_features.h`), lets the imported files stay byte-for-byte
 * comparable with upstream instead of growing RA89R-specific #ifdefs.
 *
 * Every macro is defined with a value: `#define ENABLE_X` (empty) makes
 * `#if ENABLE_X` a preprocessor error, which is how the K1 build would fail if
 * its definitions were not `-D` ones.
 *
 * Only add a macro here when the port actually compiles the code that needs it:
 * a macro that pulls in an array nothing references is harmless, but one that
 * pulls in code with unresolved symbols is not.
 */
#ifndef APP_PORT_FEATURES_H
#define APP_PORT_FEATURES_H

/* The application the port targets: the F4HWN variant of the K1 firmware. */
#define ENABLE_FEAT_F4HWN 1

/* Fonts and icons the VFO screen and the menu use. */
#define ENABLE_SMALL_BOLD 1
#define ENABLE_VOX 1

/* The K1's own `default` CMake preset, for the entries that only change how the
 * screens are drawn.  What that preset also turns on but the RA89R lacks (FM
 * broadcast, air copy, NOAA, USB, voice prompts, spectrum, password) stays off,
 * and so do the hardware features that need a module the port has not brought
 * in yet (UART console, TX1750, flashlight, DTMF calling). */
#define ENABLE_BIG_FREQ 1
#define ENABLE_CUSTOM_MENU_LAYOUT 1
#define ENABLE_KEEP_MEM_NAME 1
#define ENABLE_WIDE_RX 1

/* Version strings the K1's menus and boot screen print.  The RA89R port is its
 * own build, so these say so rather than impersonating the K1 release. */
#define AUTHOR_STRING "RA89R port"
#define AUTHOR_STRING_1 "RA89R port"
#define AUTHOR_STRING_2 "EA5JQP"
#define VERSION_STRING_1 "ra89r_fw"
#define VERSION_STRING_2 "0.3"
#define DISPLAY_VERSION_STRING_2 "0.3"
#define EDITION_STRING "port"
#define ALERT_TOT 10
#define BUILD_COMMIT "port"

#endif /* APP_PORT_FEATURES_H */
