# Bluetooth earpiece — Phase 1 (control plane) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Bluetooth service and a dedicated `DISPLAY_BT` screen (opened with F+MENU) that powers the YBT100 module, shows its state, and edits the basic BT settings — with the main menu untouched.

**Architecture:** A device-header-free service core (`App/app/bt.c`) owns the BT state machine and command queue and talks to the module through the already-validated `driver/bluetooth.c` transport. The application reads/writes `gEeprom` and drives the service. A new `App/ui/bt.c` renders the screen; `DISPLAY_BT` is added to the port's screen enum and key dispatch.

**Tech Stack:** C11, register-level PY32F403 firmware (no HAL/LL), the existing K1/F4HWN port and its host test harnesses.

**Spec:** `docs/ra89r_bluetooth_design.md`

## Global Constraints

- Module transport: USART3, PB10/PB11, **115200 8N1**; reset on **PD0** (low = held, high = released).
- Command strings/events are the stock's, already transcribed in `driver/bluetooth.h`; do not invent new ones.
- The 10 ms app slice must **not** touch the external SPI NOR; `bt_poll()` only drains USART3.
- Never write the stock codeplug area; BT settings live in the port's own blob (`gEeprom`).
- New `EEPROM_Config_t` fields require a `BLOB_VERSION` bump in `App/driver/py25q16.c`.
- This plan is **Phase 1 only**; pairing/paired-list (Phase 2), earpiece PTT (Phase 3) and audio (Phase 4) are separate plans.

## Review Focus

Inputs/conditions the spec implies but no single task test pins, most likely first:

1. **Module never sends `+IM_READY`** (absent/unpowered) — the service must stay bounded (no infinite retry storm) and the UI must show "not ready".
2. **Garbage or partial lines** — the parser/service must not advance the command queue on a non-event.
3. **BT enabled before settings load** — `bt_init()` must not read `gEeprom` before `SETTINGS_InitEEPROM()`.
4. **F+MENU must not also open the main menu** — the F path and the normal menu path must be mutually exclusive.
5. **`bt_poll()` in the 10 ms slice** — must not perform any NOR access (no `settings_save_all`, no `SETTINGS_Save*Flush`).

Each of these gets its test in the owning task below.

---

### Task 1: BT settings fields

**Files:**
- Modify: `firmware/App/settings.h` (`EEPROM_Config_t`, ~line 164-298)
- Modify: `firmware/App/settings.c` (`SettingsDefaults()`, ~line 130-191)
- Modify: `firmware/App/driver/py25q16.c` (`BLOB_VERSION`, ~line 32)
- Test: `firmware/tools/preview_k1.c` (existing `[storage]` check)

**Interfaces:**
- Produces: `gEeprom.BT_Switch` (bool), `gEeprom.BT_Mode` (uint8_t), `gEeprom.BT_Name[16]`, `gEeprom.BT_SpkGain` (uint8_t), `gEeprom.BT_MicGain` (uint8_t), `gEeprom.BT_PTTType` (uint8_t), `gEeprom.BT_HoldTime` (uint8_t).

- [ ] **Step 1: Add the failing assertion**

In `preview_k1.c`, next to the existing `[storage] save=1 load=1` line, add a check that after save+load `gEeprom.BT_Switch == false` and `gEeprom.BT_Mode == 0` and that setting `BT_Switch = true` survives a round-trip. (Use the same save/load calls already in that file.)

- [ ] **Step 2: Run the preview to verify it fails**

