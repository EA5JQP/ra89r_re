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
| 3 | `LISTEN` | the selected VFO keeps receiving on its own chip; the **other** chip sweeps the range |

`ScanFastEnabled()` is `!= NORMAL`; `ScanBothEnabled()` is `== FAST BOTH`;
`ScanListenEnabled()` is `== LISTEN`.

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

## What the "FAST BOTH" path actually does

`ScanBothFastPrecheck()` in `app/chFrScanner.c` runs when `SetScn = FAST BOTH`
and the fast scan is enabled:

1. pull up to `2 * SCAN_BOTH_PRECHECK_PAIRS` (24) candidates from the shared
   cursor, in pairs;
2. probe a pair of consecutive candidates on **different chips in the same
   band** as a pipeline: tune both, then read both.  The second tune's bus time
   (and, with a BK4815 in the pair, its fixed settle delay) covers the first
   chip's settling, so a pair costs less than two independent probes; the
   BK4815 is always tuned/read last.  Pairs in different bands fall back to one
   probe at a time, because the shared front end would move the path under the
   first chip before it was read (`ScanBothPairUsable`);
3. update each lane's floor with `scan_dual_rssi_candidate`;
4. on a hit, point `gRxVfo` at the winning candidate and set the RX scan-source
   override to the winning chip, then let the existing full single-candidate
   tune/verify path run.

The RF path is **shared**: the band/path is switched before a lane's sample
(`pa_select_band`, only on a band change), and the two parts share one
bit-banged bus, so the bus work is still serialized; what overlaps is the
settling.  The batch size is the throughput lever the measurement pointed at:
the K1 scanner runs one precheck batch per app pass, and the pass (display
included) paced the measured 61 ms per 12-candidate batch, so 24 candidates per
pass roughly doubles throughput until the probes themselves fill the pass.

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

## Listen while scanning (`LISTEN`)

`SetScn = LISTEN` keeps the selected VFO receiving on its own transceiver while
the other one sweeps the range.  Plan:
`docs/superpowers/plans/2026-10-19-listen-while-scanning.md`.  The constraints
come from the board, not from taste:

- the two parts are independent receivers on a shared bus (bus traffic is
  harmless to audio), but the **band path is shared** (PA0/PA1 and the
  BK4829's `0x33` chip path), so a probe in the other band would move the front
  end under the receiver -- `ScanRangeFastPrecheckListen()` therefore **skips
  candidates outside the listen lane's band**;
- the **AF node is shared**: the BK4815's audio reaches the amplifier while the
  BK4829's AF **DAC** (`0x30` bit 9) is on, and independently of the 4829's AF
  output select (`0x47`) -- measured in the duplex runs (2026-10-19).  The
  LISTEN precheck therefore does **not** mute the AF DAC (the FAST/FAST BOTH
  precheck does: that is what makes today's scan silent), and `rx_poll()` holds
  `0x47 = 0x6040` while a scan runs on the 4829 so the stepped chip's audio
  cannot ride the node;
- the walk still uses `gRxVfo` as its cursor (that is how the K1's range scan
  steps), so **`rx_service()` yields the coordinator while a scan is active**:
  no retune, no unassigned-4829 mirror, no band/path repair -- those read the
  chips and would fight the probes.  `CHFRSCANNER_Stop()` clears the override
  and the coordinator re-applies both lanes.

The scanner probes the scan lane only (`BK4819_SetFrequency` + the RSSI read for
a BK4829 lane, `bk4815_set_frequency` + `0x44` scaled x4 for a BK4815 lane), so
the listening lane's registers are never touched.  The listen VFO's **audio**
follows the poller's squelch as usual.

**Radio gates (unvalidated):** listen on the 4815 at 446.00625 while scanning
446.005-447.005 on the 4829 with a carrier on 446.00625 -- the downlink must
stay audible through the sweep; a hit pauses/opens as today; stopping restores
the VFO; PTT during a LISTEN scan must not break either lane; NORMAL/FAST/
FAST BOTH unchanged.

## The scanner's squelch trigger (fixed 2026-10-21)

The fast precheck's threshold used to be `gRxVfo->SquelchOpenRSSIThresh`, which
the K1 computes from six bytes per squelch level of **its own** EEPROM
calibration at `0x010060`.  This radio's external flash has nothing there (it
reads `0xFF`), so the trigger was **247** on `0x67`'s 9-bit scale -- above any
signal the part reports (a keyed carrier measured 236).  Both `FAST` and the
BK4829 lane of `FAST BOTH` were nearly deaf as a result, while still counting
every probe, so the throughput benchmark did not show it.

`ScanFastReceiveMark()` now returns the same level-derived mark the receive gate
uses (`rx_squelch_open_mark()` in `driver/rx.h`), for the scan lane and for the
BK4815 lane's squelch, so the scanner and the audio agree on what a signal is;
`ScanFastSquelchEnabled()` keeps the K1's "squelch off = no fast precheck" rule.
This is code-level and unvalidated on the radio: the gate is a hit on each lane
at the measured carrier levels (see the plan's gates).

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

Measured again after the 2026-10-21/22 changes, on the radio, both bands,
quiet range, SQL 4:

| band | FAST | FAST BOTH | ratio | in-batch | gap |
|---|---|---|---|---|---|
| 446.00625..447.00625 | 401/s | **671/s** | **1.67x** | 53.2 ms | 0.7 ms |
| 145.50000..146.50000 | 401/s | 671/s | **1.67x** | 53.2 ms | 0.7 ms |

The earlier VHF run measured 1.08x with a 53 ms/batch gap: the BK4815 lane's
own jitter read as a hit every batch and each one bought a full re-tune.  The
confirmation probe and the 12-count 4815 margin fixed that, and the gap is now
noise.  Note the ratio only means anything on a quiet range: with a carrier
inside the range both lanes hit it and the benchmark reads below 1.

That was the **12-candidate** FAST BOTH batch.  The batch is now 24 candidates
with the paired probe (`SCAN_BOTH_PRECHECK_PAIRS`), so the earlier figure is the
baseline to beat; the same `J` run has to show it.  Note that `FAST BOTH` also
probes more candidates per scheduler interval, which is where most of the gain
comes from, and that below 134 MHz all candidates stay on the BK4829 while both
lanes' batches still run.

## Open / radio-validation gates

Everything below is **unvalidated**: it builds, the host tests pass, and the
Default scan behaviour is unchanged in the host preview, but none of the dual-RF
behaviour has run on the radio.

* Candidate throughput in `FAST BOTH` vs `FAST` (probes per second) for a range
  sweep; the scan-list path is currently the documented single-lane fallback
  and is not expected to speed up.
* Whether the shared band/path switch lets a lane's RSSI reading be meaningful
  after the other lane moved the path (cross-band interleaving).
* The BK4815 lane's squelch margins (`SCAN_FAST4815_*`); its **opening mark**
  now follows `SQUELCH_LEVEL` (`rx_squelch_open_mark()` in `driver/rx.h`), with
  level 8 reproducing the measured pair.
* A hit on each lane: full verification, correct audio source, pause/resume.
* PTT while scanning, and stop restoring the RX VFO/transceiver/audio.

The branch stays unmerged until these pass on the radio.
