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
 * screens are drawn.  What that preset also turns on but the RA89R lacks (air
 * copy, NOAA, USB, voice prompts, password) stays off, and so do the
 * hardware features that need a module the port has not brought in yet (UART
 * console, TX1750, flashlight, DTMF calling). */
#define ENABLE_BIG_FREQ 1
#define ENABLE_CUSTOM_MENU_LAYOUT 1
#define ENABLE_KEEP_MEM_NAME 1
#define ENABLE_WIDE_RX 1

/* Scanning: the K1's scan engine (App/app/chFrScanner.c, App/app/scanner.c) is
 * imported verbatim and carries the F4HWN fast-RSSI sweep and the scan-range
 * engine behind these flags, so the port turns them on rather than adding code
 * to the imported files.  The fast sweep is the "Scan faster" setting
 * (`gSetting_set_scn`): it reads the chip RSSI and only fully tunes a channel
 * whose RSSI clears the learned noise floor.  `ENABLE_FEAT_F4HWN_SCAN_RSSI`
 * draws the sweep sparkline; `ENABLE_SCAN_RANGES` is the range engine the fast
 * path prechecks.  The K1's `ENABLE_FEAT_F4HWN_RESUME_STATE` is left off: it
 * needs `SETTINGS_WriteCurrentState`, which is a scan-state EEPROM record the
 * port has no equivalent of, and it is not needed for the fast sweep.  None of
 * this is validated on the radio yet. */
#define ENABLE_SCAN_RANGES 1
#define ENABLE_FEAT_F4HWN_SCAN_FASTER 1
#define ENABLE_FEAT_F4HWN_SCAN_RSSI 1

/* Spectrum: the K1/F4HWN spectrum screen (App/app/spectrum.c, fagci) is imported
 * verbatim; `ENABLE_FEAT_F4HWN_SPECTRUM` is the version with the persisted
 * settings, the interlaced sweep and listen mode.  The port adds a per-spectrum
 * transceiver choice (BK4829 / BK4815 / Both) through App/app/spectrum_rf.*; see
 * docs/ra89r_spectrum.md for the deviation list.  Not validated on the radio. */
#define ENABLE_SPECTRUM 1
#define ENABLE_FEAT_F4HWN_SPECTRUM 1

/* FM broadcast: the RA89R *does* carry a BK1080 FM receiver (docs/ra89r_bk1080.md),
 * on its own bus (PC14/PB2, driver/i2c_bus.c).  The K1's own feature is imported
 * (App/driver/bk1080.c, App/app/fm.c, App/ui/fmradio.c) and the K1 CMake's rule
 * is followed: ENABLE_FMRADIO_EMBEDDED selects the resident state machine and
 * the FM screen (the K1 sets it whenever ENABLE_FMRADIO is on and the overlay
 * apps are not, which is this port's configuration).  The FM audio path (the
 * BK1080's analogue output into the shared amplifier) is unverified -- see
 * docs/ra89r_bk1080.md. */
#define ENABLE_FMRADIO 1
#define ENABLE_FMRADIO_EMBEDDED 1

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
#define EDITION_STRING "RA89R"
#define ALERT_TOT 10

/* The CTSS/squelch tone the K1's CMake sets (App/CMakeLists.txt: SQL_TONE=550). */
#define SQL_TONE 550
#define BUILD_COMMIT "port"

#endif /* APP_K1_FEATURES_H */
