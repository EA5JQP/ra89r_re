# RA89R dual-transceiver scan design

**Status:** Proposed design; awaiting user review. No dual-scan implementation is
authorized by this document yet.

## Goal

Use both RF transceivers to scan faster while keeping the current K1 scan
semantics. Support both:

1. **Frequency/range scan:** consecutive stepped frequencies between the
   configured range endpoints (or the current band's limits).
2. **Scan-list scan:** valid memory channels in the K1's configured list order;
   entries may lie in different bands.

Add a global menu item **`ScTrMd`** with:

- **Default:** scan with the transceiver assigned to the selected RX VFO by
  `TrVfoA`/`TrVfoB`. This preserves the existing single-receiver behavior.
- **Both:** scan with both the BK4829 and BK4815, regardless of the per-VFO
  assignment. Restore the normal per-VFO route on scan stop or when a hit is
  handed to the ordinary receive path.

The speed target is approximately twice the candidate throughput on channels
the two devices can scan. Bus transactions remain serialized; no claim of
simultaneous samples or an exact 2x wall-clock speed is made until measured on
the radio.

## Current implementation and evidence

- `app/scanner.c`, `app/chFrScanner.c`, and `ui/scanner.c` are imported from the
  K1 tree. The fast-RSSI code is enabled on branch `scan`; the missing K1
  `SysTick_Handler` countdown body was ported to `driver/scheduler.c`. A host
  preview regression proves the scan frequency advances when the scheduler tick
  runs and stays put without it.
- Frequency-range fast scan is in `ScanRangeFastPrecheck()`: it probes six
  stepped frequencies per scheduler resume using the BK4829's RSSI, noise-floor
  estimate, and glitch indicator. A candidate is fully tuned and checked by the
  ordinary receive path.
- Memory-list fast scan is in `MemChannelFastPrecheck()`: it probes one valid
  list candidate using the BK4829, then either skips it or lets the full K1
  channel setup verify it. `NextMemChannel()` owns list/priority ordering and
  wraparound.
- The BK4829 reports a 9-bit RSSI (`0x67`); the BK4815 reports 7-bit RSSI
  (`0x44`). They use distinct serial framing on a shared bit-banged bus.
- The RF front-end/band path is shared at board level. In particular,
  `pa_select_band()` changes MCU path pins and the BK4815 band register. The
  stock normally chooses one transceiver per channel; concurrent cross-band
  scanning is therefore a hardware question, not something this design assumes
  to work merely because both PLLs can be tuned.

## Design options

### A. Shared candidate stream, dual scan lanes (recommended)

Factor the existing K1 range/list traversal into a candidate source that yields
an ordered item containing its ordinal, frequency, band, and (for a memory
channel) channel identity/configuration. In `Both` mode, distribute alternating
candidate ordinals between two independent lane states:

- BK4829 lane: candidate ordinals 0, 2, 4, …
- BK4815 lane: candidate ordinals 1, 3, 5, …

Each lane keeps its own tuned frequency, band/path state, RSSI/noise-floor
calibration, and pending candidate. The shared RF bus serializes tune and meter
access. The scheduler processes work from both lanes in one scan resume period,
then uses the normal scheduler countdown before the next batch. A candidate
found by either lane is fully configured/verified on that chip before the
existing scan-hit/pause behavior is entered. If both lanes report candidates in
one batch, choose the lowest original scan ordinal so the user-visible order is
deterministic.

For a frequency range, each lane probes every other step. For a memory scan,
the common candidate source preserves the K1's scan-list membership, priority
channel, direction, and wrap behavior; the lane assignment must not drop or
duplicate eligible entries. If one chip is outside the documented/validated
range for a candidate, send that candidate to the capable lane rather than
silently skipping it.

### B. Clone the K1 scan state machine per chip

Run two copies of `CHFRSCANNER` with separate globals and route each to one chip.
This superficially minimizes edits to its loops, but duplicates scan-list,
pause, resume, display, and channel-restore state. The two copies can disagree
about which hit is current and can overwrite `gRxVfo`, `gScanStateDir`, and
`g_SquelchLost`. Rejected: too much shared mutable K1 state to clone safely.

### C. Use the BK4829 hardware frequency-scan mode plus a BK4815 software scan

This could be fast for contiguous ranges, but does not provide one consistent
candidate/order model for memory lists and depends on the BK4829 frequency-scan
mode, which has not been validated on this board. Rejected as the primary
architecture; it may be considered later if radio measurements show a useful
hardware-scan primitive.

## Scan mode, persistence, and routing

- Add a global scan-transceiver mode with values `DEFAULT` and `BOTH`; `DEFAULT`
  is the safe fallback for erased, missing, or invalid settings.
- Persist it in the port's settings-extra blob, not the stock codeplug. Append
  the new field and bump the extra-schema version. Loading the existing v2
  settings-extra must preserve its transceiver choices, frequency-channel
  snapshot, and FM memories while supplying `DEFAULT` for the new field. A
  corrupt or absent mode also resolves to `DEFAULT`.
- `Default` resolves the selected RX VFO's `TrVfoA/B` assignment for the entire
  scan. `Both` temporarily owns both chips and their scan path; it does not
  rewrite the user's per-VFO transceiver settings.
- On a verified hit, restore the candidate's full channel configuration and
  select the chip that detected it as the receive-audio source during the
  existing pause. Resume continues after that candidate according to
  `ScnRev`; explicit stop restores the original RX VFO/channel and its normal
  transceiver route.
- PTT always remains on the existing BK4829-centered transmit path. Dual scan
  suspends while transmitting and resumes/restores RX after PTT release.

## Cross-band behavior and safety

Both scan lanes may be assigned candidates in different bands. Before each
lane's RSSI sample, the coordinator switches the shared RF path to that
candidate's band/path, then performs that lane's measurement. This is an
attempted time-multiplexed cross-band scan, not simultaneous front-end routing.
The test gate must establish that switching the shared path does not make the
other lane's samples stale or blind. If the path cannot support interleaved
cross-band sampling, the scan must report/choose a conservative fallback (one
active lane for that sweep); it must not claim both-band coverage while
silently dropping candidates.

The BK4815 and BK4829 RSSI scales differ. Each lane must have independent
threshold/noise-floor state; comparing raw `0x44` and `0x67` values directly is
invalid. Candidate verification always uses the existing selected receive
channel's squelch/code rules, not just the fast RSSI precheck.

## Scheduler and scan speed

- Keep the SysTick-derived 10 ms scheduler as the cadence source; no flash I/O or
  long blocking waits are added to the 10 ms handler.
- In `Both`, one resume interval must service both lane queues before arming the
  next countdown. For frequency ranges, start from the current fast path's six
  precheck samples per resume interval per lane; for memory lists, service one
  candidate per lane per interval. Tune/settle/RSSI operations are serialized
  on the shared bus, so measure real elapsed time and adjust the per-lane batch
  size if the combined batch starves the K1 loop or misses signals.
- `ScnRev` applies only after a verified hit. A quiet batch uses the fast
  scheduler cadence; it must not be treated as an RX hold or stop condition.
- Host tests verify ordering, parity assignment, band changes, wraps, unsupported
  candidates, and hit arbitration. Radio validation measures candidates/second
  in `Default` and `Both`, checks range and scan-list modes, holds known signals
  on each lane, and verifies hit audio/pause/resume/stop.

## Acceptance gates

1. **Frequency range, one band:** Both lanes advance disjoint steps without
   missing or double-visiting the range; the scan is faster than `Default` on
   the radio and pauses on a signal found by either chip.
2. **Scan list, same band:** both lanes cover the configured list in K1 order,
   honoring list filters, priority channels, direction, and wraparound; both
   can stop on a hit.
3. **Scan list, mixed bands:** cross-band path switching yields valid readings
   from both lanes; otherwise the documented fallback is used and no candidate
   is silently omitted.
4. **Lifecycle:** scan stop restores the selected VFO, its configured chip, and
   audio; PTT suspends scan and RX resumes correctly after PTT release.
5. The implementation remains on `scan` until the radio gates pass; only then
   may it be merged into `develop`.

## Non-goals

- Do not change frequency-step rounding; that is explicitly deferred.
- Do not change the existing `ScnRev` menu semantics beyond ensuring it applies
  only to a found signal.
- Do not add the spectrum screen in this phase; it can consume the validated
  scan/RSSI engine later.
- Do not promise exactly 2x speed or simultaneous cross-band reception before
  measuring the real radio.
