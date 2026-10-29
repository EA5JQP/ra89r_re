# ra89r_re — Retevis RA89R firmware reverse engineering

Target: Retevis RA89R / RA89G (same family as TYT UV8800 / TH9000D) on a Puya
PY32F403 (Cortex-M4F, 384 KB flash at `0x08000000`, 64 KB SRAM at `0x20000000`).
This repository holds the codec and analysis tooling (`tools/`), the write-ups
(`docs/`, indexed by `docs/README.md`) and the custom firmware (`firmware/`), a
port of the F4HWN UV-K1/K5V3 application that boots into the K1 GUI on the radio.

## Layout and ownership

- `tools/ra89r.py` — the authoritative `.icf` codec, and the only source of truth for the
  container format. Its module docstring documents the record layout, the per-family
  baselines, and the bootloader routine the check byte was derived from. Stdlib only
  (`argparse`, `os`, `struct`, `sys`) — keep it that way.
- `tools/ra89r_flash.py` — host-side flasher implementing the bootloader protocol
  (`probe`/`flash`, `--dry-run`, `--baud auto`). Needs `pyserial`; refuses records
  inside the bootloader's own flash region.
- `tools/ra89r_bootloader_sim.py` — test double for the bootloader: speaks the
  protocol on a PTY and emulates the page erase/program timing, so the flasher
  (including its retry/abort/baud-recovery paths) can be exercised without the radio.
- `tools/ra89r_analyze.py` — capstone-based analyzer of a decoded image (function
  discovery, recursive disassembly with literal pools, strings, xrefs). Needs
  `capstone` and the `.meta` sidecar from `tools/ra89r.py decode`.
- `tools/ra89r_eeprom.py` + `ra89r_eeprom_analyze.py` — dump the external SPI NOR
  flash through the firmware console (`e`/`E`) and make sense of the image offline.
