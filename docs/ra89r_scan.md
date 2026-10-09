# The scan, and the dual-transceiver scan

The K1/F4HWN frequency-range and scan-list scanners are imported almost
unchanged (`app/scanner.c`, `app/chFrScanner.c`, `ui/scanner.c`).  What the port
adds on top is a **dual-transceiver** mode: a global menu item `ScTrMd` that lets
the scanner use both RF parts (BK4829 and BK4815) as interleaved lanes instead of
only the selected VFO's chip.

This file records what is implemented, what is a software fact, and what is still
only radio-validated work.  Nothing here is called *verified* until it has run on
the radio.

## The two modes

`ScTrMd` is persisted in the port's own settings blob (not the read-only stock
codeplug) as `scan_transceiver_mode_t` (`settings.h`):

| value | label | meaning |
|---|---|---|
| 0 | `Default` | one lane, on the chip the selected RX VFO is assigned to (`TrVfoA`/`TrVfoB`) |
| 1 | `Both` | the candidate stream is split across the BK4829 and the BK4815 |

The extra blob moved from version 2 to version 3 to carry the new byte.  A v2
blob still loads: its fields (the per-VFO RF choices, the frequency-channel
snapshot, the FM memories) are migrated and the new mode defaults to `Default`
(`settings.c` keeps the v2 layout explicit).  `preview_k1.c` checks the migration.

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

## What the "Both" path actually does (software, not yet radio-validated)

`ScanBothFastPrecheck()` in `app/chFrScanner.c` runs when `ScTrMd == Both` and
the fast scan is enabled:

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

**Scan-list scope:** only the frequency range is interleaved for now.  The list
keeps the K1's single-lane fast precheck so its priority/rotation behaviour stays
exactly as validated; the spec allows this as the conservative fallback.  This is
a deliberate limit, not an accident, and is the first thing to revisit if the
list is to be split too.

**`Default` and a BK4815-assigned VFO:** the K1 fast precheck is hardcoded to the
BK4829, so with `TrVfoA/B = 4815` the `Default` precheck probes the BK4829 while
the full verify path follows the BK4815 assignment.  Making the single-lane
precheck lane-aware is future work; `Both` is the mode that uses both chips
explicitly.

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

Console `J` (`App/main.c`) now also prints `ScTrMd`, the override state, and the
`scan_dual_stats_t` snapshot (per-lane candidate counts, last frequency, last
RSSI, selected hit) via `CHFRSCANNER_GetScanDualStats()`.  It reads state only;
it never tunes hardware to print.

## Open / radio-validation gates

Everything below is **unvalidated**: it builds, the host tests pass, and the
Default scan behaviour is unchanged in the host preview, but none of the dual-RF
behaviour has run on the radio.

* Candidate throughput in `Both` vs `Default` (candidates per second) for a range
  sweep and a same-band list.
* Whether the shared band/path switch lets a lane's RSSI reading be meaningful
  after the other lane moved the path (cross-band interleaving).
* The BK4815 lane's squelch mark and margins (currently
  `RX4815_SQUELCH_OPEN_MARK` and the shared `SCAN_FAST_*` margins).
* A hit on each lane: full verification, correct audio source, pause/resume.
* PTT while scanning, and stop restoring the RX VFO/transceiver/audio.

The branch stays unmerged until these pass on the radio.