Run: `cd firmware && gcc -std=c11 -I tools/host -I App -I App/driver -DPY32F403xD -include App/k1_features.h -DST7565_HOST_TEST -ffunction-sections -fdata-sections -Wl,--gc-sections tools/preview_k1.c tools/host/host_hw.c tools/host/host_bk4819.c tools/host/host_beeper.c App/ui/main.c App/ui/menu.c App/ui/ui.c App/ui/status.c App/ui/welcome.c App/ui/battery.c App/ui/scanner.c App/ui/helper.c App/ui/inputbox.c App/ui/fmradio.c App/app/menu.c App/app/action.c App/app/app.c App/app/main.c App/app/generic.c App/app/common.c App/app/chFrScanner.c App/app/dtmf.c App/app/scanner.c App/app/fm.c App/radio.c App/functions.c App/audio.c App/misc.c App/driver/py25q16.c App/board.c App/settings.c App/version.c App/dcs.c App/frequencies.c App/helper/battery.c App/helper/boot.c App/driver/system.c App/font.c App/bitmaps.c App/driver/st7565.c App/driver/keyboard.c App/driver/backlight.c -o /tmp/preview_k1 && /tmp/preview_k1`
Expected: compile error `'EEPROM_Config_t' has no member named 'BT_Switch'`.

- [ ] **Step 3: Add the fields, defaults and version bump**

Add the fields from Interfaces to `EEPROM_Config_t`; in `SettingsDefaults()` set `BT_Switch=false, BT_Mode=0, BT_Name[0]='\0', BT_SpkGain=0, BT_MicGain=0, BT_PTTType=0, BT_HoldTime=0`; bump `BLOB_VERSION` by one.

- [ ] **Step 4: Run the preview to verify it passes**

Run the Step 2 command. Expected: the `[storage]` line reports the BT round-trip OK and the harness prints `0 failures`.

- [ ] **Step 5: Commit**

```bash
git add firmware/App/settings.h firmware/App/settings.c firmware/App/driver/py25q16.c firmware/tools/preview_k1.c
git commit -m "bt: add BT settings fields and blob version bump"
```

---

### Task 2: BT service core

**Files:**
- Create: `firmware/App/app/bt.c`, `firmware/App/app/bt.h`
- Test: `firmware/tools/test_bt_service.c`

**Interfaces:**
- Consumes: `driver/bluetooth.h` (`bt_cmd_t`, `bt_event_t`, `bluetooth_send_cmd`, `bluetooth_send`), `driver/bt_capture.h` (unchanged).
- Produces:
  - `typedef enum { BT_STATE_OFF, BT_STATE_RESET, BT_STATE_CONFIG, BT_STATE_IDLE, BT_STATE_SCAN, BT_STATE_CONNECT, BT_STATE_CONNECTED } bt_state_t;`
  - `void bt_init(void);` (target only)
  - `void bt_poll(void);` (target only: drains USART3 then `bt_service_tick()`)
  - `void bt_service_tick(void);` (pure)
  - `void bt_service_event(bt_event_t ev, const char *payload, unsigned len);` (pure)
  - `void bt_set_enabled(bool on);` / `bool bt_enabled(void);`
  - `void bt_set_mode(uint8_t mode);` / `void bt_set_name(const char *name);` (NULL = unset)
  - `bt_state_t bt_state(void);`
  - `const char *bt_version(void);` / `const char *bt_local_addr(void);`

- [ ] **Step 1: Write the failing test**

Create `firmware/tools/test_bt_service.c`. It compiles `App/app/bt.c` and `App/driver/bluetooth.c` with `-DBLUETOOTH_HOST_TEST` and provides a recording `bluetooth_hw_write`. Assertions:

