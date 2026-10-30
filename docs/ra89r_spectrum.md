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
scaled ×4 into the BK4829's 9-bit range, as the FAST scanner's BK4815 lane does)
and the `Data[4]` encode/decode.

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
- **A BK4815 sweep start** (`InitScan`) also idles the BK4815's T/R state
  (`0x0C = 0x0A03`, restored to its entry state on exit), runs its receive
  entry (the `0x70` restart) and waits (≤ 50 ms) for `0x44` to leave 0.  Without
  them a BK4815 that no VFO was using, or one parked by a boot/PTT with the
  BK4829 on VHF, read `0x44 = 0` on every bin (radio 2026-10-29/30).

| operation | BK4829 | BK4815 |
|---|---|---|
| sweep tune (`SetFScan`) | `BK4819_SetFrequency` + `REG_30` (upstream) | the FAST scanner's BK4815 probe: `bk4815_set_frequency` + `bk4815_set_band` |
| full tune (`SetF`) | `BK4819_SetFrequency` + route + `REG_30` | `bk4815_set_frequency` + `bk4815_receive_entry` (bandwidth from the listen BW) + the BK4829 tuned to the same channel + route |
| RSSI | the FAST scanner's settled read, `CHFRSCANNER_ReadRssiSettled()` | `Read4815Settled()`: the value after the meter's 2nd update (≤ 24 ms), normalized |
| filter BW (`0x43`) | written per step | skipped (the BK4815 keeps its receive entry's spacing) |
| listen AF | `0x30` bit 9 + `0x47` bit 8 (upstream) | `bk4815_set_af` (`0x49`) **and** the BK4829's `0x30` bit 9 + `0x47` |

**The RSSI reads are the measured part.**

- *BK4829:* the K1's glitch wait + one `0x67` read put noise peaks (75–83) over
  the auto trigger (71): every half-sweep stopped on noise, listen opened and
  closed, no full sweep completed, and the peak label kept resetting to 0 (radio
  2026-10-29).  `0x67` climbs after the `REG_30` restart; the FAST scanner's
  plateau mean (radio 2026-10-27, `app/chFrScanner.c`) gives noise 56–64
  against a trigger of 72.
- *BK4815:* `0x44` is a sample-and-hold updating about every 8 ms, and the first
  update after a retune still carries the old frequency — one sample per ms
  after a jump: `0 0 0 0 0 20 ×8 10 ×8 13 …` (radio 2026-10-30).  350 µs, or a
  plateau of back-to-back reads (all the same held value), read earlier bins: a
  carrier peaked up to ±400 kHz off with the sweep direction, and the first bins
  after each sweep's jump back to the left edge opened listen on the held level.
  Waiting for two updates costs ~23 ms a bin: a 64-bin round trip takes ~3 s on
  the BK4815 against ~1 s on the BK4829.

The listen AF and the mirrored BK4829 tune follow what `develop` measured: the
BK4815's audio reaches the amplifier only through the BK4829's AF output (radio
2026-10-27: `0x47 = 0x6040` no audio, `0x6142` a clean voice), and an
unassigned BK4829 on another frequency sums its demodulated noise into that path
(`rf_dual_refresh()`).

The BK4829's receive registers are backed up and restored on entry/exit
(`BackupRegisters`/`RestoreRegisters`); on exit the BK4829 is retuned, and
`rf_dual_reapply()` makes `rx_service()` re-apply both lanes and the VFO's own
receive-path pins.

## Radio results

| check | result |
|---|---|
| F+5 opens the screen, EXIT returns | ✔ (2026-10-29/30) |
| BK4829, PMR2 from a second radio / tinySA | peak on **446.01875**, RSSI 185–213 against noise 56–64, trigger 72; listen held for the carrier, sweep resumed after |
| BK4829 listen audio | PMR2 **audible** (2026-10-29) |
| BK4815, tinySA -60 dBm on 446.01875 | peak on **446.01875** (bin 31, RSSI 376 scaled) against noise 88–96, trigger 104; one listen per carrier-on, no false opens (2026-10-30, after the driver's `0x75` bit-10 fix) |
| long-press 6 toggles the chip, label updates | ✔ |

The BK4815 result depends on two driver fixes found on this branch's bench and
kept on `driver/bk4815-tuning` (`docs/ra89r_bk4815.md`, top notes): `0x75` bit
10 = 1 (the part had received f − 275 kHz) and the `0x0C` un-park.

The `spec:` console trace (key, peak, listen, half-sweep and per-second sweep
lines, and the BK4815 step response on a tune to a peak) is on while the
feature is radio-gated: `SPECTRUM_TRACE` in `app/spectrum.c`, `-DSPECTRUM_TRACE=0`
drops it.

## Open / radio-gated

- **VHF on either chip is not measured** (the tinySA was on its high-output
  connector).  The BK4815 below 134 MHz is unproven.
- **BK4815 listen audio** from the spectrum: routed as the receiver routes it,
  not yet heard (the bench carrier was unmodulated).
- **One unexplained BK4815 open** at 445.69375 (RSSI 120, −325 kHz from the
  carrier) around a generator switch, once in a 90 s run.
- **BK4815 bandwidth:** upstream changes `0x43` per step; the BK4815's spacing
  comes from its receive entry (25 / 12.5 kHz), set on a full tune only.
- **Short-press 6 on release, and the chip choice surviving a power cycle**,
  are not checked on the radio.
