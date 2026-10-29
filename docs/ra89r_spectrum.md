# The spectrum analyzer

The RA89R runs the K1/F4HWN **spectrum analyzer** screen, imported from the
UV-K1/K5V3 tree (fagci's `App/app/spectrum.c`/`.h`, with the F4HWN additions), on
top of this repo's RF drivers.  The one port addition is a **per-spectrum
transceiver choice** — BK4829 or BK4815 — and the receive routing that goes with
it.

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
`APP_RunSpectrum()` under `ENABLE_SPECTRUM` (`app/main.c`; `ENABLE_NOAA`, which
would take the key first, is off), so **F+5** opens the screen.  Long-press 5
remains the scan-range arm; the two do not collide.  `APP_RunSpectrum()` is a
blocking loop (`while (isInitialized) Tick();`), so the port's `rx_service()`
does not run while it is up -- which is why the spectrum has to route the
receive path itself (below).

## Port deviations (exact)

1. `spectrum.h` keeps `#include "py32f0xx.h"`; this repo ships
   `App/py32f0xx.h`, a shim that maps it to `py32f4xx.h`.
2. `UART_ServiceCommands()` stays behind its `ENABLE_UART`/`ENABLE_USB` guard,
   which is off here.
3. A per-spectrum transceiver choice (`SpectrumSettings.chip`) and the RF calls
   that honor it (see below).
4. `BK4819_PickRXFilterPathBasedOnFrequency()` (upstream's band/LNA switch at
   the 280 MHz boundary) is replaced by `SetChipRoute()`; on this board the K1
   hook is a no-op (`driver/bk4819.c`), the lane owns the LNA pins.
5. `App/app/spectrum_rf.{c,h}` — a new, device-free module holding the chip
   policy, so it is host-testable (`tools/test_spectrum.c`).
6. A host stub `void APP_RunSpectrum(void) { }` in `tools/host/host_hw.c`, so the
   preview links (the preview does not drive the blocking screen).

The backlight call is upstream's `BACKLIGHT_Update()` (the per-tick fade step,
which this repo's `driver/backlight.c` has under the same name).  An earlier
revision swapped in `BACKLIGHT_UpdateTickless()`, which loops with 10 ms delays
until the fade ends -- a blocking fade inside `Tick()`.

## The chip choice

`SpectrumSettings.chip` is a `spectrum_chip_t` (`SPECTRUM_CHIP_4829` / `_4815`,
default `4829`), independent of the scanner's `SetScn` and of `TrVfoA`/`TrVfoB`.
It is:

- **selected** in-screen: **long-press `KEY_6`** toggles 4829 ↔ 4815 and
  restarts the sweep (the two chips' RSSI scales and noise floors differ, so the
  history and the auto trigger are rearmed).  **Short-press `KEY_6`** still
  toggles the listen bandwidth, but on *release*, the way the screen already
  handles `KEY_MENU` — otherwise every long press would also change the
  bandwidth at counter 3 and keep toggling it while held.  The value is shown in
  the spectrum frame beside the step count (`DrawNums`), as `4829` / `4815`.
- **persisted** in `Data[4]` of the spectrum's 8-byte store at EEPROM
  `0x00A148` (the F4HWN branch already saves `Data[0..3]` there; `Data[4..7]`
  were free), on EXIT like the rest of the spectrum's settings.  That address is
  inside the port's writable K1-image band (`storage_writable()`,
  `0x4000`–`0x20000`), and the stock leaves the `0xA000` sector erased
  (`stock_eeprom.bin`).  An erased or invalid byte -- including `2`, the removed
  `Both` -- decodes to `4829`.

### Why there is no "Both"

An earlier revision had a third setting, `Both`, alternating the chip per swept
bin "for a faster sweep".  It was removed (2026-10-29) for two reasons:

- **It cannot hear.**  The chips share one set of `0x33` receive-path pins (pin 0
  the BK4829's, pin 4 the BK4815's), and the chip the pins are not set for hears
  next to nothing — radio 2026-10-28, the reason the scanner's `FAST BOTH` and
  `LISTEN` were removed (`docs/ra89r_scan.md`).  Switching the pins per bin is a
  bus write per bin and is just time-sharing.