- `tools/ra89r_calib.py` — dump / decode / write the factory RF calibration window
  (external flash `0x3000`, inside the codeplug's first 16 KB).
- `tools/look4sat_cat.py` — sends the exact FT-817 CAT bytes the Look4Sat phone
  app would send, to exercise the radio's CAT server.
- `tools/rf_harness.py` — drives a tinySA generator against the radio console's
  `S`/`F`/`B`/`Q` diagnostics to measure RSSI with a carrier on vs off (front-end
  pin / band-pin experiments, squelch autodetect).  Needs `pyserial`; the radio
  side also needs the CH340 USB-C port.  See `docs/ra89r_rf_harness.md`.
- `tools/ra89r_rf_console_sim.py` — test double for the console: emits the same
  `S`/`F`/`B`/`Q` text over a PTY and accepts CAT frames, so `rf_harness.py` can
  be exercised with no radio attached.
- `tools/ra89r_bt.py` — the Bluetooth debug tool: talks to the firmware's `!`
  BT debug lines on the radio console (status, trace, raw AT, inject, restart,
  a guided `ptt-test`) or sniffs the module's own UART; `--selftest` runs it
  against `tools/ra89r_bt_console_sim.py`; `tools/ra89r_bt_headset.py` makes
  this PC the earpiece (HFP + SCO/mSBC, a known mic signal, PTT) for over-air
  audio tests.  See `docs/ra89r_bt_debug.md`.
- `tools/cat_rf_test.py` — the CAT + tinySA-MCP receive test: tunes and reads the
  S-meter over CAT (`SM`/`STATUS`) and drives the carrier through the tinySA MCP;
  `tools/tinysa_mcp_sim.py` is its stdio MCP test double, so `--sim` and
  `--mcp-selftest` run without hardware.  See `docs/ra89r_cat_tinysa_test.md`.
- `tools/BUILDING.md` — building the firmware and host tools. `tools/FLASHING.md`
  — the concrete flash procedure: hardware, entering update mode, the probe/flash
  commands, and how to go back to stock.
- `docs/README.md` — the index of the write-ups; read it before any `docs/ra89r_*.md`.
  Convention: **every feature gets its own `ra89r_<feature>.md`** next to the code,
  holding the protocol/register semantics, the evidence for each hardware claim,
  what has been ruled out and how, and what is still open. `docs/ra89r_findings.md`
  stays the cross-cutting write-up. Feature work starts with a dated plan/spec under
  `docs/superpowers/plans|specs/` (tracked; see the dual-rf and scan examples).
- `firmware/` — the RA89R custom firmware project, documented in
  `docs/ra89r_port.md`. The build emits `build/<preset>/ra89r_fw.icf` (a POST_BUILD
  step calls `tools/ra89r.py mkicf`), the file the bootloader accepts. It does
  **not** use the HAL/LL: `firmware/App/driver/` is register-level and depends only
  on the CMSIS device header from `PY32F4xx_Firmware/`. Board facts are split so
  the code also builds on a PC: `firmware/App/board_pins.h` holds the numeric
  pins/baud/clock and includes no MCU header; `firmware/App/board.h` adds the
  peripheral instances. `firmware/tools/check_all.sh` runs every offline check
  (below); `firmware/tools/preview*.c` and `firmware/tools/host/` render and
  key-drive the same code on a PC.
- `work/` — generated artifacts (`*.bin`, `*.meta`, `analysis/`). Rebuild from the
  stock `.icf` rather than editing by hand.
- `bootloader.bin` — 16 KiB bootloader dumped from the radio over serial, mapped at
  `0x08000000`. It contains the record validator at `0x08000BE0` that
  `tools/ra89r.py`'s check byte reproduces.
- Root `*.bin` dumps (`stock_eeprom.bin`, `after.bin`, `calib.bin`) are committed
  reference dumps of the external SPI NOR flash (see `tools/ra89r_eeprom.py` /
  `tools/ra89r_calib.py`) — reference inputs, not build output.
- `PY32F4xx_Firmware/` — vendored upstream Puya SDK (tag `1.4.8`, a nested git
  repo/submodule). Read-only reference for peripheral/register semantics; do not
  edit it, and do not add build output to it.
- `PY32F403_Datasheet_V1.8.pdf`, `docs/BK4815N_Datasheet_v1.0.1.md` — register
  reference. `LICENSE` is Apache-2.0 (the port imports DualTachyon's UV-K5 code);
  the per-file attribution list lives in `NOTICE`.

The stock `.icf` samples are **gitignored reference inputs, not tracked** (and
larger reference material — the old decodes, the Ghidra project, the CPS sources —
lives outside the workspace, e.g. `~/Repos/ra_re/`, `~/Repos/h8_re`).

## Verified commands

```sh
python3 tools/ra89r.py verify  STOCK.icf   # detects the family baseline, validates every record
python3 tools/ra89r.py info    STOCK.icf   # one line per record: address, length, key
python3 tools/ra89r.py decode  STOCK.icf work/fw.bin   # + work/fw.bin.meta sidecar
python3 tools/ra89r.py encode  work/fw.bin out.icf     # uses the sidecar, or --like STOCK.icf

python3 tools/ra89r_analyze.py work/<decoded>.bin      # -> work/analysis/

python3 tools/ra89r.py mkicf my_firmware.bin my_firmware.icf --base 0x08004000
python3 tools/ra89r_flash.py --dry-run flash my_firmware.icf     # inspect frames
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 probe           # is the radio in update mode?
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 flash my_firmware.icf
```

Round-trip check (the fastest way to prove a codec change did not break anything):

```sh
python3 tools/ra89r.py decode FW.icf work/fw.bin && python3 tools/ra89r.py encode work/fw.bin work/out.icf && cmp work/out.icf FW.icf
```

`encode` self-checks the records it produced and exits non-zero on failure. Patches
must not change the image size; edit the image in place and re-encode.

## Firmware build and offline checks

Build (needs the ARM GNU toolchain; the one installed here lives in
`~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi` and is **not**
on `PATH`, so `ARM_TOOLCHAIN_ROOT` is required):

```sh
cd firmware
export ARM_TOOLCHAIN_ROOT=~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi
cmake --preset Debug && cmake --build build/Debug
```

All offline checks in one go — the host tests (RF register paths, dual-RF
coordinator, beeper tone math, BK1080, scan policy, CAT parser), the K1 screen/key
preview (`preview_k1`, renders the VFO/menu screens as ASCII and runs the K1 key
loop), and the target build if the toolchain is set:

```sh
firmware/tools/check_all.sh        # needs gcc; the build step is skipped without ARM_TOOLCHAIN_ROOT
```

The checks that script does **not** cover:

```sh
# flasher against the bootloader test double (protocol + timing, on a PTY)
python3 tools/ra89r_bootloader_sim.py --pty /tmp/ra89r-pty &
python3 tools/ra89r_flash.py --port "$(cat /tmp/ra89r-pty)" flash firmware/build/Debug/ra89r_fw.icf

# the bring-up screen layout on a PC (see docs/firmware.md), then eyeball the ASCII art
cd firmware && gcc -std=c11 -I App -I App/driver -DLCD_HOST_TEST \
    tools/preview.c App/ui.c App/driver/lcd_st7565.c \
    App/driver/font_8x16.c App/driver/font_5x7.c -o /tmp/preview && /tmp/preview

# the codec round-trip above
```

`tools/host/` is a test double for the device header: CMSIS's `__DSB()` is ARM
assembly, so a PC build cannot use the real one (see `NOTICE`).

## Contracts that are easy to get wrong

- **Image base is `0x08004000`, not `0x08000000`.** The `.icf` carries only the
  application. The bootloader is the separate 16 KiB dump at `0x08000000`.
  `decode` derives the base from the lowest record address and writes it into the
  sidecar; do not re-base by hand.
- **Family baseline** is `0x66` (RA89R), `0x88` (UV8800), `0x90` (TH9000D);
  `tools/ra89r.py` detects it by trying all 256 values. Stock RA89R V49 verifies
  `baseline 0x66 -- all 72 records valid`; the RA89G V52 10 W build verifies the
  same. The two are **different builds** (83% of bytes differ at equal offsets), so
  use the RA89G one only as a *cross-check* and compare by content, never by offset.
- **Only size-preserving edits are representable**, and `encode` needs either the
  `<bin>.meta` sidecar from `decode` or `--like <original.icf>`. The sidecar is
  version-tagged (`MAGIC = b"RA89RMETA3"`); regenerate rather than patch it.
- **The factory calibration window (external flash `0x3000`–`0x38AF`) must never
  be overwritten** — the stock reads it into the RF chips at boot
  (`docs/ra89r_calibration.md`). A flash write that clobbers the codeplug's first
  16 KB is not recoverable without the original dump (`calib.bin` here).
- **`work/analysis/` is heuristic output.** Function entry points come from the
  vector table plus `bl`/`blx`/pointer-table discovery; a function only reached
  through a RAM-built pointer may be missing from the listing. Verify against the
  raw image before concluding that code has no callers.
- **Vocabulary: *located* vs *verified*.** A register sequence read out of the
  disassembly is *located*, not validated; a behaviour is only *verified* once it
  was observed on the radio (see `docs/README.md` conventions).

## Driver development workflow (per-peripheral branches)

Rules: branch off `develop` (`git switch -c driver/<peripheral> develop`), keep
each driver self-contained under `firmware/App/driver/`, keep it host-testable
where possible, and merge into `develop` with `git merge --no-ff` **only after the
change has been validated on the radio**. Building, passing a host/simulator test,
or looking right in the disassembly is not validation: unvalidated work stays on
the `driver/<peripheral>` branch so `develop` never carries something we cannot
stand behind. Tooling, docs and integration changes go straight onto `develop`.
Every branch is pushed to `origin` as WIP backup.

State (each has its own doc — read it before touching the feature):

| area | state |
|---|---|
| lcd, uart, backlight, keypad | merged; validated on the radio (panel, 115200 console, `PA5` backlight, 20-button ADC ladder → `KEY_Code_e`) |
| led | merged; `PA13` red / `PA14` green, active high, measured (`docs/ra89r_led.md`) |
| beeper | merged; DAC tone on `PA4`/`DAC_OUT1` via TIM4 + DMA1 ch3, heard on the radio |
| audiocontrol, pa, tx, rx | merged; **transmit works** (voice heard on a second radio) — the measured chain is `driver/pa.c` + `driver/tx.c` (`docs/ra89r_rfpath.md`).  Receive gate: per-(lane, band) noise floor + level margin, driven every poll (`driver/rx.c`, `rx.h`) |
| bk4829 | merged; the shared 3-wire bus, both transceivers, ids/config/tuning/RSSI validated (`docs/ra89r_bk4829.md`, `docs/ra89r_bk4815.md`) |
| dual-rf (`driver/rf_dual.c`) | merged; per-VFO transceiver selection, host-tested (`test_rf_dual`) |
| scan (`app/chFrScanner.c`) | `SetScn` NORMAL / FAST on the VFO's own chip; the memory scan locks in both on either transceiver (radio 2026-10-27).  `FAST BOTH` and `LISTEN` were **removed** on `driver/scan-route`: the chips share one set of receive-path pins, so the chip they are not set for hears nothing (`docs/ra89r_scan.md`) |
| usbc / CAT (`App/cat.c`, `cat_radio.c`) | merged; FT-817 CAT on the USB-C serial port, validated (`docs/ra89r_cat.md`, `ra89r_carserial.md`); enabled by `ENABLE_CAT` in `k1_features.h`, console kept |
| fm (`driver/bk1080.c`, `app/fm.c`) | merged but **unvalidated on the radio** (`docs/ra89r_bk1080.md`) |
| bluetooth (`app/bt.c`, `driver/bluetooth.c`) | merged; Jieli YBT100 on USART3 `PB10`/`PB11` 115200, `PD0` as reset/enable, F+MENU BT screen + service.  **Validated on the radio (2026-10-29):** pairing/connect, BT receive audio in the earpiece, the headset PTT keys TX, and speech sent over SCO from an emulated headset (`tools/ra89r_bt_headset.py`) came through clear on a second radio; on the build merged with develop's squelch/scan work, receive/squelch and BT receive audio were re-checked.  Debug: `!` console lines + `tools/ra89r_bt.py` (`docs/ra89r_bluetooth.md`, `docs/ra89r_bt_debug.md`) |
| airband | merged; the receive demodulation is chosen by band (108.0-137.0 MHz is AM), so an airband range scan demodulates AM; validated on the radio (clean AM tone at boot, a 108-137 scan stops on the carrier) (`docs/ra89r_airband.md`) |
| battery | protocol decoded and implemented; the chip never answers (`docs/ra89r_battery.md`) |
| eeprom (`py25q16.c`, `spi_flash.c`) | read + full dump validated; the write-validation test has not run (`docs/ra89r_eeprom.md`) |
| port | merged; the K1 GUI boots on the radio (`docs/ra89r_port.md`) |
| spectrum (`app/spectrum.c`, branch `spectrum`) | open, unmerged, **unvalidated**: the K1/F4HWN spectrum screen (F+5) on the BK4829 *or* the BK4815, routed through `pa_set_rx_lane()`/`pa_select_band()` like the receiver; no "both" mode, for the shared-pins reason `FAST BOTH` was removed (`docs/ra89r_spectrum.md`) |

## RF architecture (read before touching the RF code)

- Two transceivers, one bit-banged bus: BK4829 on select `PB8`, BK4815 on `PB13`
  (`driver/rf_bus.c`). They are *not* interchangeable: the BK4829 carries
  frequency, RSSI, the chip GPIOs, squelch and the validated transmit path; the
  BK4815 is the > 134 MHz receive path (`docs/ra89r_bk4815.md`).
- The application is written against the K1's single-RF interface,
  `App/driver/bk4819.h` (~80 `BK4819_*` entry points). That API is implemented in
  `driver/bk4819.c` **bound to the BK4829**. `driver/rf_dual.c` decides which chip
  is tuned to which VFO (`rf_dual_choose` is pure and host-testable); **transmit is
  always the BK4829** — it is the only part with a validated TX path.
- The register map matches the K1's expectations on the BK4829 (`0x38`/`0x39`
  frequency, `0x67` RSSI, `0x13` AGC/squelch step, `0x33` chip-GPIO outputs —
  not a filter bitfield). Frequencies are in 10 Hz units on both sides.
- The stock's feature routines (AF, AGC, CTCSS/scramble/VOX, compander, mode
  restore) are *located* by register in `docs/ra89r_rffeatures.md`; the port keeps
  running the K1's sequences for those, with the stock as a reference — none of it
  counts as validated. DTMF has no chip-side routine at all (most likely MCU-side).
