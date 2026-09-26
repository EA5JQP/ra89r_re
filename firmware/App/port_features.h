/* Feature selection for the files imported from the UV-K1/K5V3 firmware.
 *
 * The K1 tree passes its features as CMake compile definitions (its
 * `enable_feature()` helper) and its sources are written against `#ifdef
 * ENABLE_*`.  Keeping that switch in one header, force-included by the build
 * (`-include App/port_features.h`), lets the imported files stay byte-for-byte
 * comparable with upstream instead of growing RA89R-specific #ifdefs.
 *
 * Only add a macro here when the port actually compiles the code that needs it:
 * a macro that pulls in an array nothing references is harmless, but one that
 * pulls in code with unresolved symbols is not.
 */
#ifndef APP_PORT_FEATURES_H
#define APP_PORT_FEATURES_H

/* The application the port targets: the F4HWN variant of the K1 firmware. */
#define ENABLE_FEAT_F4HWN

/* Fonts and icons the VFO screen and the menu use. */
#define ENABLE_SMALL_BOLD
#define ENABLE_VOX

#endif /* APP_PORT_FEATURES_H */
