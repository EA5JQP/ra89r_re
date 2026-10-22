# RX sensitivity (both transceivers), dual-lane scan throughput, LISTEN

Date: 2026-10-21.  Branch: `scan-listen` (off `develop` `e746f93`; already carries
`06af313`, the LISTEN mode).  All three items are validated in one radio session
and merged only after that.

## Goal

1. **Sensitivity.**  Close the receive-path gaps the stock image shows, on both
   parts and in both bands, and make the user's squelch setting actually set the
   opening threshold.
2. **Scan throughput.**  The two lanes are time-multiplexed today (measured 1.7x
   over FAST).  Pipeline the probes so each chip's settle time overlaps the other
   chip's bus traffic.
3. **LISTEN.**  Radio-validate the listen-while-scanning mode already on this
   branch (gates in `2026-10-19-listen-while-scanning.md`).

Transceiver selection stays **manual per VFO** (`TrVfoA`/`TrVfoB`), as decided:
no AUTO-by-frequency.  FAST uses the selected VFO's chip, FAST BOTH uses both.

## What the stock does — the register evidence (Ghidra, `stock_v49_raw`)

Complete write map of both parts, from every caller of the two register layers
(`FUN_080220A0` = BK4829 write, `FUN_08022082` = BK4815 write, `FUN_08021F78` =
BK4815 block write), 98 functions walked.  The receive-relevant parts:

### BK4829 receive entry — `FUN_08016EC8` -> `FUN_08016DE8(vfo)`

```
0x30 = 0x0200                      (mute before the tune)
FUN_08017176 -> 0x38/0x39 = word   (tune)
0x32 = 0x0244
FUN_0800B634(vfo[0x79] - 1)        0x09 = 16-tap channel filter, 0x21 = 0x06D8,
                                   0x24 = (state << 7) | 0x807E
0x24 = 0                            (when no bandwidth index)
0x47 = 0x6042                      (AF output)
0x4A = 0x5450 / 0x5448             (by vfo[0x75])
FUN_08006E6A -> FUN_0801763C       0x43 = 0x3028 (wide) / 0x4048 (narrow)
FUN_08006DE4                       0x51 = 0x9032 / 0x801D, 0x52 = table
FUN_08019E2C(vfo[0x77])            0x31 = 0/4 (+2 with a profile)
FUN_080137D4(3, 0)                 0x33 band pins 0/1 cleared
FUN_08013790(1|2)                  BK4815 0x75 band register
FUN_080137D4(0x10, 0)              0x33 pin 4 CLEARED -- the BK4829 branch
FUN_0801719A                       0x78 = (thr-2) | (thr << 8)   -- squelch marks
FUN_0801887C                       RSSI/SNR UI calibration
0x50 = 0x3B20                      (AF DAC on)
FUN_08018A3C -> 0x37/0x30 ladder   (0x9D1F, 0x0200, 0xBDF1, 0xBFF1)
```

Squelch (`FUN_080052B8`, polled): reads `0x63`/`0x65`/`0x67`, keeps `0x13` at
`0x3B0 | (8 - level)` between `0x3FF` (fully open) and the level word.  The
threshold `param_1` is the per-channel field `vfo+0x22`, from `FUN_0801D568`
(the codeplug's calibrated squelch level, `FUN_0800E3F6` over the EEPROM
tables; the squelch page is EEPROM `0x3710`, the RSSI page `0x3860`).

### BK4815 receive entry — `FUN_08016CEC(vfo)`

```
0x33 pin 4 SET (BK4815 branch)
FUN_0801703C                       0x04 = band<<7|0xB041, 6 bytes at 0x70
                                   (0xA000 + word), 4 bytes at 0x7E (RX IF word)
0x16 = 0xB200
0x2B = 0x403F
0x6D = 0x6618
0x73 = 0x81D0
FUN_08016F28(0x226, 2)             0x5B = 0x2616 (CTCSS symbol)
FUN_08006E4C(vfo[0x91])            0x01 = 0x0044 (narrow) / 0x8044 (wide)
FUN_08019DF4(vfo[0x77])            0x48 = 0xA002, or 0x8002 + 0x4A = profile
FUN_0800B520(profile)              compander (0x42/0x4C/0x4D/0x4E/0x56/0x57/0x73)
FUN_08013790(band)                 0x75 = band
0x70 = 0; 0x70 = 0xA000            (receive restart pair)
```

