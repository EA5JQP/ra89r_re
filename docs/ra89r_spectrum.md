# The spectrum analyzer

The RA89R runs the K1/F4HWN **spectrum analyzer** screen, imported from the
UV-K1/K5V3 tree (fagci's `App/app/spectrum.c`/`.h`, with the F4HWN additions), on
top of this repo's RF drivers.  The one port addition is a **per-spectrum
transceiver choice** — BK4829 / BK4815 / Both — where `Both` splits the swept
range across the two chips for a faster sweep.

Nothing here is called *verified* until it has run on the radio.

## What is imported

| file | state |
|---|---|
| `firmware/App/app/spectrum.c` `.h` | upstream (fagci), with the port deviations below |
| `firmware/App/app/keyboard_state.h` | verbatim (Armel F4HWN; a 3-field key state struct) |

Enabled by `ENABLE_SPECTRUM` and `ENABLE_FEAT_F4HWN_SPECTRUM` in
`firmware/App/k1_features.h`.  `ENABLE_FEAT_F4HWN_SPECTRUM` is the version with
the persisted settings, the interlaced sweep and listen mode.

**Launch:** the K1 application's `KEY_5` handler already calls
`APP_RunSpectrum()` under `ENABLE_SPECTRUM` (`app/main.c`), so **F+5** opens the
screen.  Long-press 5 remains the scan-range arm; the two do not collide.
`APP_RunSpectrum()` is a blocking loop (`while (isInitialized) Tick();`), so the
port's `rx_service()` does not run while it is up.

## Port deviations (exact)

1. `spectrum.h` keeps `#include "py32f0xx.h"`; this repo ships
   `App/py32f0xx.h`, a shim that maps it to `py32f4xx.h`.
2. `spectrum.c` calls `BACKLIGHT_UpdateTickless()` where upstream calls
   `BACKLIGHT_Update()` (this repo's name).
3. `UART_ServiceCommands()` stays behind its `ENABLE_UART`/`ENABLE_USB` guard,
   which is off here.
4. A per-spectrum transceiver choice (`SpectrumSettings.chip`) and the RF calls
   that honor it (see below).
5. `App/app/spectrum_rf.{c,h}` — a new, device-free module holding the chip
   policy, so it is host-testable (`tools/test_spectrum.c`).
6. A host stub `void APP_RunSpectrum(void) { }` in `tools/host/host_hw.c`, so the
   preview links (the preview does not drive the blocking screen).

## The chip choice

`SpectrumSettings.chip` is a `spectrum_chip_t`
(`SPECTRUM_CHIP_4829` / `_4815` / `_BOTH`, default `4829`), independent of the
scanner's `SetScn` and of `TrVfoA`/`TrVfoB`.  It is:

- **selected** in-screen: **long-press `KEY_6`** cycles 4829 → 4815 → Both
  (short-press `KEY_6` still toggles the listen bandwidth).  The value is shown
  in the spectrum frame beside the step/count (`DrawNums`), as `4829` / `4815` /
  `BOTH`.
- **persisted** in `Data[4]` of the spectrum's 8-byte store at EEPROM
  `0x00A148` (the F4HWN branch already saves `Data[0..3]` there; `Data[4..7]` were
  free).  That address is inside the port's writable K1-image region
  (`0x4000`–`0x20000`), so it saves.  An absent/invalid byte decodes to `4829`.

`app/spectrum_rf.c` owns the policy:

- `spectrum_rf_step_chip(setting, index)` — the chip for a swept step.  For
  `BOTH` it alternates (even index → BK4829, odd → BK4815), so a sweep covers the
  range in about half the BK4829 steps' worth of work.
- `spectrum_rf_normalize_rssi(chip, raw)` — the BK4815's 7-bit `0x44` value is
  scaled ×4 into the BK4829's 9-bit scale; the BK4829's `0x67` is unchanged.
- `spectrum_rf_encode_chip` / `_decode_chip` — the `Data[4]` byte.

`spectrum.c` resolves `activeChip = StepChip(index)` before each tune/read
(`SetFScan`, `GetRssi`) and before tuning a peak (`TuneToPeak`), and branches the
RF calls on it:

| operation | BK4829 | BK4815 |
|---|---|---|
| tune | `BK4819_SetFrequency` + `REG_30` | `bk4815_set_frequency` + `0x75` band |
| RSSI | `0x63` glitch wait + `0x67` | `bk4815_read_rssi` (`0x44`), scaled ×4 |
| filter BW (`0x43`) | written per step | skipped (BK4815 keeps its configured BW) |
| AF mute | `0x30`/`0x47` | `bk4815_set_af` (`0x49`) |

The BK4829's receive state is backed up and restored on entry/exit
(`BackupRegisters`/`RestoreRegisters`); on exit `rf_dual_reapply()` re-applies
the BK4815's receive routing, so a `4815`/`Both` session does not leave it
mid-sweep.

## Open / radio-gated

- **The BK4815's RF role below 134 MHz is unproven.**  `4815`/`Both` at VHF/UHF
  may draw a flat trace; the screen must still run and exit cleanly.
- **BK4815 audio during listen:** the port opens the BK4815's own AF
  (`0x49`) but not the BK4829's shared AF node, which the scan path found the
  BK4815's audio rides.  If a `4815`/`Both` listen is inaudible, that is the
  first thing to check (`driver/rx.c` has the validated handling).
- **BK4815 bandwidth:** upstream changes `0x43` per step; the BK4815's equivalent
  is not established, so its lane keeps one bandwidth.  A per-chip BW step is the
  fallback if the radio shows it matters.
- **BK4815 sweep settle:** before sampling `0x44`, the spectrum now waits 350 µs
  (the same fixed delay used by the validated FAST BOTH scanner lane), discards
  the first RSSI read, then uses the next.  The BK4815 has no documented glitch
  indicator like the BK4829's, so whether 350 µs is sufficient for a spectrum
  sweep remains radio-gated.

Radio gate before merge: F+5 opens the screen; a sweep draws with chip=4829;
`4815` sweeps on the BK4815; `Both` sweeps with alternating chips and looks about
twice as fast; long-press 6 cycles the chip and the label updates; the choice
survives a power cycle; peak/listen tune the right chip; EXIT restores normal
receive.
