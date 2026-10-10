# Dual-Transceiver Scan Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a persisted `ScTrMd` setting that lets the imported K1 frequency-range and scan-list scanners use either the selected VFO's transceiver or both RF chips as interleaved scan lanes.

**Architecture:** Keep `CHFRSCANNER` as the owner of K1 scan order, pause/resume, list priority, and result/stop semantics. Factor its next-candidate traversal so `Default` probes with the selected RX VFO's configured chip and `Both` assigns consecutive candidate ordinals to independent BK4829/BK4815 fast-RSSI lane states; a verified hit is handed back to the existing K1 receive path through a temporary RX-chip override. The 10 ms SysTick scheduler remains the cadence source; the two RF buses are serialized, and batch size is measured/tuned on-radio.

**Tech Stack:** C11, PY32F403 register-level drivers, BK4829/BK4815 shared bit-banged RF bus, `firmware/tools/check_all.sh`, host preview and focused C tests.

**Spec:** `docs/superpowers/specs/2026-10-09-dual-transceiver-scan-design.md`

## Global Constraints

- The stock codeplug is read-only; `ScTrMd` lives in the port's settings-extra blob.
- Existing v2 settings-extra fields (RF A/B, frequency snapshot, FM memories) survive migration; an absent/invalid new scan mode resolves to `DEFAULT`.
- The BK4819-compatible driver remains bound to BK4829; BK4815 uses its own driver and framing.
- The 10 ms scheduler handler performs no flash I/O and no long blocking wait.
- Frequency-step rounding remains unchanged.
- Both chips use independent RSSI/noise-floor state; BK4815 `0x44` and BK4829 `0x67` values are never compared directly.
- Scan work stays on `scan` and is not merged to `develop` until the radio gates pass.

## Review Focus

1. **A v2 settings-extra blob:** preserve RF assignments, channel snapshot, and FM memories; default only the new mode. Test in Task 1.
2. **Scan-list priority entries:** preserve K1 priority/list ordering without duplicating or losing eligible channels when split. Test in Task 2.
3. **Range wrap/skip entries and reverse direction:** each logical candidate is visited once and assigned consistently. Test in Task 2.
4. **Different RSSI scales / weak hits:** each lane has independent thresholds and a fast hit is verified by the full receive path. Test in Task 3.
5. **Hit, PTT, and explicit stop while both lanes are active:** restore the correct VFO, transceiver, AF source, and scan state. Test in Task 4 and on-radio gates.

---

### Task 1: `ScTrMd` menu setting and settings-extra migration

**Files:**
- Modify: `firmware/App/settings.h`, `firmware/App/settings.c`
- Modify: `firmware/App/ui/menu.h`, `firmware/App/ui/menu.c`
- Modify: `firmware/App/app/menu.c`
- Test: `firmware/tools/preview_k1.c`

**Interfaces:**
- Produce `scan_transceiver_mode_t` with `SCAN_TRANSCEIVER_DEFAULT=0` and `SCAN_TRANSCEIVER_BOTH=1`, plus `SETTINGS_GetScanTransceiverMode()` / `SETTINGS_SetScanTransceiverMode(scan_transceiver_mode_t)`.
- Produce `MENU_SC_TR_MODE`, a `ScTrMd` menu row, and `gSubMenu_SCAN_TRANSCEIVER[] = {"Default", "Both"}`.
- Persist the setting in `settings_extra_t`; bump `EXTRA_VERSION` from 2 to 3. Decode v2 using an explicit v2 layout, preserving its existing fields and setting the new mode to Default. Continue accepting v3. Invalid values normalize to Default.