```c
/* enabling pulses reset and enters RESET */
bt_set_enabled(true);
assert(bt_state() == BT_STATE_RESET);

/* +IM_READY queues the stock config sequence, in order */
bt_service_event(BT_EV_READY, NULL, 0);
assert(bt_state() == BT_STATE_CONFIG);
assert(recorded_count() == 6);
assert(recorded_is(0, "AT+GMR?\r\n"));
assert(recorded_is(1, "AT+BT=EMITTER\r\n"));      /* mode 0 */
assert(recorded_is(2, "AT+BLE_LOCAL?\r\n"));
assert(recorded_is(3, "AT+BT_SCANATCN=ON\r\n"));
assert(recorded_is(4, "AT+CONN_STATE?\r\n"));
assert(recorded_is(5, "AT+BT_CONN_LAST\r\n"));

/* with a name set, AT+WRITE_NAME is inserted before AT+BLE_LOCAL? */
bt_set_name("RA89R");
bt_service_event(BT_EV_READY, NULL, 0);
assert(recorded_is(2, "AT+WRITE_NAME=RA89R\r\n"));

/* +OK advances; a non-event does not */
assert(bt_state() == BT_STATE_CONFIG);
bt_service_event(BT_EV_NONE, NULL, 0);
assert(recorded_count() == 7);                    /* no new command */

/* the +IM_VERSION payload is stored */
bt_service_event(BT_EV_VERSION, "YBT100_FW_V01_02_012", 19);
assert(strcmp(bt_version(), "YBT100_FW_V01_02_012") == 0);

/* disable sends AT+BT_DISCN and returns to OFF */
bt_set_enabled(false);
assert(bt_state() == BT_STATE_OFF);
assert(recorded_last_is("AT+BT_DISCN\r\n"));
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd firmware && gcc -std=c11 -I App -I App/driver -DBLUETOOTH_HOST_TEST tools/test_bt_service.c App/app/bt.c App/driver/bluetooth.c -o /tmp/test_bt_service && /tmp/test_bt_service`
Expected: link error `undefined reference to 'bt_set_enabled'`.

- [ ] **Step 3: Implement the service**

`bt.h` declares the Interfaces above. `bt.c` holds the state, the mode/name, the command queue (an array of `bt_cmd_t` plus an index), `bt_version`/`bt_local_addr` buffers, and:

- `bt_set_enabled(true)` → state `BT_STATE_RESET`, queue nothing, call `bluetooth_power(true)` (target) / no-op (host).
- `bt_set_enabled(false)` → `bluetooth_send_cmd(BT_CMD_BT_DISCN)`, state `BT_STATE_OFF`.
- `bt_service_event(BT_EV_READY, …)` → queue `{GMR, mode==0?BT_CMD_BT_EMITTER:BT_CMD_BT_RECEIVER, [WRITE_NAME], BLE_LOCAL_Q, BT_SCANATCN_ON, CONN_STATE_Q, BT_CONN_LAST}`, state `BT_STATE_CONFIG`, send the first.
- `BT_EV_OK`/`BT_EV_ERROR` → advance per the stock rules: on `+OK` advance when one command remains or the current is `MICGAIN`/`SPKGAIN`/`BT_SCANATCN_ON`/`BT_CONN_LAST`; on `+ERROR` advance only for `BT_EMITTER`/`BT_RECEIVER`/`BLE_MASTER_ON`/`BLE_SLAVE_ON`. When the queue drains, state `BT_STATE_IDLE`.
- `BT_EV_VERSION`/`BT_EV_BT_LOCAL` → copy the payload into the stored buffer.
- `BT_EV_NONE` → no state change.
- `bt_service_tick()` → Phase 1: a bounded reset-retry counter (e.g. re-pulse at most N times while in `BT_STATE_RESET`, then go `BT_STATE_OFF`) so a silent module cannot spin forever. (Review Focus 1.)
- Hardware calls (`bluetooth_power`, `bluetooth_poll`, `bluetooth_init`) guarded by `#ifndef BT_HOST_TEST`.

- [ ] **Step 4: Run to verify it passes**

Run the Step 2 command. Expected: prints a pass line and exits 0.

- [ ] **Step 5: Add the review-focus tests**

Add to the same test: (a) `bt_set_enabled(true)` then many `bt_service_tick()` calls with no `+IM_READY` ends in `BT_STATE_OFF` (bounded retry); (b) `bt_service_event(BT_EV_NONE, …)` after the queue drains changes nothing (already partly covered — make it explicit after `BT_STATE_IDLE`).

- [ ] **Step 6: Run to verify it passes**

Run the Step 2 command. Expected: pass, 0 failures.

- [ ] **Step 7: Commit**

```bash
git add firmware/App/app/bt.c firmware/App/app/bt.h firmware/tools/test_bt_service.c
git commit -m "bt: service core -- state machine and command queue (host-tested)"
```

---

### Task 3: Target integration (boot init, 10 ms poll, console)