Gain is **not** per-VFO on this part: the boot block's `0x67 = 0xC31F` fixes
`REG103`: RX AGC disabled, RSSI offset 3, LNA high gain, IF PGA +21 dB.  The
port writes the same word, so the chip is already at maximum gain.

### The port's gaps (the work)

| # | gap | effect | fix |
|---|---|---|---|
| 1 | `0x33` pin 4 (VHF LNA path) is set whenever **any** VFO names the BK4815, not when the BK4815 is the lane being **listened to** | measured on the radio: pin 4 set costs the BK4829 ~16 dB (`0x67` 199 vs 232 at 145.500) | pin 4 follows the audio lane: set for a BK4815 lane, cleared for a BK4829 lane |
| 2 | the BK4815 gets only the tune + `0x75`; the rest of `FUN_08016CEC` is missing | stock parity for demod/AF/baseband (`0x01`, `0x16`, `0x2B`, `0x6D`, `0x73`, `0x70` restart) is unproven either way; the `0x01` channel-spacing bit is the documented bandwidth control | `bk4815_receive_entry()` after each lane (re)configuration, not per scan probe |
| 3 | squelch marks are fixed (`0xCF`/`0xB4` on `0x67`, 30/24 and 20/14 on `0x44`) and ignore `SQUELCH_LEVEL` | the knob does nothing on either lane; the default 4 behaves like 8 and hides weak signals | marks derived from `SQUELCH_LEVEL`, with level 8 reproducing the measured pair |
| 4 | squelch threshold source | the stock derives it from calibrated EEPROM pages (`0x3710` squelch, `0x3860` RSSI) that this radio still has | follow-up (documented, not in this change) |

## Design

### 1. Lane path pin (`driver/rx.c`)