- **Full-duplex is impossible** — the two chips share one band-switched front end;
  the stock's "Aut Repeat" is a cross-band *repeater* only. Don't try to build
  simultaneous RX+TX; see `docs/ra89r_fullduplex.md`.

## Port context

The application is ported from the F4HWN UV-K1/K5V3 tree
(`/home/gonzalo/Repos/uv-k1-k5v3-firmware-custom`, PY32F071 — same Puya family,
different register map). It already contains drivers for the same classes of part
(`st7565.c`, `bk4819.c`/`bk4829.c`, `py25q16.c`, `font.c`, `bitmaps.c`, `ui/`) —
read it before writing new ones. Editing that tree is out of scope unless asked.

Current port state on `develop`: the radio **boots into the K1 GUI** (double-row
layout forced by `k1_features.h`; `gEeprom.DUAL_WATCH` stays OFF so the receiver
follows the selected VFO — dual-watch would toggle it). `settings.c` decodes the
stock's 21-byte channel records, bitmaps and tones (`docs/ra89r_codeplug.md`) into
the K1's `gEeprom`, and `driver/py25q16.c` backs settings with the external SPI NOR.
Keys: `EXIT` switches VFO A/B (this radio has no A/B key); PTT is deliberately on
the measured TX chain. `docs/ra89r_port.md` is the port's working document and
`docs/ra89r_codeplug.md` is the authority for anything the codeplug holds.