**Files:**
- Modify: `firmware/App/main.c` (boot bring-up ~line 636; 10 ms slice ~line 1816-1822; console switch ~line 1403-1763; `print_help()` ~line 226-249)
- Modify: `firmware/CMakeLists.txt` (`APP_SOURCES`, after `App/driver/bluetooth.c`)

**Interfaces:**
- Consumes: `bt_init`, `bt_poll`, `bt_set_enabled`, `bt_state`, `bt_version` from Task 2.
- Produces: console `A` prints BT state/version and toggles enable.

- [ ] **Step 1: Add `bt.c` to the build**

Add `App/app/bt.c` to `APP_SOURCES`.

- [ ] **Step 2: Boot init**

After `SETTINGS_InitEEPROM()` and the existing bring-up, call `bt_init()` once, then `bt_set_enabled(gEeprom.BT_Switch)`. (Review Focus 3: this runs after settings load.)

- [ ] **Step 3: Poll in the 10 ms slice**

In the 10 ms block (`App/main.c` ~1816-1822), add `bt_poll();` next to `tx_poll_ptt();`/`rx_service();`. It must not call any settings/NOR function. (Review Focus 5.)

- [ ] **Step 4: Console command `A`**

Add `case 'A':` that prints `bt_state()`, `bt_version()`, `gEeprom.BT_Switch`, and on a second press toggles `gEeprom.BT_Switch` + `bt_set_enabled()` + `gRequestSaveSettings`. Add a `print_help()` line.

- [ ] **Step 5: Build**

Run: `cd firmware && export ARM_TOOLCHAIN_ROOT=~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi && cmake --build build/Debug`
Expected: links, emits `ra89r_fw.icf`.

- [ ] **Step 6: On-radio check**

Flash and run console `A`: enabling prints `BT_STATE_RESET` then (module boot) `BT_STATE_CONFIG`/`BT_STATE_IDLE`, and `bt_version()` reads `YBT100_FW_V01_02_012`. Disabling sends `AT+BT_DISCN` and returns to `BT_STATE_OFF`. Record the output in `docs/ra89r_bluetooth.md`.

- [ ] **Step 7: Commit**

```bash
git add firmware/App/main.c firmware/CMakeLists.txt
git commit -m "bt: boot init, 10 ms poll and console A (validated on the radio)"
```

---

### Task 4: `DISPLAY_BT` screen and F+MENU

**Files:**
- Modify: `firmware/App/ui/ui.h` (`GUI_DisplayType_t`, after `DISPLAY_FM`)
- Create: `firmware/App/ui/bt.c`, `firmware/App/ui/bt.h`
- Modify: `firmware/App/app/app.c` (`ProcessKeysFunctions[]`, ~line 109-131)
- Modify: `firmware/App/app/main.c` (`MAIN_Key_MENU`, ~line 778-877; F dispatch)
- Modify: `firmware/CMakeLists.txt` (`APP_SOURCES`)
- Test: `firmware/tools/preview_k1.c`

**Interfaces:**
- Consumes: `bt_state`, `bt_enabled`, `bt_set_enabled`, `bt_set_mode`, `gEeprom.BT_*`.
- Produces: `void UI_DisplayBT(void);` and `void BT_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);`.

- [ ] **Step 1: Add the screen enum**

Add `DISPLAY_BT` to `GUI_DisplayType_t` after `DISPLAY_FM`.

- [ ] **Step 2: Write the failing preview check**

In `preview_k1.c`, add a section that sets `gScreenToDisplay = DISPLAY_BT`, calls `UI_DisplayBT()`, and asserts the rendered buffer contains `"BT Switch"` and `"BT Menu"`. Run the preview (Step 2 command from Task 1) and expect it to fail to link (`UI_DisplayBT` undefined).

- [ ] **Step 3: Implement `App/ui/bt.c`**