`rx_apply_lane_path(bool use_4815)` writes `0x33` pin 4 only on a change, through
`BK4819_ToggleGpioOut` so the other output bits survive.  Called:
* from `rx_set_audio_source()` (a VFO switch or a scan-source change),
* at the end of `rf_dual_refresh()` (the K1's own radio setup clears the pin),
* by `rx_service()`'s scan-override branch.

`rf_dual_refresh()`'s "set pin 4 if a BK4815 VFO exists" block is removed; the
lane rule replaces it.

### 2. BK4815 receive entry (`driver/bk4815.c`)

`bk4815_receive_entry(freq_10hz, wide)` writes, in `FUN_08016CEC`'s order after
the tune: `0x16 = 0xB200`, `0x2B = 0x403F`, `0x6D = 0x6618`, `0x73 = 0x81D0`,
`0x01 = wide ? 0x8044 : 0x0044`, then `0x70 = 0`, `0x70 = 0xA000`.  It is called
with the VFO's own `CHANNEL_BANDWIDTH`, from the same place `rf_dual_apply()`
runs (lane (re)configuration), and never from the scanner's probe loop.

### 3. Squelch from the setting (`driver/rx.h` inlines)

Pure helpers, host-tested:

```
BK4829 (0x67): open = 0xCF - (8 - level) * 6      (level 1..8, 9 = 8)
               close = open - 27                  (the stock pair's hysteresis)
BK4815 UHF     (0x44): open = 30 - (8 - level) * 2, close = open - 6
BK4815 VHF     (0x44): open = 20 - (8 - level) * 2, close = open - 6
level 0 = off (always open, as today)
```

Level 8 reproduces exactly the pairs measured on the radio
(`docs/ra89r_bk4815.md`, `docs/ra89r_scan.md`), so the change is a scaling of a
known point rather than a new calibration.

## Scan throughput (phase 2)

`ScanBothProbe()` today tunes a candidate, waits, reads, then does the next one.
New shape: **two candidates in flight** -- tune the BK4829 candidate, tune the
BK4815 candidate (the BK4815's bus traffic covers the BK4829's settle), then
read both RSSIs.  The band/path switch stays a single change per pair; `0x75` is
only written when the band changes.  The bench metric is console `J`'s probes/s
(currently 97/s FAST, 168/s FAST BOTH).

## Radio validation gates

Sensitivity:
* at 145.5000 and 446.00625 with a keyed carrier at a known low level, `S`/`J`
  RSSI and the opening level on each lane, with `TrVfo` set to each chip;
* with VFO A on the BK4829 and VFO B on the BK4815, the BK4829 lane's `0x67`
  must be the *pin-4-clear* value (~232 at 145.5), not the pin-4 one (~199);
* SQL 0 opens at any level; SQL 1 opens earlier than SQL 8; SQL 0..9 is monotone;
* the BK4815 entry writes read back on the console (`0x01`, `0x16`, `0x2B`,
  `0x6D`, `0x73`);
* no regression in transmit or in the stock boot configuration replay.

Scan:
* `J` probes/s FAST BOTH >= the current 168/s, and FAST unchanged;
* hits on both lanes still pause/resume and open the right audio source.

LISTEN: the 2026-10-19 gates.

## Out of scope

* AUTO-by-frequency transceiver selection (decided against; `TrVfoA/B` stay
  manual).
* Reading the squelch/RSSI calibration pages into the port (gap 4).
* The BK4815 compander and AF profile (`0x42/0x4C/0x4D/0x4E/0x56/0x57`, `0x48`/
  `0x4A`) -- audio, not RF sensitivity; a separate feature.
* Making the single-lane FAST precheck lane-aware for a BK4815 VFO.

## Implementation status (2026-10-21)

Done on `scan-listen`, target build clean:

* `driver/pa.c`/`.h`: `pa_set_rx_lane()` and the lane rule in
  `pa_apply_chip_path()` (AUTO); `BK4819_PickRXFilterPathBasedOnFrequency()` no
  longer writes the LNA pins (`driver/bk4819.c`).
* `driver/rx.c`: `rx_set_audio_source()` sets the lane; `rx_poll()` uses
  `rx_squelch_open_mark()`/`rx_squelch_close_mark()` on both lanes;
  `rf_dual_refresh()` calls `bk4815_receive_entry()` with the VFO's bandwidth.
* `driver/bk4815.c`/`.h`: `bk4815_receive_entry(bool wide)`.
* `app/chFrScanner.c`: `SCAN_BOTH_PRECHECK_PAIRS` = 12 (24 candidates per pass)
  and the paired probe (`ScanBothTune`/`ScanBothRead`/`ScanBothProbePair`,
  `ScanBothPairUsable`); no candidate array any more.
* `app/chFrScanner.c`: `ScanFastReceiveMark()`/`ScanFastSquelchEnabled()` --
  the precheck trigger is the level-derived mark, not the K1's blank-flash
  EEPROM threshold (which made it 247, unbeatable).  This is the single biggest
  scan-sensitivity fix in the change.
* `App/main.c`: `J` prints the squelch level, both lanes' marks and the lane
  pin state.
* `firmware/tools/test_rf_dual.c`: receive-entry register checks and the
  squelch-mark ladder (level 8 = the measured pairs, monotone, hysteresis).

Not yet done / blocked:

* **Host tests were run in a container** (this host has no x86 gcc, only the
  ARM toolchain): `podman run --rm -v "$PWD":/w -w /w docker.io/library/gcc:13
  bash firmware/tools/check_all.sh` -- all suites pass (103 test_rf, 0 failed
  test_rf_dual, 12 beeper, 86 bk1080, scan_dual and CAT OK, preview_k1 0
  failures).  The only warnings are the pre-existing host 64-bit pointer casts
  in `backlight.c`.
* The radio console is unreachable because the CH340 is on the USB-C port;
  flashing needs the Kenwood jack (update mode is the bootloader's USART1).
  Every gate above is still open.

## Radio session 2026-10-21 (automated, tinySA over the two serial ports)

### Measured with the port build, receive only (no PTT, no TX register)

BK4829, selected lane, `0x67` (9-bit), carrier from the tinySA:

| band | floor | −38 dBm | −30 dBm | −20 dBm |
|---|---|---|---|---|
| 446.00625 | 56.5 (54-59) | +5 | **+21** (~10.7 dB) | 199, saturated |
| 145.5000 (earlier) | 96-103 | — | carrier 233-252 | — |

The saturation at 199 is the AGC table's lowest-gain entry, not a
sensitivity limit.  Rough 10 dB quieting point: **−31 dBm** (with this
coupling).

BK4815, same antenna and a correct tune (`0x71/0x72 = 0x893b91b9` = 446.00625,
`0x75 = 0x0A`, `0x0C = 0x0A03`): **no response at all**, in either band, over
28 dB of generator level.  Swept with every receive-path combination the port
can drive -- `0x33` pin 0 set/cleared, pin 2 (`0x10`) set/cleared, pin 4 set,
PA1 high/low -- and always 13-15 (VHF) or 11-13 (UHF)
flat.  Its earlier documented response (16 -> 41 at 145.75) is **not
reproducible now**; either it was measured through the 4829's coupling or the
board routes the 4815's RF input with a line this port does not drive.  Until a
scope/logic-analyser trace of the stock settles it, **the BK4815 is not a usable
receiver in this port** and both VFOs belong on the BK4829 (the settings
default).

Two real fixes came out of the session:

* `0x75` must keep **bit 10 = 0** (low-side injection) for this port's
  synthesiser word, which omits the IF offset the datasheet's high-side form
  needs.  Preserving the part's reset `0x0400` (high side) gave a constant
  LO-leakage level (0x44 = 28 over 145-146.5, 21 at 174) that no carrier moved;
  with low side the baseline returns to 13.5.
* `0x33` pin 0 (`0x40`, the K1's "RX_ENABLE") is cleared on a BK4815 lane, as
  the stock's receive entry does (`FUN_080093EC` -> `FUN_080137D4(3, 0)`); a
  BK4829 lane keeps it.

### Scan throughput, `J` benchmark, 446.00625..447.00625, 12.5 kHz

| build | FAST | FAST BOTH | ratio |
|---|---|---|---|
| before this branch | 97/s | 168/s | 1.73x |
| 12-candidate batch, old trigger | 335/s | 502/s | 1.49x |
| 48-candidate batch + settle skip | 401/s | **671/s** | **1.67x** |

Absolute Both throughput is **4x** the branch's starting point.  At 145.5 the
same build measured 1.08x with a 53 ms/batch gap: the 4815 lane's jitter read
as a hit every batch and each one bought a full re-tune.  The confirmation
probe and the 12-count 4815 margin in this change target exactly that; the
number is still to be measured.

### Squelch gate, final run (2026-10-22, UHF, SQL 4)

| generator | `0x67` | gate | floor |
|---|---|---|---|
| off | -- | **closed** | 49 |
| -20 dBm | 101 | **OPEN** | 52 (stable) |
| -38 dBm (stage minimum) | 156 | OPEN | 51 |
| off | 58 | **closed** | 51 |

The high output stage cannot go below -38 dBm, so a below-threshold carrier
could not be produced at UHF; that side is covered by the host tests and by the
off measurement either side of it.  Floor tracking survived a retune with a
carrier present (the seed path) and a 40 dB level change without moving.

### Squelch gate (earlier notes)

With the fixed marks the 4815 lane sat permanently open at 145.5 (mark 12
against a floor of 13.8) and imported 100+ counts above the floor on the 4829.
The floor + margin rule is in this change; the first flash of it exposed a
floor-tracker bug (a mid-retune sample dragged the floor to 1, open mark 11 <
noise 13), now guarded by a per-lane minimum and a plausible-drop limit.

## The CPS test map (`docs/TestData-FackPrichal.test`)

A TYT CPS map (21-byte channel records, `MapRev=1`), 0x0000-0x16A0.  Its
calibration region starts one 16-byte page earlier than this radio's, and at
that shift its `0x0700` page is the same **squelch page** layout as the RA89R's
external flash `0x3710` (4-byte groups, mostly `17 23 1A 25`), with 28 of 336
bytes different -- i.e. a different unit's values, same table.  It carries no
copy of the `0x3860` RSSI page.  Useful as a second sample of the *layout*, not
as this radio's calibration, and it does not decode the table's fields.
