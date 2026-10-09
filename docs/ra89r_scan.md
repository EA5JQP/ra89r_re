# The scan, and the dual-transceiver scan

The K1/F4HWN frequency-range and scan-list scanners are imported almost
unchanged (`app/scanner.c`, `app/chFrScanner.c`, `ui/scanner.c`).  What the port
adds on top is a **dual-transceiver** mode: the `SetScn` menu item can run the
scanner over both RF parts (BK4829 and BK4815) as interleaved lanes instead of
only the selected VFO's chip.

This file records what is implemented, what is a software fact, and what is still
only radio-validated work.  Nothing here is called *verified* until it has run on
the radio.

## The scan modes

The single `SetScn` menu item (`MENU_SET_SCN`) is the scan mode, persisted in the
port's own settings blob (not the read-only stock codeplug) as `scan_mode_t`
(`settings.h`).  It is the K1's `gSetting_set_scn` at run time:

| value | label | meaning |
|---|---|---|
| 0 | `NORMAL` | step scan, no fast precheck |
| 1 | `FAST` | single-lane fast-RSSI precheck on the selected VFO's chip |
| 2 | `FAST BOTH` | the candidate stream is split across the BK4829 and the BK4815 |

`ScanFastEnabled()` is `!= NORMAL`; `ScanBothEnabled()` is `== FAST BOTH`.

The value is persisted, which the K1's `gSetting_set_scn` was not.  The extra blob
is now version 4 and reuses the byte the old `ScTrMd` used.  Migration: a v3 blob
held the old `ScTrMd` byte (0 `Default` → `FAST`, 1 `Both` → `FAST BOTH`); a v2
blob has no byte and loads as `FAST`; an invalid v4 value clamps to `FAST`
(`settings.c` keeps the older layouts explicit).  `preview_k1.c` checks both
migrations.

## The candidate stream and the lane rule

`CHFRSCANNER_NextCandidate()` (`app/chFrScanner.c`) produces the scanner's items
in exactly the K1 order — for a frequency range the same step/limits/skip walk as
`ScanRangeNextFrequency()`, for a list the same membership/priority/direction/wrap
as `RADIO_FindNextChannel()` + the scan-list rotation.  Each item carries a
monotonically increasing `ordinal`.  The list cursor was factored out into
`CHFRSCANNER_NextMemCursor()` so the Default path and the dual path share it.

`app/scan_dual.c` owns the pure decisions and is host-tested
(`tools/test_scan_dual.c`, linked by `tools/check_all.sh`):

* **Lane assignment** (`scan_dual_assign_candidate`): above the stock's 134 MHz
  split (`frequency_10hz > 13400000`) even ordinals go to the BK4829 and odd to
  the BK4815; at or below the split *every* candidate stays on the BK4829, so no
  candidate is lost to an unproven BK4815 band.
* **Per-lane RSSI state** (`scan_dual_rssi_candidate`): each lane learns its own
  noise floor and tests its own reading.  This is mandatory: the BK4829 reports a
  9-bit RSSI in `0x67` and the BK4815 a 7-bit one in `0x44`, so raw values are
  never compared across lanes.
* **Hit arbitration** (`scan_dual_choose_hit`): if both lanes hit in one batch the
  lower original ordinal wins, so scan order is preserved.

## What the "FAST BOTH" path actually does (software, not yet radio-validated)

`ScanBothFastPrecheck()` in `app/chFrScanner.c` runs when `SetScn = FAST BOTH`
and the fast scan is enabled:

1. pull a full fast batch per lane (`2 * SCAN_FAST_PRECHECK_STEPS` candidates)
   from the shared cursor;
2. probe each candidate on its assigned chip in ordinal order — the BK4829 lane
   through the existing `ScanFastTune`/`ScanFastReadCandidateRssi`, the BK4815
   lane through `bk4815_set_frequency` + `0x44`;
3. update each lane's floor with `scan_dual_rssi_candidate`;
4. on a hit, point `gRxVfo` at the winning candidate and set the RX scan-source
   override to the winning chip, then let the existing full single-candidate
   tune/verify path run.

The RF path is **shared**: the band/path is switched before a lane's sample
(`pa_select_band`), and the two parts share one bit-banged bus, so the lanes are
time-multiplexed, not simultaneous.  No wall-clock speed-up is claimed until it
is measured on the radio.

Both lanes feed the RSSI sparkline: the BK4829's 9-bit `0x67` value as-is and the
BK4815's 7-bit `0x44` value scaled ×4 into the same range, so the graph works in
`FAST BOTH` and a spike means the same thing on either lane.  The BK4815 has its
own meters (`0x44` RSSI, `0x43` SNR) — the stock uses them for its >134 MHz
signal detect (`FUN_08005218`, `docs/ra89r_bk4815.md`) — but no documented glitch
indicator, so its lane uses a fixed settle delay instead of the BK4829's glitch
wait.