`UI_DisplayBT()` renders a title line and a scrollable list of the nine items (BT Switch, Pairing, Paired Dev, Hold Time, Scan, Spk Volume, Mic Gain, Blooth Inf, PTT Type), with the current value for toggles/selectors, using the port's font helpers as `ui/fmradio.c` does. `BT_ProcessKeys()` moves the cursor with UP/DOWN, toggles/enters with MENU, and leaves with EXIT (`gRequestDisplayScreen = DISPLAY_MAIN`). Only the items the service supports in Phase 1 act (BT Switch, Scan, Spk Volume, Mic Gain, PTT Type, Blooth Inf); the rest are shown but inert until Phase 2.

- [ ] **Step 4: Register key handling**

Add `BT_ProcessKeys` to `ProcessKeysFunctions[DISPLAY_BT]` in `App/app/app.c`.

- [ ] **Step 5: Open with F+MENU**

In `MAIN_Key_MENU`, before the normal open, add: if `gWasFKeyPressed` then `gRequestDisplayScreen = DISPLAY_BT`, `HideFKeyIcon()`, clear `gWasFKeyPressed`, and return — so F+MENU never opens the main menu. (Review Focus 4.)

- [ ] **Step 6: Run the preview to verify it passes**

Run the Task 1 preview command. Expected: the `DISPLAY_BT` assertion passes and the harness prints `0 failures`.

- [ ] **Step 7: Build and on-radio check**

Build (Task 3 Step 5 command). On the radio: F+MENU opens the BT screen; the main menu still opens with MENU alone; the list scrolls and toggles.

- [ ] **Step 8: Commit**

```bash
git add firmware/App/ui/ui.h firmware/App/ui/bt.c firmware/App/ui/bt.h firmware/App/app/app.c firmware/App/app/main.c firmware/CMakeLists.txt firmware/tools/preview_k1.c
git commit -m "bt: dedicated DISPLAY_BT screen opened with F+MENU"
```

---

### Task 5: BT status indicator

**Files:**
- Modify: `firmware/App/ui/status.c` (`UI_DisplayStatus`, ~line 80-345)
- Test: `firmware/tools/preview_k1.c`

**Interfaces:**
- Consumes: `bt_state()`, `bt_enabled()`.

- [ ] **Step 1: Write the failing preview check**

In `preview_k1.c`, force `bt_set_enabled(true)` (or a stub state), call `UI_DisplayStatus()`, and assert `gStatusLine` contains the `"BT"` glyph. Run the preview and expect failure.

- [ ] **Step 2: Implement**

In `UI_DisplayStatus`, reserve an `x` slot and draw `"BT"` with `UI_PrintStringSmallBufferNormal("BT", line + x)` (or a glyph) when `bt_enabled()`, and advance `x`. Set `gUpdateStatus = true` when the BT state changes.

- [ ] **Step 3: Run the preview to verify it passes**

Run the Task 1 preview command. Expected: `0 failures`.

- [ ] **Step 4: Build and on-radio check**

Build; on the radio the status line shows `BT` while the module is on.

- [ ] **Step 5: Commit**

```bash
git add firmware/App/ui/status.c firmware/tools/preview_k1.c
git commit -m "bt: status-line BT indicator"
```

---

## Self-Review

- **Spec coverage:** Phase 1 of the spec = service, settings, `DISPLAY_BT`, BT Switch + mode/scan/gains/PTT-type items, status indicator, console commands → Tasks 1-5. Pairing/paired-list, earpiece PTT and audio are explicitly deferred to later plans (spec phases 2-4).
- **Step scan:** each step is one action with a checkable result; code steps carry signatures/tests, not bodies.
- **Type consistency:** `bt_state_t`, `bt_service_tick`, `bt_service_event`, `bt_set_enabled`, `bt_set_mode`, `bt_set_name`, `bt_version`, `bt_local_addr`, `UI_DisplayBT`, `BT_ProcessKeys` are used with the same names throughout.
- **Review Focus:** items 1 and 4 have explicit tests (Task 2 Step 5, Task 4 Step 5); 2 is covered by the `BT_EV_NONE` assertion (Task 2 Step 1/5); 3 is a placement requirement (Task 3 Step 2); 5 is a placement requirement (Task 3 Step 3).
- **Proportion:** the plan is signatures + test assertions; no function bodies are transcribed except the queue sequence the spec fixes.