- [ ] **Step 1: Write the failing menu/API behavior test** in `preview_k1.c`. Find the menu row by the string `ScTrMd` (so the pre-feature test still compiles); assert it exists, has exactly `Default`/`Both`, accepts indices 0/1, and reopening the item reports the selected index.
- [ ] **Step 2: Run the preview and verify RED.** Run `firmware/tools/check_all.sh`; expected: a runtime assertion reports the missing `ScTrMd` row (not a compiler error).
- [ ] **Step 3: Implement the mode API and menu.** Add the enum/getter/setter, menu row/labels, explicit index mapping, and v3 `settings_extra_t` save/load for the new byte. Test save/load of `Both` through the menu.
- [ ] **Step 4: Write the failing v2 migration test.** In `preview_k1.c`, save a current fixture, construct the exact old v2 extra prefix (magic/version, RF A/B bytes, frequency snapshot, conditional FM memories), store it with `storage_set_extra` and `storage_save_settings`, then call `SETTINGS_InitEEPROM`. Assert scan mode defaults and all old extra fields survive the upgrade.
- [ ] **Step 5: Run the preview and verify RED.** Expected: old v2 extra is rejected or its saved fields are not migrated.
- [ ] **Step 6: Implement v2-extra migration.** Decode the v2 prefix explicitly; preserve old fields and set the newly introduced mode to Default. Keep the v3 loader and invalid-value normalization.
- [ ] **Step 7: Run the preview and full host checks.** Expected: both-mode round-trip and v2 migration pass.
- [ ] **Step 8: Commit** `feat(scan): add persistent Default/Both scan mode`.

### Task 2: Candidate stream and deterministic lane assignment

**Files:**
- Modify: `firmware/App/app/chFrScanner.c`, `firmware/App/app/chFrScanner.h`
- Create: `firmware/App/app/scan_dual.c`, `firmware/App/app/scan_dual.h`
- Modify: `firmware/CMakeLists.txt`
- Test: `firmware/tools/test_scan_dual.c`
- Modify: `firmware/tools/check_all.sh`

**Interfaces:**
- `scan_dual.h` defines `scan_candidate_t { uint32_t ordinal; uint32_t frequency_10hz; uint16_t channel; uint8_t band; bool is_memory_channel; }`, `scan_lane_chip_t { SCAN_LANE_BK4829=0, SCAN_LANE_BK4815=1, SCAN_LANE_NONE=2 }`, and `scan_dual_assign_candidate(const scan_candidate_t *)`. The BK4815 lane is eligible only above the stock's 134 MHz split (`frequency_10hz > 13400000`); for eligible candidates, even ordinals go to BK4829 and odd ordinals to BK4815. At/below the split, assign to BK4829 so no candidate is lost to an unvalidated BK4815 band. Also define `scan_lane_state_t { uint16_t noise_floor; uint32_t last_frequency_10hz; uint32_t candidates; uint16_t last_rssi; }`, `scan_dual_state_t { scan_lane_state_t lanes[2]; uint32_t next_ordinal; uint8_t mode; bool active; scan_lane_chip_t selected_hit; }`, `scan_dual_reset(scan_dual_state_t *)`, and `bool scan_dual_rssi_candidate(scan_lane_state_t *, uint16_t rssi, uint16_t squelch_open, uint16_t noise_margin, uint16_t squelch_margin, uint16_t weak_margin)`.
- In `chFrScanner.c`, factor the existing range/list cursor advance into a private `bool CHFRSCANNER_NextCandidate(scan_candidate_t *out)`. It must preserve `ScanRangeNextFrequency`, range exclusions, `RADIO_FindNextChannel`, scan-list enable, priority channel order, direction, and wrap; return false if a list/range has no valid candidate. `Default` uses one lane selected from the RX VFO's `TrVfoA/B` setting; Both uses the lane assignment helper.
- Candidate source is single-owner; it must not clone `currentScanList`, `gNextMrChannel`, or K1 global scan state per transceiver.