**Scan-list scope:** only the frequency range is interleaved for now.  The list
keeps the K1's single-lane fast precheck so its priority/rotation behaviour stays
exactly as validated; the spec allows this as the conservative fallback.  This is
a deliberate limit, not an accident, and is the first thing to revisit if the
list is to be split too.

**Single-lane `FAST` and a BK4815-assigned VFO:** the K1 fast precheck is
hardcoded to the BK4829, so with `TrVfoA/B = 4815` the single-lane precheck
probes the BK4829 while the full verify path follows the BK4815 assignment.
Making the precheck lane-aware is future work; `FAST BOTH` is the mode that uses
both chips explicitly.

## The RX scan-source override

A paused hit may have been found by the BK4815 while the selected VFO is normally
assigned to the BK4829 (or vice versa).  `driver/rx.c` gained a temporary
override (`rx_set_scan_source_override` / `rx_clear_scan_source_override`,
`rx_scan_source_t`): while set, `rx_service()` follows the hit's chip and
frequency instead of the saved VFO route, retuning the hit's chip explicitly —
the BK4815 for a BK4815 hit, the BK4829 for a BK4829 hit (the K1 receive setup
only tunes the BK4829 when the selected VFO is assigned to it).  It never
modifies the saved `TrVfoA`/`TrVfoB` values, and it is cleared on scan start,
resume and stop (checked in `preview_k1.c`).

## Diagnostics

Console `J` (`App/main.c`) now also prints the `SetScn` mode, the override state,
the `scan_dual_stats_t` snapshot (per-lane candidate counts, last frequency, last
RSSI, selected hit), and **total RSSI probes / elapsed wall time / probes per
second** via `CHFRSCANNER_GetScanDualStats()`.  Rate timing starts when a new
scan starts and includes scheduler cadence and serialized bus work; the count
includes completed RSSI probes, including range-refinement samples.  The
diagnostic reads state only; it never tunes hardware to print.

`J` then runs the automated benchmark below; the probe/elapsed figures are
sampled while the app loop is still live, before the diagnostic sampling pauses
it.

### Required speed comparison (automated)

Console `J` runs the comparison for you.  It arms one shared range on the selected
RX VFO (forcing it to a frequency channel, so the range path is taken), then runs
`FAST` and `FAST BOTH` for 3 s each, pumping the app loop, and prints
`probes / elapsed ms / probes/s` for both plus the `Both/Default` ratio.  It
restores the saved VFO, range and scan mode before returning, so the manual
`F+5` range arming and menu switching are not needed.

`J` also prints the `SetScn` mode and `ScnRev` so the menu wiring is visible; it
uses a 1 MHz span at the VFO's step, starting at the current frequency.  Run it
over a quiet range, repeat a few times, and treat `FAST BOTH` as useful only if
its measured rate is repeatably higher.  If it cannot beat `FAST`, the serialized
probe design should be revisited or `FAST BOTH` removed.

### Measured on the radio

`J`, 446.005..447.005 MHz, 10 kHz step, 3 s per mode, quiet band, two runs:

| run | FAST | FAST BOTH | ratio |
|---|---|---|---|
| 1 | 300 probes / 3083 ms = 97/s | 516 / 3070 ms = 168/s | 1.73x |
| 2 | 300 / 3083 = 97/s | 516 / 3070 = 168/s | 1.72x |

So `FAST BOTH` is about **1.7x**, not 2x: the BK4829's per-candidate tune/settle
is the expensive half (~10 ms each), and the BK4815 lane adds cheaper probes.  The
sweep was above the 134 MHz split, so both lanes were used.  Note that `FAST BOTH`
also probes one batch per lane per interval, so some of the gain is simply "more
candidates per scheduler batch"; below 134 MHz all candidates stay on the BK4829
and both lanes' batches still run, which should still be faster but is not yet
measured.

## Open / radio-validation gates

Everything below is **unvalidated**: it builds, the host tests pass, and the
Default scan behaviour is unchanged in the host preview, but none of the dual-RF
behaviour has run on the radio.

* Candidate throughput in `FAST BOTH` vs `FAST` (probes per second) for a range
  sweep; the scan-list path is currently the documented single-lane fallback
  and is not expected to speed up.
* Whether the shared band/path switch lets a lane's RSSI reading be meaningful
  after the other lane moved the path (cross-band interleaving).
* The BK4815 lane's squelch mark and margins (currently
  `RX4815_SQUELCH_OPEN_MARK` and the shared `SCAN_FAST_*` margins).
* A hit on each lane: full verification, correct audio source, pause/resume.
* PTT while scanning, and stop restoring the RX VFO/transceiver/audio.

The branch stays unmerged until these pass on the radio.