The UART console is the primary debugging channel — if it is silent too, the
application is not running (run `probe`; is the radio still in the bootloader at
9600?). Console: USART1, PB6/PB7, **115200 8N1** on the same Kenwood jack the
bootloader uses at 9600. The command switch lives in `App/main.c` (single-letter
commands; `'i'` diagnostics, `'d'` framebuffer dump, `'e'` EEPROM dump,
`'4'`/`'1'` boot screen/GUI, `'5'` save settings, `'6'` flash write test, `R`/`W`/`X`/`K`
RF bring-up). `firmware/App/driver/fault.c` prints the Cortex-M fault status and
stacked registers on any fault, then halts — a crash is never silent.
`App/main.c` reports the `0x0805FFF0` update-request byte at boot.

## Firmware / flashing

- **`0x0805FFF0` is the bootloader's *update-mode request*, not an
  application-valid flag** (`docs/ra89r_bootloader.md` §4c). `0xFF` is the normal
  value and starts the application; `0x11` makes it enter update mode, and it
  consumes the request on the way in. The stock app sets `0x11` on a serial
  `Reset`+`'0'`; the key combination (`tools/FLASHING.md` §6) is the manual
  equivalent. `tools/ra89r_flash.py` writes `0x11` by default (`--no-valid-marker`
  skips it) and the bootloader consumes it on the next reset, so the flash ends
  with the radio running either way. Nothing validates the image.
