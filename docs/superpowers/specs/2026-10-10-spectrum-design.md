# RA89R spectrum design

**Status:** Proposed design; awaiting user review. No spectrum implementation is
authorized by this document yet.

## Goal

Bring the K1/F4HWN **spectrum analyzer** screen to the RA89R, **imported as-is**
(fagci's `App/app/spectrum.c`/`.h`, with the F4HWN additions), with one addition
the user asked for: a **per-spectrum transceiver choice** — BK4829 / BK4815 /
Both — where **Both** splits the swept range across the two chips for a faster
sweep.

Success: pressing the spectrum key opens the K1 spectrum screen on the RA89R
panel; it sweeps, draws, peaks and listens as upstream does; the chip choice is
selectable in-screen, persisted, and works in all three settings; and the radio's
receive state is restored on exit.

## Principle: import as-is, adapt minimally, keep the interfaces

The imported files keep their upstream copyright headers and structure. The only
changes are:

1. build/include/board adaptation (device header, one backlight call, no UART
   overlay);
2. the sweep's RF calls routed through a small **chip-abstraction module**;
3. the chip setting (a field, a key, a status item).

Every deviation is listed in this document and in `docs/ra89r_spectrum.md`, the
way the other imports (`app/chFrScanner.c`, `driver/bk4819.c`) record theirs.
Upstream function signatures and file layout are preserved wherever possible.

## Evidence / current state

- The port's `driver/bk4819.{c,h}` + `bk4819-regs.h` already declare **every**
  `BK4819_*` symbol the spectrum uses (`GetRSSI`, `SetFrequency`,
  `SetFilterBandwidth`, `ToggleGpioOut`, `ReadRegister`, `WriteRegister`,
  `PickRXFilterPathBasedOnFrequency`, the `FILTER_BW_*` values, the `REG_*` enum
  and `GPIO6_PIN2_GREEN`), so the K1-compatible layer is complete for the sweep.
- The port has the rest the spectrum needs: `driver/st7565.{c,h}`,
  `driver/keyboard.{c,h}` (`KEYBOARD_GetKey`), `driver/backlight.{c,h}`,
  `driver/system.h` (`SYSTEM_DelayMs`), `radio.{c,h}` (`RADIO_SetModulation`,
  `RADIO_SetupAGC`, `RADIO_CheckValidChannel`), `settings.c`, `frequencies.c`,
  `ui/helper.{c,h}`, `external/printf/printf.h`, `misc.c`.
- The spectrum's settings store is 8 bytes at EEPROM `0x00A148` (F4HWN branch).
  That address is inside the port's writable K1-image region
  (`K1_IMAGE_BASE 0x04000` .. `K1_IMAGE_END 0x20000`), so the store saves.
- Launch already exists: the K1 app's `KEY_5` handler calls `APP_RunSpectrum()`
  under `ENABLE_SPECTRUM` (`app/main.c`), i.e. **F+5**. Long-press 5 remains the
  scan-range arm, so the two do not collide.
- The BK4815 is a real receiver with its own meters (`0x44` RSSI, `0x43` SNR),
  already used by the `FAST BOTH` scan lane.

## Architecture

- **Imported:** `firmware/App/app/spectrum.c` + `.h`, and
  `firmware/App/app/keyboard_state.h` (a 3-field struct). Enabled by
  `ENABLE_SPECTRUM` + `ENABLE_FEAT_F4HWN_SPECTRUM` in `k1_features.h`; added to
  `CMakeLists.txt`.
- **New:** `firmware/App/app/spectrum_rf.{c,h}` — the chip abstraction, device
  header free and host-testable (like `app/scan_dual.{c,h}`). It owns the
  per-chip RF operations and the step→chip decision; the imported `spectrum.c`
  calls it where it used to call `BK4819_*` for the sweep.

## Chip abstraction (`app/spectrum_rf.{c,h}`)

| operation | BK4829 | BK4815 |
|---|---|---|
| tune | `BK4819_SetFrequency` | `bk4815_set_frequency(f, false)` |
| read RSSI | `BK4819_GetRSSI` (`0x67`, 9-bit) | `bk4815_read_rssi()` (`0x44`, 7-bit), scaled ×4 |
| filter bandwidth | `BK4819_SetFilterBandwidth` | `0x75` band register; the sweep does not change the BK4815 BW |
| AF mute | `BK4819_*` (`0x47`/`0x30`) | `bk4815_set_af(on)` (`0x49`) |
| RX on/off | `BK4819_*` | `bk4815_set_af(false)` / re-apply on exit |

`read_rssi` returns a value normalized to the BK4829's scale so the spectrum's
existing dBm mapping and bar scaling apply unchanged to either chip.

The module also exposes the **step→chip decision**:
- `4829` → every step on the BK4829.
- `4815` → every step on the BK4815.
- `Both` → alternate the chip per swept step (BK4829 even, BK4815 odd), so a
  sweep covers the same range in about half the steps' worth of BK4829 work.

## Chip setting

- `spectrumChip` enum `{SPECTRUM_CHIP_4829, SPECTRUM_CHIP_4815, SPECTRUM_CHIP_BOTH}`,
  default `4829`.
- **In-spectrum** control: a key toggles it, and the value is shown in the
  spectrum's own status line beside step/count/BW (the F4HWN spectrum already
  draws that line).
