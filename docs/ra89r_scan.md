# The scan, and the dual-transceiver scan

The K1/F4HWN frequency-range and scan-list scanners are imported almost
unchanged (`app/scanner.c`, `app/chFrScanner.c`, `ui/scanner.c`).  `SetScn` is
**NORMAL** (the K1 step scan) or **FAST** (an RSSI precheck on the selected
VFO's own chip, then the full tune), on either transceiver.

> **2026-10-28: `FAST BOTH` and `LISTEN` were removed** (branch
> `driver/scan-route`).  Both ran a chip on the *other* chip's receive path:
> the two parts share one set of `0x33` receive-path pins (pin 0 the BK4829's,
> pin 4 the BK4815's, `docs/ra89r_rfpath.md`), and the chip they are not set
> for hears next to nothing -- radio 2026-10-28, VFO on the BK4815: the BK4829
> read a keyed PMR1 carrier as **68**, the noise level, where with its own pins
> it reads 220+.  FAST BOTH therefore missed every carrier that fell on the
> deaf lane and stopped on the other lane's noise; LISTEN's sweeping chip is
> always the deaf one.  Switching the pins per probe would cost a bus write per
> probe, forbid the paired probe (FAST BOTH's only gain) and take the antenna
> path away from the LISTEN listener -- i.e. turn it into time-sharing, which a
> single-chip scan already is.  A saved FAST BOTH / LISTEN loads as FAST.  The
> sections below that describe them (the candidate stream, the lane rule, the
> RX scan-source override, the LISTEN runs) are kept as the record of what was
> built and measured; the code (`app/scan_dual.*`, `tools/test_scan_dual.c`,
> `rx_set_scan_source_override()`, `rx_squelch_quiet()`) is gone, and the
> throughput counter it carried lives on as `app/scan_rate.*`.

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
- the **AF node is shared**: the BK4815's audio reaches the amplifier through
  the BK4829's AF **DAC** (`0x30` bit 9) *and* its AF output select (`0x47`).
  The 2026-10-19 duplex note said "independently of `0x47`"; **that is wrong**
  -- radio 2026-10-27, lane 4815 squelch open: `0x47 = 0x6040` gave no audio at
  all, `0x6142` a clean voice.  The LISTEN precheck does **not** mute the AF
  DAC (the FAST/FAST BOTH precheck does), and the BK4815 lane's `0x47` follows
  its squelch, scan or not (the old scan-time `0x6040` made every BK4815 hit
  silent); the stepping chip cannot ride the node because the sweep runs only
  while the listener's squelch is closed;
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

## LISTEN, validated on the radio (2026-10-22)

Automated with CAT `STATUS`/`SM` plus the console's `o`/`N`/`E` keys, an
FM-modulated carrier on the listen channel:

| check | result |
|---|---|
| audio during the scan | **tone, continuous** (heard on the radio) |
| sweep while the listen squelch is open | **0 probes in 9.5 s** — the sweep yields |
| sweep after the carrier goes | 1716 probes / 9.5 s = **180 probes/s**; 436/s over a shorter run |
| listen VFO during the scan | unchanged (the sweep cursor is internal) |
| after stop | back on the channel, squelch state intact |

Three defects had to be fixed to get there, all found by that run:

1. **The walk moved the listen VFO.**  `ScanRangeFastPrecheckListen()` used
   `gRxVfo` as its cursor and never restored it; after one scan the receiver sat
   75 kHz off its channel (carrier lost).  The cursor now lives in
   `scanListenCursor` and the listen record is restored after every quiet batch;
   a *hit* is left in place for the full tune.
2. **A false hit parked the scan.**  One noisy sample on the scan lane moved the
   receiver onto a non-signal and the sweep never resumed ("446.29375, no tone,
   no resume").  LISTEN now re-probes a candidate once before accepting it
   (`ScanListenProbe()`), like the FAST BOTH path already did.
3. **The amplifier was left off.**  `RADIO_SetupRegisters()` begins with
   `AUDIO_AudioPathOff()` (PC13) and runs on every scan start/reconfiguration,
   while `rx_poll()` re-enabled the audio only on a squelch *transition*: with a
   carrier already open, the receiver stayed silent for the whole scan.  Both
   `0x47` and the audio path are now driven every poll on both lanes.

And the sweep has to yield, not just coexist: with a carrier present, the
stepping chip's LO perturbed the listener's `0x67` between 269 and 86 through
the shared front end, which is audible as chopping.  Pausing while the listen
squelch is open is therefore the correct behaviour, not a workaround.

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

## The scan's receive route and the deaf BK4829 probe (2026-10-27, branch `driver/scan-route`)

Symptom (radio, 2026-10-25): the memory scan does not lock a keyed PMR446
carrier in the FAST modes; integration of the two transceivers during a scan is
inconsistent.  Read out of the code, four defects:

1. **A BK4815 VFO was never retuned during a scan.**  `rx_service()` yields the
   coordinator while `gScanStateDir != SCAN_OFF`, and `RADIO_SetupRegisters()`
   skips the BK4829 for a BK4815 VFO, so a NORMAL/FAST full tune reached
   neither chip: the BK4815 kept its pre-scan channel and its squelch decided
   on that.
2. **The single-lane FAST precheck always probed the BK4829**, also for a
   BK4815 VFO -- with the lane's receive-path pins set for the BK4815 (~16 dB
   down on the BK4829 at VHF, `docs/ra89r_rfpath.md`) and against the
   *active* lane's mark, which is then on the BK4815's 7-bit scale.
3. **The precheck's mark used the receiver's band, not the probe's**, and an
   unlearned floor slot fell back to `0xCF` (207) -- above a -30 dBm VHF
   carrier (187).  A scan into a band or modulation the VFO had not sat on
   (VHF from a UHF VFO, the AM airband from an FM one) was deaf.
4. **A resumed scan dropped the hit's route at the moment the reception
   started** (`CHFRSCANNER_ContinueScanning()` cleared the override before
   `APP_StartListening()`), and a LISTEN hit moved the *listener* onto the hit.

The shape now:

* the scanner **commits** every full tune: `rx_scan_commit(f)` after
  `RADIO_SetupRegisters()` in `NextFreqChannel()`/`NextMemChannel()` tunes the
  route's chip (BK4815: tune + `0x75` + receive entry; BK4829: tune + band),
  selects it as the audio/squelch lane and starts the gate closed (no settle
  hold-off: NORMAL locked inside the 100 ms step dwell without one);
* the route is the override if one is set (a FAST BOTH or LISTEN hit names its
  chip), else the selected VFO's transceiver -- `rx_scan_route_is_4815()`,
  host-tested in `test_squelch`; `RADIO_SetupRegisters()` retunes the BK4829
  only when `rx_bk4829_is_receiver()`;
* the single-lane FAST precheck (range and memory) probes that same chip, with
  a two-read confirmation on the BK4815 like LISTEN/FAST BOTH;
* the scanner's marks come from `rx_squelch_open_mark_for(lane, f)`: the probed
  lane/band/modulation's learned floor, or its measured prior;
* a LISTEN hit is received on the scan lane's chip (override), the listen
  record is restored by the next batch, and the override is cleared only when
  the scan moves on.

### The root cause on the radio: the BK4829 probe read before the receiver ran

The four defects above were real, but with the VFO on the BK4829 the memory
scan still raced through the list without locking.  A per-probe console trace
(memory list: PMR1-8 + 11 airband channels, another radio keyed on PMR1, VFO on
the BK4829, `FAST`) showed the cause:

| probe read | PMR noise | airband noise | keyed PMR1 | result |
|---|---|---|---|---|
| at once (the K1's code) | 14-15 | 14 | **14-15** | never a hit |
| first read over 28 (~1.1-1.6 ms) | 31-39 | 29-33 | 42-43 | never a hit |
| plateau (no new max for 8 reads) | ~64 | 81-88 | **222-227** | **locks** |

`ScanFastTune()` restarts the receiver (`REG_30 = 0`, then the old value) and
the meter restarts low and climbs while the AGC recovers.  The K1's settle
guard waits only while the glitch indicator is high -- and it is low right
after the restart -- so the K1 code read the reset value: **the BK4829's fast
probe was deaf on this board from the start** (`FAST`, and the BK4829 lane of
`FAST BOTH`).  The earlier FAST BOTH hit in this doc came from the BK4815
lane; the benchmark only counted probes, so it never showed this.

`ScanFastReadRssi()` now reads (~135 us per read on this bus) until the meter
holds still -- `SCAN_FAST_PLATEAU_READS` (8) reads in a row within
`SCAN_FAST_PLATEAU_TOL` (6) of a reference reading, which restarts whenever a
read leaves that band, so a climb after a restart and a fall from the previous
channel's level are both waited out -- capped at 20 ms, and returns the **mean
of the plateau reads**.  Returning the peak instead (the first fix) put plain
noise at 90-110 UHF / 80-90 VHF, above the receive gate's single-read marks,
and every pass started with false hits.

**The probe no longer restarts the receiver.**  `ScanFastTune()` now only
writes `0x38/0x39` (what the LISTEN probe always did) and applies the AF DAC
mute once per batch; the AGC keeps running, so there is no climb from zero to
wait out.  Console `#` switches back to the K1's `REG_30` restart for an A/B.
`J`, 446.00625-447.00625, 12.5 kHz, quiet, the same radio:

| BK4829 probe | FAST | FAST BOTH | locks on PMR1 |
|---|---|---|---|
| K1: restart, read at once (deaf) | 401/s | 671/s | no |
| restart + wait for no new maximum, peak (`4c4f6cc`) | 49/s | 373/s | yes |
| restart + plateau rule | 154/s | 303/s | yes |
| **plain write + plateau rule (default)** | **394/s** | **355/s** | **yes** |

**FAST BOTH is no longer faster than FAST (0.90x).**  Its 1.67x came from a
BK4829 lane that cost almost nothing because it never waited for a reading;
with a real reading, the split buys no speed (profile: ~69 ms of reads per
48-probe batch).  There is also **no evidence either chip is the better
receiver**: the stock receives on the BK4815 above 134 MHz, which is a design
choice read out of its code, and the BK4815's sensitivity is unmeasured
(`ra89r_bk4815.md`: the tinySA's -38 dBm minimum already saturates the
comparison), while the two RSSI scales (9-bit `0x67`, 7-bit `0x44`) cannot be
compared directly.  What would decide FAST BOTH's future is a step-attenuated
carrier into each chip with its own lane pins set, stepped down until each
one's squelch drops out.

**Validated on the radio (2026-10-27), VFO on the BK4829:** the PMR memory
scan locks on keyed PMR1 in `FAST` and in `FAST BOTH`, with the restart and
with the plain write (a memory list runs the
single-lane precheck in both, see "Scan-list scope"): `HIT`, full tune, the
squelch opens, the reception is heard; the squelch closes when the other radio
unkeys.  `NORMAL` was unaffected.

Seen in the same runs, still open:

* **Strong-carrier spill-over:** with the keyed radio close by, PMR ch4-8 read
  100-144 and were taken as hits; the chip squelch even opened briefly on
  ch8.  Each costs a 200 ms full tune; the scan still reaches PMR1 and locks.
* **Settle cost with the restart:** after a probe-to-probe restart VHF
  channels start at 0 and take 10-14 ms to plateau (UHF ~1.2 ms).  The plain
  write (above) removes that ramp.

**VFO on the BK4815, validated on the radio (2026-10-27):** the PMR memory
scan locks on keyed PMR1 in `NORMAL`, `FAST` and `FAST BOTH` with `H` showing
`lane 4815`, and the voice is heard -- after one more fix: `rx_apply_audio_gate()`
held the BK4829's `0x47` at `0x6040` on a BK4815 lane whenever a scan was
active (a paused hit included), which silences the BK4815.  The gate now
follows the squelch as in plain receive (`0x6142` open), where the voice is
clean with the BK4829 left running.  Tried and not needed: powering the BK4829
down under a BK4815 reception, as the stock does (`FUN_08021844` ->
`FUN_080187E8`: `0x30 = 0`, `0x37 = 0x9D10`; with VOX, `FUN_0801754A`).

**Range FAST, radio 2026-10-28** (446.00625-447.00625, 12.5 kHz, PMR1 keyed,
per-probe console trace `%`): with the VFO on the BK4815 it locks and is fast.
With the VFO on the BK4829 it locked only by luck: PMR1 is the first probe
after the 1 MHz wrap, and the plain write read it as **84** (noise) -- the
restart it skips also runs the BK4829's VCO calibration.  A probe that jumps
more than 100 kHz now takes the restart (`SCAN_FAST_PLAIN_MAX_JUMP`); steps keep
the plain write.  Still to be confirmed on the radio.

**Radio gates still open:** a FAST *range* hit on the BK4829 every pass (the
>100 kHz restart); a VHF range from a UHF VFO; LISTEN keeps
the listener audible after a hit and resume.  Until they pass, the branch
stays off `develop`.

## Diagnostics

**Current (2026-10-28):** console `J` prints the `SetScn` mode and the scan's
probes / elapsed / probes-per-second (`CHFRSCANNER_GetScanRate()`), then runs a
3 s FAST benchmark over 1 MHz from the VFO's frequency.  Two bench toggles, off
by default:

* `%` -- the per-probe scan trace on the UART: `rng`/`mem` probe lines
  (frequency, chip, RSSI, floor, mark, verdict), `TUNE` on a full tune, `SQL`
  edges and `CONT` on a resume.  Blocking output, so it slows the sweep.
* `#` -- the BK4829 probe's retune: the plain `0x38/0x39` write (default) or the
  K1's `REG_30` restart, for an A/B with `J`.

The rest of this section describes the removed FAST BOTH diagnostics.

Console `J` (`App/main.c`) used to also print the `SetScn` mode, the override state,
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