- [ ] **Step 1: Add failing pure tests** for even/odd eligible ordinals, BK4815-ineligible candidates at/below 134 MHz always going to BK4829, and independent per-lane RSSI floor learning.
- [ ] **Step 2: Run the focused test and verify RED.** Compile `test_scan_dual.c` with `App/app/scan_dual.c`; expected: missing interface/test assertions fail.
- [ ] **Step 3: Implement `scan_candidate_t`, lane assignment, and `scan_dual_rssi_candidate(scan_lane_state_t *, uint16_t rssi, uint16_t squelch_open, uint16_t noise_margin, uint16_t squelch_margin, uint16_t weak_margin)` in `firmware/App/app/scan_dual.c`.** Keep this policy module device-header-free and host-testable; use the `frequency_10hz > 13400000` BK4815 eligibility boundary.
- [ ] **Step 4: Extract the candidate traversal from `NextFreqChannel`/`NextMemChannel` into `CHFRSCANNER_NextCandidate(scan_candidate_t *out)`.** Do not change ordering or frequency-step rounding. Leave the existing Default single-lane call path intact.
- [ ] **Step 5: Extend `preview_k1.c` with an integration regression.** Feed a short frequency range and a mixed-band scan-list fixture through the actual `CHFRSCANNER` candidate source; assert each eligible item appears once and in K1 order, with lane selection matching its ordinal/support.
- [ ] **Step 6: Run focused and full host tests.** Expected: candidate order/parity tests pass, the preview range/list integration regression passes, and the existing single-scan movement regression remains green.
- [ ] **Step 7: Commit** `refactor(scan): expose the K1 candidate stream and dual-lane policy`.

### Task 3: Two RF fast-probe lanes and scan-hit arbitration

**Files:**
- Modify: `firmware/App/app/chFrScanner.c`, `firmware/App/app/chFrScanner.h`
- Modify: `firmware/App/driver/rx.c`, `firmware/App/driver/rx.h`
- Modify: `firmware/App/driver/pa.c` only if per-candidate path switching needs a scan-specific entry point
- Test: `firmware/tools/test_scan_dual.c`, `firmware/tools/preview_k1.c`

**Interfaces:**
- `scan_dual.h` defines `scan_dual_state_t { scan_lane_state_t lanes[2]; uint32_t next_ordinal; uint8_t mode; bool active; scan_lane_chip_t selected_hit; }`, `scan_dual_reset(scan_dual_state_t *)`, and `scan_dual_choose_hit(bool hit_4829, uint32_t ordinal_4829, bool hit_4815, uint32_t ordinal_4815, scan_lane_chip_t *selected)`. `chFrScanner.c` owns one state and performs RF operations; `scan_dual.c` owns only host-testable lane policy/state transitions.
- `rx.h`/`rx.c` define `rx_scan_source_t { RX_SCAN_SOURCE_DEFAULT, RX_SCAN_SOURCE_BK4829, RX_SCAN_SOURCE_BK4815 }` and provide `rx_set_scan_source_override(rx_scan_source_t)` / `rx_clear_scan_source_override()`. The override affects the hit's receive tune, RSSI, AF, and audio selection while paused; it never modifies saved `TrVfoA/B` values.
- For BK4829, reuse `ScanFastTune`/RSSI logic and `0xCF`/`0xB4`-compatible scale; for BK4815 use its own frequency/band programming and `0x44` scale with a lane-local floor/threshold. Keep the conversion/threshold policy explicit and covered by host tests.
- On candidate hit, compare original ordinal if both lanes hit in a batch; fully configure/verify the selected candidate, then invoke the existing K1 found/pause path with the scan RX override active.

- [ ] **Step 1: Add failing tests** for dual-lane sample routing, per-lane independent noise floors/scales, candidate verification, and earliest-ordinal selection when both lanes hit.
- [ ] **Step 2: Run focused tests and confirm RED.** Expected: the dual-lane state/decision interface is missing.
- [ ] **Step 3: Implement two lane probe states.** Tune/sample over the shared bus sequentially, switch shared band/path before each sample, and use each chip's own RSSI domain.
- [ ] **Step 4: Integrate range and memory candidate batches.** In Both, process six range candidates per lane or one list candidate per lane per scan resume interval; process in ordinal order and retain unprocessed candidates if a hit occurs.
- [ ] **Step 5: Integrate the temporary RX override.** Verify a hit on either chip reaches the existing full channel setup, squelch/code verification, AF path, and pause display; do not persist the override.
- [ ] **Step 6: Run host tests and `check_all.sh`.** Expected: Default behavior and all existing regressions remain unchanged; Both covers disjoint candidate ordinals.
- [ ] **Step 7: Commit** `feat(scan): run fast prechecks on both transceivers`.

