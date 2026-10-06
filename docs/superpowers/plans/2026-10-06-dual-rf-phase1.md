# Dual-RF Phase 1: per-VFO transceiver selection + dual-active

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let VFO A and VFO B each select their RF transceiver, and run both transceivers at once, each configured for its own VFO.

**Architecture:** Keep the K1 `BK4819_*`/BK4829 layer as the *primary* transceiver (RX+TX, the one the K1 app drives). Add a narrow BK4815 receive service for the *secondary* VFO and an `rf_dual` coordinator that resolves each VFO's selected transceiver and keeps both configured. A per-VFO setting, stored in the port's own versioned settings blob, records the choice.

**Tech Stack:** C11, PY32F403 register-level firmware, host C tests under `firmware/tools/`, `firmware/tools/check_all.sh`, Ghidra stock V49 as the register reference.

**Spec:** `docs/superpowers/specs/2026-10-06-dual-rf-transceiver-design.md`

## Global Constraints

- The stock codeplug is read-only. The per-VFO transceiver choice lives in the port's own settings blob (`settings_extra_t`), never in the codeplug.
- The `BK4819_*` layer stays bound to the BK4829; do not add a second instance of it in this phase.
- TX remains on the primary (BK4829). A VFO set to `BK4815` is receive-capable only; the transmit VFO must resolve to `BK4829`.
- The BK4815's frequency tuning is transcribed from the stock (`FUN_0801703c`): `word = (freq_10hz / 100000.0) * r4 * 645277.53846154`, `r4 = 24` for `freq_10hz <= 18,700,000`, `16` for `<= 27,000,000`, `12` for `<= 38,300,000`, else `8`; register 4 gets `(band << 7) | 0xB041`. The exact register placement is verified on the radio, not assumed.
- Nothing in the K1 10 ms slice may touch the external SPI NOR flash.
- Unvalidated work stays on `driver/dual-rf`; do not merge to `develop` before the radio gate passes.

## Review Focus

- **Both VFOs select the same transceiver:** a single BK4829 cannot receive two frequencies at once; the coordinator must fall back to single-VFO rather than mis-tune the shared chip. Tested in Task 4.
- **Erased/invalid saved value:** an absent or out-of-range `rf_xcvr` byte must resolve to `AUTO`, not to a random chip. Tested in Task 1.
- **Transmit VFO set to BK4815:** must be refused or resolved to the primary, never silently transmit on an unproven path. Tested in Task 4.
- **BK4815 tune at band edges / non-ham frequencies:** the r4 selection and the register-4 band byte must not corrupt the band register `0x75`. Tested on the radio in Task 5.
- **Host preview regression:** the new menu item must not shift or hide existing items. Tested in Task 2.

---

### Task 1: Per-VFO transceiver setting and persistence

**Files:**
- Modify: `firmware/App/settings.h`, `firmware/App/settings.c`
- Test: `firmware/tools/preview_k1.c` (the storage round-trip section)

**Interfaces:**
- Produce: `typedef enum { RF_XCVR_AUTO=0, RF_XCVR_BK4829=1, RF_XCVR_BK4815=2 } rf_xcvr_t;` in `settings.h`.
- Produce: `rf_xcvr_t SETTINGS_GetVfoTransceiver(uint8_t vfo);` and `void SETTINGS_SetVfoTransceiver(uint8_t vfo, rf_xcvr_t xcvr);` (`vfo` 0=A, 1=B).
- Consumes: the existing `settings_extra_t` save/load path (`storage_set_extra` / `storage_get_extra`, `EXTRA_MAGIC`).

- [ ] **Step 1: Write the failing test** in the storage section of `preview_k1.c`, after the existing BT-field round-trip:

```c
/* Per-VFO transceiver choice survives a save/load round trip. */
SETTINGS_SetVfoTransceiver(0u, RF_XCVR_BK4829);
SETTINGS_SetVfoTransceiver(1u, RF_XCVR_BK4815);
ok = storage_save_settings();
SETTINGS_SetVfoTransceiver(0u, RF_XCVR_AUTO);
SETTINGS_SetVfoTransceiver(1u, RF_XCVR_AUTO);
ok = ok && storage_load_settings();
ok = ok && SETTINGS_GetVfoTransceiver(0u) == RF_XCVR_BK4829 &&
     SETTINGS_GetVfoTransceiver(1u) == RF_XCVR_BK4815;
printf("[dual] %s per-VFO transceiver survives save/load (A=%u B=%u)\n",
       ok ? "ok  " : "FAIL", (unsigned)SETTINGS_GetVfoTransceiver(0u),
       (unsigned)SETTINGS_GetVfoTransceiver(1u));
if (!ok) failures++;
```