- **It was not faster.**  `Scan()` tunes and reads one bin at a time on one bus;
  every bin was still measured once, and the BK4815 bins added the 350 µs
  settle, a discarded read and the pin write.  It also tied even bins to one
  chip and odd bins to the other, so their different floors would draw a comb.

## The receive route

`app/spectrum_rf.c` owns the pure policy: `spectrum_rf_next_chip()` (the
toggle), `spectrum_rf_normalize_rssi()` (the BK4815's 7-bit `0x44` masked and
scaled ×4 into the BK4829's 9-bit range, as the FAST scanner's BK4815 lane does),
`spectrum_rf_rssi_settle_us()`, and the `Data[4]` encode/decode.

`spectrum.c` keeps one `activeChip` (= `settings.chip`, set on entry, on the
toggle and by the reset-to-defaults long-press MENU) and routes like the
receiver (`driver/rx.c`):

- **`SetChipRoute(f)`** = `pa_set_rx_lane(activeChip == 4815)` +
  `pa_select_band(f)`.  `pa_set_rx_lane()` sets pin 4 *and clears pin 0* for the
  BK4815 lane (and the reverse for the BK4829) — with both set the BK4815 hears
  nothing (radio 2026-10-21).  `pa_select_band()` keeps `pa`'s band state and
  the BK4815's `0x75` consistent.  It runs on every full tune (`SetF`: peak,
  listen, exit), at each sweep start (`InitScan`), and when a BK4829 sweep
  crosses 280 MHz — not per step.

| operation | BK4829 | BK4815 |
|---|---|---|
| sweep tune (`SetFScan`) | `BK4819_SetFrequency` + `REG_30` (upstream) | the FAST scanner's BK4815 probe: `bk4815_set_frequency` + `bk4815_set_band` |
| full tune (`SetF`) | `BK4819_SetFrequency` + route + `REG_30` | `bk4815_set_frequency` + `bk4815_receive_entry` (bandwidth from the listen BW) + the BK4829 tuned to the same channel + route |
| RSSI | `0x63` glitch wait + `0x67` | 350 µs settle, discarded read, `0x44` normalized |
| filter BW (`0x43`) | written per step | skipped (the BK4815 keeps its receive entry's spacing) |
| listen AF | `0x30` bit 9 + `0x47` bit 8 (upstream) | `bk4815_set_af` (`0x49`) **and** the BK4829's `0x30` bit 9 + `0x47` |

The listen AF and the mirrored BK4829 tune follow what `develop` measured: the
BK4815's audio reaches the amplifier only through the BK4829's AF output (radio
2026-10-27: `0x47 = 0x6040` no audio, `0x6142` a clean voice), and an
unassigned BK4829 on another frequency sums its demodulated noise into that path
(`rf_dual_refresh()`).

The BK4829's receive registers are backed up and restored on entry/exit
(`BackupRegisters`/`RestoreRegisters`); on exit the BK4829 is retuned, and
`rf_dual_reapply()` makes `rx_service()` re-apply both lanes and the VFO's own
receive-path pins.

## Open / radio-gated

- **The BK4815's RF role below 134 MHz is unproven.**  A `4815` sweep at VHF may
  draw a flat trace; the screen must still run and exit cleanly.
- **BK4815 listen audio:** routed as the receiver routes it (above), but not yet
  heard from the spectrum.
- **BK4815 bandwidth:** upstream changes `0x43` per step; the BK4815's spacing
  comes from its receive entry (25 / 12.5 kHz), set on a full tune only.  A
  per-step BK4815 bandwidth is the fallback if the trace shows it matters.
- **BK4815 sweep settle:** 350 µs + one discarded read, the FAST scanner's
  BK4815 probe.  That probe locks on the radio (memory scan, 2026-10-27), but
  whether the delay is enough at spectrum step rates is not measured.

Radio gate before merge: F+5 opens the screen; a sweep draws with chip=4829
and with chip=4815 (a carrier shows on both, at UHF at least); long-press 6
toggles the chip, the label updates and short-press 6 alone changes the listen
BW; the choice survives a power cycle; peak/listen are audible on both chips;
EXIT restores normal receive on a 4829 VFO and on a 4815 VFO.