- The bootloader also enters update mode when **PB9 and PA2 are both low at
  reset**, whatever that byte says, and its launch trampoline checks almost nothing.
- The firmware is linked for flash address `0x08004000` with a **368K** flash
  region: the bootloader at `0x08000000-0x08003FFF` must stay intact (both the
  bootloader and `ra89r_flash.py` refuse records in it).
- The port runs the MCU at **48 MHz** (HSI x6, 1 flash wait; `BOARD_PLL_MUL` /
  `BOARD_FLASH_WS` in `board_pins.h`). It once stayed at the 8 MHz reset default,
  which made every bit-banged bus ~6x slower than the K1 it was compared against —
  suspect clock/bus timing before the algorithm when the port is slow.

## Working rules

- Keep scratch output in `/tmp` (use `/tmp/opencode`). Generated artifacts live
  under `work/` and `firmware/build/`; both are gitignored and regenerable.
- In the K1 app loop, nothing on the 10 ms slice may touch the external SPI NOR
  flash, and a timeout does not belong in a backlight fade; both mistakes shipped
  once and made the radio unusable.
- Hardware claims (which filter is active, what the panel controller really is)
  come from the radio, not from static analysis alone; don't state a hardware
  behaviour as verified unless it was observed on the device.
- Host-testable by construction: `driver/beeper.h` is device-header free so the
  tone math can be checked without a radio; `rf_dual_choose` is pure; the app core
  renders and key-drives on a PC via `preview_k1`. When adding a driver, keep the
  narrow-interface pattern and add its test to `firmware/tools/check_all.sh`.

## Remote

`origin` is `git@github.com:EA5JQP/ra89r_re.git` and every branch is pushed there
(`main`, `develop`, `driver/*`, `port`, `scan`, `spectrum`), so work in progress is
backed up without having to merge it first. `PY32F4xx_Firmware/` is a **submodule**
(see `.gitmodules`) — a clone needs `git clone --recurse-submodules`.

On this host the sandbox mounts `/etc/ssh/*` owned by `nobody`, so ssh refuses to
read its own system config and every push fails until bypassed:

```sh
GIT_SSH_COMMAND="ssh -F /dev/null -o BatchMode=yes" git push origin --all
```