- [ ] **Step 2: Run and confirm RED.**

Run: `cd firmware && gcc -std=c11 -I tools/host -I App -I App/driver -DPY32F403xD -include App/k1_features.h -DST7565_HOST_TEST -DBLUETOOTH_HOST_TEST -ffunction-sections -fdata-sections -Wl,--gc-sections tools/preview_k1.c tools/host/host_hw.c tools/host/host_bk4819.c tools/host/host_beeper.c tools/host/host_bluetooth.c App/ui/main.c App/ui/menu.c App/ui/ui.c App/ui/status.c App/ui/welcome.c App/ui/battery.c App/ui/scanner.c App/ui/helper.c App/ui/inputbox.c App/ui/bt.c App/app/menu.c App/app/action.c App/app/app.c App/app/main.c App/app/generic.c App/app/common.c App/app/chFrScanner.c App/app/dtmf.c App/app/scanner.c App/app/bt.c App/radio.c App/functions.c App/audio.c App/misc.c App/driver/bluetooth.c App/driver/bt_capture.c App/driver/py25q16.c App/board.c App/settings.c App/version.c App/dcs.c App/frequencies.c App/helper/battery.c App/helper/boot.c App/driver/system.c App/font.c App/bitmaps.c App/driver/st7565.c App/driver/keyboard.c App/driver/backlight.c App/driver/audio_path.c -o /tmp/preview_k1 && /tmp/preview_k1 | grep dual`
Expected: compile error (no `SETTINGS_GetVfoTransceiver`), so the test cannot pass.

- [ ] **Step 3: Implement the setting.** In `settings.h` add the `rf_xcvr_t` enum and the two prototypes. In `settings.c`:
  - Add `uint8_t rf_xcvr[2];` to `settings_extra_t`, bump `EXTRA_VERSION` to `3u`.
  - Keep a file-static `static uint8_t s_rf_xcvr[2];`.
  - `SETTINGS_SetVfoTransceiver(vfo, xcvr)`: ignore `vfo > 1`; store `xcvr` if it is one of the three enum values, else store `RF_XCVR_AUTO`.
  - `SETTINGS_GetVfoTransceiver(vfo)`: return `s_rf_xcvr[vfo]` for `vfo <= 1`, else `RF_XCVR_AUTO`.
  - In `storage_save_settings()` copy `s_rf_xcvr` into `extra.rf_xcvr`; in the load path copy `extra.rf_xcvr` back when `version == EXTRA_VERSION`.
  - Initialise `s_rf_xcvr` to `AUTO` in `SettingsDefaults()`.

- [ ] **Step 4: Run and confirm GREEN.**

Run the Step 2 command; expected `[dual] ok ... A=1 B=2` and `0 failures`.

- [ ] **Step 5: Commit.**

```bash
git add firmware/App/settings.h firmware/App/settings.c firmware/tools/preview_k1.c
git commit -m "settings: store a per-VFO RF transceiver choice"
```

### Task 2: `RF` main-menu item

**Files:**
- Modify: `firmware/App/ui/menu.h`, `firmware/App/ui/menu.c`, `firmware/App/app/menu.c`
- Test: `firmware/tools/preview_k1.c` (the menu section)

**Interfaces:**
- Consumes: `SETTINGS_GetVfoTransceiver` / `SETTINGS_SetVfoTransceiver`, `RF_XCVR_*`, and the current VFO index `gEeprom.TX_VFO`.
- Produces: `MENU_RF` id and a `MenuList[]` row `{"RF", MENU_RF}`; a `gSubMenu_RF[] = {"Auto","4829","4815"}` label array (declare `extern` in `ui/menu.h`).

- [ ] **Step 1: Write the failing test** in the menu section of `preview_k1.c`: assert the item is present in `MenuList[]` and that accepting it edits the selected VFO.