### Task 4: Scheduler integration, lifecycle, diagnostics, and radio gate

**Files:**
- Modify: `firmware/App/driver/scheduler.c` only if measured dual-batch time requires cadence adjustment
- Modify: `firmware/App/app/chFrScanner.c`, `firmware/App/driver/rx.c`
- Modify: `firmware/App/main.c` (extend console `J` with scan-rate/lane diagnostics)
- Create: `docs/ra89r_scan.md`
- Modify: `docs/README.md`
- Test: `firmware/tools/preview_k1.c`, `firmware/tools/check_all.sh`

**Interfaces:**
- Keep the existing 10 ms scheduler as the tick source. Dual work is budgeted per scan resume interval; the interval must never include flash I/O or a long blocking wait.
- `scan_dual.h` defines `scan_dual_stats_t { bool active; uint8_t mode; uint32_t candidates[2]; uint32_t last_frequency_10hz[2]; uint16_t last_rssi[2]; scan_lane_chip_t selected_hit; }` and `scan_dual_get_stats(const scan_dual_state_t *, scan_dual_stats_t *out)`; console `J` prints it without tuning hardware.
- On stop/hit/PTT: restore the saved scan-start RX VFO/channel and normal VFO chip route exactly once; do not leave either chip in TX/scan AF state.

- [ ] **Step 1: Add a failing preview lifecycle test** for stopping Both scan and restoring RX VFO/transceiver/audio source; add a PTT-suspension/return assertion.
- [ ] **Step 2: Run the preview and verify RED.** Expected: scan override is absent or leaks after stop.
- [ ] **Step 3: Implement lifecycle cleanup and diagnostics.** Preserve `ScnRev` only as the found-signal pause; quiet scan batches continue on the fast cadence.
- [ ] **Step 4: Adjust the quiet-scan work budget.** Each 10 ms resume interval services both lanes; for `Both`, set the quiet-batch pause to one 10 ms tick after the batch. Keep the verified-hit `ScnRev` delay unchanged, and reduce per-lane batch size only if radio timing shows the combined work overruns the cooperative loop.
- [ ] **Step 5: Record behavior/evidence in `docs/ra89r_scan.md`.** Separate software facts from radio-validated facts; document cross-band path findings and any fallback.
- [ ] **Step 6: Run full verification.** Run `firmware/tools/check_all.sh`, `git diff --check`, and `python3 tools/ra89r.py verify firmware/build/Debug/ra89r_fw.icf`.
- [ ] **Step 7: Radio validation before merge.** Measure candidates/second in Default vs Both for range scan and a same-band list; test mixed-band list, hold known signals on each chip, test hit audio/pause/resume/stop, and key PTT while scanning. Do not merge to `develop` unless every radio acceptance gate passes.
- [ ] **Step 8: Commit** `docs(scan): document dual scan behavior and radio evidence` after recording measurements. Keep the branch unmerged until all gates pass.

## Radio validation matrix

| Mode | Input | Expected |
|---|---|---|
| Default | Frequency range, BK4829 RX VFO | Existing single-lane behavior; BK4815 unused |
| Default | Frequency range, BK4815 RX VFO | Single-lane scan tunes/meters BK4815 |
| Both | Same-band frequency range | Even/odd candidates partitioned; both lanes contribute; higher measured candidate rate |
| Both | Same-band scan list | K1 membership/priority/order preserved; neither channel skipped or duplicated |
| Both | Mixed-band scan list | Cross-band path switched per sample and both RSSI lanes validated; otherwise scan all entries serially in fallback |
| Both | Signal hit on each lane | Full verification, correct audio source, pause/resume from hit |
| Both | PTT / explicit stop | Correct TX, then RX route and scan state restored without stale scan AF/TX mode |