- **Persisted** in the free byte `Data[4]` of the spectrum's 8-byte store at
  `0x00A148` (the other bytes are already used; the file notes `Data[4..7]` free).
  An absent/invalid value defaults to `4829`.
- Independent of `SetScn` and `TrVfoA`/`TrVfoB`; it affects only the spectrum.

## "Both" semantics

Split the range across the chips: each swept step is measured on one chip,
alternating, so a full sweep completes in about half the BK4829 steps' time. Each
trace point is a single chip's reading (normalized), not a combination. This is
the spectrum analogue of the validated `FAST BOTH` scan.

## Lifecycle

`APP_RunSpectrum()` is a blocking loop, so the port's `rx_service()` does not run
while it is up — no shared-state conflict. On entry the spectrum backs up the
BK4829 registers (upstream `BackupRegisters`); on exit it restores them
(upstream `RestoreRegisters`) and the port re-applies the BK4815's receive state
(`bk4815_configure()` / `rf_dual_reapply()`), so normal receive resumes.

## Adaptation list (exact)

1. Add `app/keyboard_state.h`; `spectrum.h` keeps its `#include "keyboard_state.h"`.
2. Device header: upstream `#include "py32f0xx.h"` → the port's device header;
   `board.h` resolves to the port's `App/board.h`.
3. `BACKLIGHT_Update` → `BACKLIGHT_UpdateTickless`.
4. `UART_ServiceCommands` stays behind its `ENABLE_UART`/`ENABLE_USB` guard, which
   is off here (no UART overlay).
5. Sweep RF calls (`SetF`, `SetFScan`, `Measure`, `ToggleRX`, `ToggleAFDAC`,
   `ToggleAudio`) routed through `spectrum_rf_*`.
6. Chip field in `SpectrumSettings`, its key, its status item, and `Data[4]` in
   `LoadSettings`/`SaveSettings`.
7. `k1_features.h`: `ENABLE_SPECTRUM` + `ENABLE_FEAT_F4HWN_SPECTRUM`; `CMakeLists.txt`
   gains `App/app/spectrum.c` and `App/app/spectrum_rf.c`.

## Testing / verification

- `firmware/tools/test_spectrum.c`: pure tests of the chip abstraction — the
  step→chip split for each of the three settings, RSSI normalization, and the
  chip-setting encode/decode (including an invalid byte). Added to
  `tools/check_all.sh`.
- `preview_k1.c`: a smoke check that `spectrum_rf` links and the chip setting
  round-trips; the spectrum's own blocking screen is not host-driven.
- `tools/check_all.sh` + target build + `ra89r.py verify`.
- **Radio gate (merge condition):** F+5 opens the screen; a sweep draws on the
  BK4829; `4815` sweeps on the BK4815; `Both` sweeps with alternating chips and
  looks about twice as fast; peak/listen work; exiting restores normal receive.
  Nothing merges to `develop` until these pass.

## Non-goals

- Do not change `SetScn` or `TrVfoA`/`TrVfoB`.
- Do not reimplement the spectrum on the scan engine.
- Do not add a spectrum menu item; the choice is in-screen.
- Do not validate the BK4815's RF role below 134 MHz here; that is a radio gate.

## Open risks

- **BK4815 RF role below 134 MHz** is unproven, so `4815`/`Both` at VHF/UHF is
  radio-gated; the screen still runs, but a trace may be flat if the part is not
  the receive path there.
- **BK4815 bandwidth for the sweep:** upstream sets `0x43` per step; the BK4815's
  equivalent is not established, so the design keeps the BK4815 at its configured
  bandwidth for the sweep. If that proves wrong on the radio, a per-chip BW step
  is the fallback.
- **BK4815 register backup/restore:** upstream backs up the BK4829; the BK4815's
  restore is delegated to the port's `bk4815_configure()`/`rf_dual_reapply()`.