```c
/* The RF item exists and sets the selected VFO's transceiver. */
{
    bool found = false;
    unsigned mi;
    for (mi = 0; mi < ARRAY_SIZE(MenuList); mi++)
        if (MenuList[mi].menu_id == MENU_RF) { found = true; break; }
    printf("[dual] %s RF menu item exists\n", found ? "ok  " : "FAIL");
    if (!found) failures++;
}
gEeprom.TX_VFO = 0;
gSubMenuSelection = 1;              /* BK4829 */
MENU_AcceptSetting();
ok = SETTINGS_GetVfoTransceiver(0u) == RF_XCVR_BK4829;
printf("[dual] %s RF menu item sets VFO A transceiver\n", ok ? "ok  " : "FAIL");
if (!ok) failures++;
```

- [ ] **Step 2: Run and confirm RED.** Rebuild the preview as in Task 1 Step 2; expected compile error (no `MENU_RF`).

- [ ] **Step 3: Implement.** Add `MENU_RF` to the `MENU_*` enum in `ui/menu.h`; add `{"RF", MENU_RF}` to `MenuList[]` in `ui/menu.c` next to the per-VFO rows; add `gSubMenu_RF[]` and its `extern`; in `app/menu.c` add a `MENU_GetLimits` case returning `0..2`, and a `MENU_AcceptSetting` case that calls `SETTINGS_SetVfoTransceiver(gEeprom.TX_VFO, (rf_xcvr_t)gSubMenuSelection)` and sets `gRequestSaveSettings = true`. Follow the `MENU_W_N`/`MENU_COMPAND` pattern exactly.

- [ ] **Step 4: Run and confirm GREEN.** Expected `[dual] ok ...` and `0 failures`.

- [ ] **Step 5: Commit.**

```bash
git add firmware/App/ui/menu.h firmware/App/ui/menu.c firmware/App/app/menu.c firmware/tools/preview_k1.c
git commit -m "menu: add the per-VFO RF transceiver item"
```

### Task 3: BK4815 receive service

**Files:**
- Create: `firmware/App/driver/bk4815_rx.c`, `firmware/App/driver/bk4815_rx.h`
- Test: `firmware/tools/test_bk4815_rx.c`; add it to `firmware/tools/check_all.sh`

**Interfaces:**
- Consumes: `bk4815_write_reg` / `bk4815_read_reg`, `bk4815_configure`, `pa_select_band`'s band value (0/1/2/3).
- Produces: `void bk4815_rx_tune(uint32_t freq_10hz, uint8_t band);` and `uint16_t bk4815_rx_meter(void);`

- [ ] **Step 1: Write the failing test** `test_bk4815_rx.c`: link `App/driver/bk4815.c` with a `rf_bus_write` recorder (copy the stub style from `tools/test_rf.c`), call `bk4815_rx_tune(14575000u, 1u)`, and assert the written `0x70`-block bytes and the `0x7e`-block match the transcribed formula for `r4 = 24`:

```c
/* freq 14,575,000 -> word = (145.75) * 24 * 645277.53846154 = 2257180830 */
check_hex(block_after(0x70, 0), 0xa0, "0x70 opcode");
check_hex(block_word(0x72), 0x8689d89d, "tuning word");
```

- [ ] **Step 2: Run and confirm RED.**

Run: `cd firmware && gcc -std=c11 -I tools/host -I App -I App/driver tools/test_bk4815_rx.c App/driver/bk4815.c App/driver/rf_bus.c -o /tmp/test_bk4815_rx && /tmp/test_bk4815_rx`
Expected: undefined reference to `bk4815_rx_tune`.

- [ ] **Step 3: Implement `bk4815_rx.c`** by transcribing `FUN_0801703c`:
  - `r4` from the thresholds in Global Constraints; `word = (uint32_t)((freq_10hz / 100000.0) * r4 * 645277.53846154)`.
  - Write register 4 = `(band << 7) | 0xB041`.
  - Write the 6-byte block at `0x70`: `0xa0` (or `0xe0` when the offset/TX flag is set), `0x00`, then `word` big-endian.
  - Write the 4-byte block at `0x7e` from the per-band constants `{0xffdfa037,0xffea6ad0,0xffefd01c,0xfff53568}` selected by the band index.
  - `bk4815_rx_meter()` returns the BK4815 register the radio test identifies as its RSSI; until then return `bk4815_read_reg(0x73)` with a comment marking it unconfirmed.

