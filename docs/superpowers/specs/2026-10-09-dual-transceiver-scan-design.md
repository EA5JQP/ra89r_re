# RA89R dual-transceiver scan design

**Status:** Proposed design; awaiting user review. No dual-scan implementation is
authorized by this document yet.

## Goal

Use both RF transceivers to scan faster while preserving the existing K1 scan
semantics. Support both:

1. **Frequency/range scan:** consecutive stepped frequencies between configured
   range endpoints (or the current band's limits).
2. **Scan-list scan:** valid memory channels in the K1's configured list order;
   entries may lie in different bands.

Add a global menu item **`ScTrMd`** with:

- **Default:** scan through the one transceiver assigned to the selected RX VFO
  by `TrVfoA`/`TrVfoB`. This preserves single-lane scanning while making the
  scanner honor the selected VFO's chip.
- **Both:** temporarily use both the BK4829 and BK4815 as scan lanes, regardless
  of the per-VFO assignment. Restore the user's normal VFO routing on stop.

The speed target is approximately twice the candidate throughput on candidates
both parts can cover. The shared bus serializes register operations; no claim of
simultaneous sampling or exact 2x wall-clock speed is made until measured on the
radio.

## Current implementation and evidence

- `app/scanner.c`, `app/chFrScanner.c`, and `ui/scanner.c` are imported from the
  K1 tree. The fast-RSSI path is enabled on branch `scan`; the missing K1
  `SysTick_Handler` countdown body was ported to `driver/scheduler.c`. A host
  preview regression proves the scan advances when the scheduler tick runs.
- Frequency-range fast scan is `ScanRangeFastPrecheck()`: six consecutive steps
  per scheduler resume, sampled with the BK4829's RSSI, noise-floor estimate,
  and glitch indicator. A candidate is fully tuned and checked by the ordinary
  receive path.
- Memory-list fast scan is `MemChannelFastPrecheck()`: it probes one valid list
  candidate using the BK4829, then either skips it or lets the full K1 channel
  setup verify it. `NextMemChannel()` owns scan-list membership, priority
  channels, direction, and wraparound.
- The BK4829 reports 9-bit RSSI (`0x67`); the BK4815 reports 7-bit RSSI
  (`0x44`). They use distinct serial framing on a shared bit-banged bus.
- The RF front-end/band path is shared at board level. `pa_select_band()` changes
  MCU path pins and the BK4815 band register. Concurrent cross-band scanning is
  therefore a radio-validation question, not an assumption based on two PLLs
  being independently tunable.

## Design options

### A. Shared candidate stream, dual scan lanes (selected)

Factor the existing K1 range/list traversal into an ordered candidate source.
Each item has a monotonically increasing ordinal, frequency, band, and, for a
memory channel, channel identity/configuration. In `Both`, distribute eligible
candidate ordinals between two independent lane states:

- BK4829 lane: even ordinals.
- BK4815 lane: odd ordinals.

Each lane keeps its own frequency, band/path state, RSSI/noise-floor calibration,
and pending candidate. The shared bus serializes tuning and meter reads. Both
lanes are serviced in one scan scheduling interval. If either finds a
candidate, fully configure/verify it on that chip before entering the existing
scan-hit/pause path. If both report candidates in one batch, choose the lowest
ordinal to preserve deterministic scan order.

The frequency-range source preserves the current direction, start/stop, step,
wrap, and skip-exclusion behavior; each lane visits every other candidate. The
memory-list source preserves the K1's valid-list, priority-channel, direction,
and wrap behavior; it distributes eligible entries without dropping or
duplicating them. If a candidate is outside a chip's supported range, assign it
to the capable lane instead of silently skipping it.

### B. Clone the K1 scan state machine per chip (rejected)

Two copies of `CHFRSCANNER` would duplicate scan-list, pause, resume, display,
and channel-restore state. They could disagree about the current hit and
overwrite shared globals such as `gRxVfo`, `gScanStateDir`, and
`g_SquelchLost`.

### C. BK4829 hardware sweep plus BK4815 software scan (not the base design)

This may accelerate contiguous ranges, but does not naturally share the same
candidate ordering with memory lists and depends on BK4829 frequency-scan mode,
which is not yet validated on this board. It can be reconsidered after the
software dual-lane path works.

## Scan mode, persistence, and routing

- Add a radio-global scan mode `DEFAULT` / `BOTH`, exposed as `ScTrMd`. Store it
  in the port's settings-extra blob, not in the read-only stock codeplug. Bump
  the extra-schema version and migrate the existing v2 extra fields unchanged
  (per-VFO transceiver choices, frequency-channel snapshot, and FM memories);
  old or invalid scan mode defaults to `DEFAULT`.
- `Default` resolves the selected RX VFO's `TrVfoA/B` value once for the scan.
  `Both` temporarily takes ownership of both chips; it does not rewrite saved
  per-VFO assignments.
- Add a temporary scan-RX override so a verified hit found by BK4815 can use
  BK4815 RX/audio while paused even when the selected VFO is normally assigned
  to BK4829. Clear the override on explicit stop, PTT, and scan restart; normal
  routing is restored from the saved VFO setting.
- On a hit, restore that candidate's full channel configuration and let the
  existing K1 signaling/squelch and `ScnRev` pause/resume behavior decide when
  to resume. PTT remains on the existing BK4829-centered TX path; scan suspends
  during TX and returns to RX after PTT release.

## Cross-band behavior and safety

Both lanes may hold candidates in different bands. Before each lane's RSSI
sample, switch the shared RF path to that candidate's band/path, then measure
that chip. This is time-multiplexed cross-band scanning, not simultaneous
front-end routing. The radio gate must establish that the other lane's
measurement is not stale or blind after a path switch. If shared path switching
prevents reliable cross-band coverage, serialize the mixed-band candidate stream
through one active lane for that sweep. This loses the speed-up but still visits
every candidate; it must not claim both-band coverage while silently dropping
candidates.

The RSSI scales differ: per-lane noise floors and thresholds are mandatory; raw
`0x44` and `0x67` readings are not comparable. A fast-RSSI candidate is only a
precheck. Full channel setup and the existing receive verification remain the
authority for a hit.

## Scheduler and scan rate

- Keep the SysTick-derived 10 ms scheduler as the cadence source. No flash I/O or
  long blocking wait may be added to the 10 ms handler.
- In `Both`, each resume interval services both lane queues before arming the
  next countdown. Initially preserve the current range batch size (six
  candidates per lane) and process one memory-list candidate per lane per
  interval. Tune/settle/RSSI operations remain serialized; measure elapsed time
  and reduce batch size if the combined work starves the K1 loop or misses hits.
- `ScnRev` applies after a verified hit. Quiet batches use the fast cadence and
  must not become an RX hold or stop condition.
- Expose a scan-rate diagnostic (logical candidates completed per second and
  each lane's count) so the radio test can compare `Default` and `Both` rather
  than infer speed from a screen animation.

## Acceptance gates

1. **Default mode:** range and scan-list behavior remains the single selected
   VFO's assigned transceiver; stepping, stop-on-hit, `ScnRev`, and explicit
   stop still work.
2. **Both, one-band range:** both lanes visit disjoint candidate steps, with no
   missed/duplicated steps; measured throughput improves over Default; a signal
   found by either lane pauses on the right frequency and is audible.
3. **Both, scan list:** entries across multiple supported bands preserve K1 list
   membership, priority, direction, and wrap rules; each eligible entry is
   visited exactly once per pass; either lane can trigger a verified hit.
4. **Cross-band:** band/path is switched before each lane sample and both lane
   readings are meaningful; if not, the documented fallback is taken.
5. **Lifecycle:** scan stop restores the original RX VFO, per-VFO transceiver
   route, and audio; PTT suspends scan and RX resumes after PTT release.
6. Work remains on `scan` until every radio gate passes; only then may it be
   merged into `develop`.

## Non-goals

- Do not change frequency-step rounding; the user explicitly deferred that.
- Do not change `ScnRev` menu semantics except to ensure it only applies to a
  verified hit.
- Do not add the spectrum screen in this phase; it can use the validated scan
  engine later.
- Do not promise exactly 2x throughput or simultaneous cross-band reception
  before radio measurement.
