/* Feature selection for the files imported from the UV-K1/K5V3 firmware.
 *
 * The K1 tree passes its features as CMake compile definitions (its
 * `enable_feature()` helper, i.e. `-DENABLE_X`, which the compiler reads as
 * `ENABLE_X 1`) and its sources use both `#ifdef ENABLE_X` and `#if ENABLE_X`.
 * Keeping that switch in one header, force-included by the build
 * (`-include App/k1_features.h`), lets the imported files stay byte-for-byte
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
#ifndef APP_K1_FEATURES_H
#define APP_K1_FEATURES_H

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

/* The port draws the K1's double-channel VFO layout without the K1's dual-watch
 * engine.  The K1 draws both rows only when DUAL_WATCH (or CROSS_BAND) is not
 * OFF -- `ui/main.c`'s isMainOnly() -- so the port forces isMainOnly() false
 * and leaves `gEeprom.DUAL_WATCH` OFF.  The alternative, leaving DUAL_WATCH set
 * for the layout, also runs `app/app.c`'s DualwatchAlternate(): it toggles
 * `gEeprom.RX_VFO` and retunes every ~500 ms, ignoring the VFO the user
 * selected (this radio's A/B key is EXIT), and it diverts CheckForIncoming()
 * away from the port's polled `g_SquelchLost`.  Dual-watch is an RF behaviour
 * the port has no engine for. */
#define TWO_ROW_UI 1

/* Version strings the K1's menus and boot screen print.  The SysInf screen
 * (ui/menu.c, MENU_VOL) shows AUTHOR_STRING_2 + DISPLAY_VERSION_STRING_2 + the
 * edition, and the K1's identity there is kept -- only the version is the
 * port's, so the screen matches the K1's apart from the version. */
#define AUTHOR_STRING "F4HWN"
#define AUTHOR_STRING_1 "F4HWN"
#define AUTHOR_STRING_2 "F4HWN"
#define VERSION_STRING_1 "ra89r_fw"
#define VERSION_STRING_2 "0.3"
#define DISPLAY_VERSION_STRING_2 "0.3"
#define EDITION_STRING "Custom"
#define ALERT_TOT 10

/* The CTSS/squelch tone the K1's CMake sets (App/CMakeLists.txt: SQL_TONE=550). */
#define SQL_TONE 550
#define BUILD_COMMIT "port"

#endif /* APP_K1_FEATURES_H */