- [ ] **Step 4: Run and confirm GREEN.** Expected the assertions pass.

- [ ] **Step 5: Add the test to `check_all.sh`** (a `test_bk4815_rx` block in the same style as `test_rf`) and run `firmware/tools/check_all.sh`; expected `0 failures`.

- [ ] **Step 6: Commit.**

```bash
git add firmware/App/driver/bk4815_rx.c firmware/App/driver/bk4815_rx.h firmware/tools/test_bk4815_rx.c firmware/tools/check_all.sh
git commit -m "rf: add the BK4815 receive service"
```

### Task 4: `rf_dual` coordinator

**Files:**
- Create: `firmware/App/driver/rf_dual.c`, `firmware/App/driver/rf_dual.h`
- Modify: `firmware/App/driver/rx.c`, `firmware/App/driver/tx.c`, `firmware/App/main.c`
- Test: `firmware/tools/test_rf_dual.c`; add it to `firmware/tools/check_all.sh`

**Interfaces:**
- Consumes: `SETTINGS_GetVfoTransceiver`, `RF_XCVR_*`, `bk4815_rx_tune`, `gEeprom.VfoInfo[]`, `gEeprom.TX_VFO`, `gEeprom.RX_VFO`.
- Produces: `rf_xcvr_t rf_dual_resolve(uint8_t vfo);` and `void rf_dual_refresh(void);` and `bool rf_dual_secondary_active(void);`

- [ ] **Step 1: Write the failing test** `test_rf_dual.c` (pure logic; stub `SETTINGS_GetVfoTransceiver` via a settable table): assert that (a) `AUTO` resolves to `BK4829`, (b) two VFOs selecting the same chip leave only the primary active, (c) a TX VFO selecting `BK4815` resolves the primary to the other VFO or is refused.

- [ ] **Step 2: Run and confirm RED.** Expected undefined reference to `rf_dual_resolve`.

- [ ] **Step 3: Implement `rf_dual.c`:** `rf_dual_resolve` maps `AUTO` → `BK4829`, otherwise the stored value. `rf_dual_refresh` picks the primary VFO (the one whose resolved chip is `BK4829`; if both are `BK4829`, the primary is `gEeprom.RX_VFO` and the secondary is inactive), tunes the secondary with `bk4815_rx_tune(gEeprom.VfoInfo[secondary].freq_config_RX.Frequency, band)`, and records `s_secondary_active`.

- [ ] **Step 4: Wire it in:** call `rf_dual_refresh()` from `rx_service()` when a VFO frequency changes, and call it from `main.c` after settings load. Do not touch the external flash from the 10 ms slice.

- [ ] **Step 5: Run and confirm GREEN** for `test_rf_dual` and the full `check_all.sh`; expected `0 failures`.

- [ ] **Step 6: Commit.**

```bash
git add firmware/App/driver/rf_dual.c firmware/App/driver/rf_dual.h firmware/App/driver/rx.c firmware/App/driver/tx.c firmware/App/main.c firmware/tools/test_rf_dual.c firmware/tools/check_all.sh
git commit -m "rf: coordinate two transceivers across the two VFOs"
```

### Task 5: Radio gate

- [ ] Build and flash `firmware/build/Debug/ra89r_fw.icf` (see `tools/FLASHING.md`).
- [ ] Set VFO A to `4829` and VFO B to `4815`; confirm the BK4815 receives an independent signal on VFO B while VFO A stays on the BK4829.
- [ ] Confirm the BK4815 tune produces a real signal across 144–146 and 430–440 MHz, and that band register `0x75` is not corrupted.
- [ ] Set both VFOs to the same chip and confirm the radio falls back cleanly (no mis-tune, no lock-up).
- [ ] Record the result in `docs/ra89r_rfpath.md` and the feature doc; do not merge to `develop` until all pass.

## Execution notes

- Host tests run through `firmware/tools/check_all.sh`; the preview needs `App/driver/audio_path.c` linked (already in `check_all.sh`).
- If the BK4815 tune does not receive, use systematic-debugging against `FUN_0801703c` before changing the formula; the register placement (0x70..0x75 vs 0x75 band) is the most likely discrepancy.
