# ra89r_re — Retevis RA89R firmware reverse engineering

Target: Retevis RA89R / RA89G (same family as TYT UV8800 / TH9000D) on a Puya
PY32F403 (Cortex-M4F, 384 KB flash at `0x08000000`, 64 KB SRAM at `0x20000000`).
This directory holds the stock `.icf`, the codec, the analysis tooling and the
write-ups; larger reference material (the old decodes, the Ghidra project, the
CPS sources) lives outside the workspace (see "External inputs").

## Layout and ownership

- `tools/ra89r.py` — the authoritative `.icf` codec, and the only source of truth for the
  container format. Its module docstring documents the record layout, the per-family
  baselines, and the bootloader routine the check byte was derived from. Stdlib only
  (`argparse`, `os`, `struct`, `sys`) — keep it that way.
- `tools/BUILDING.md` — how to build the firmware and the host tools.
- `tools/FLASHING.md` — the concrete flash procedure: hardware, entering update
  mode, the probe/flash commands, and how to go back to stock.
- `docs/README.md` — the index of the write-ups; read it before `docs/ra89r_*.md`.
- `docs/ra89r_findings.md` — the main write-up: hardware identification, address map,
  RF/band data, UI strings, open points. Addresses in it are for the **current**
  decode; older notes/quoted addresses use a shifted coordinate system, see
  "Address drift" in that file.
- `docs/ra89r_bootloader.md` — the flashing protocol of the stock bootloader, with
  the evidence from `bootloader.bin` and from the decompiled CPS updater.
- `docs/ra89r_lcd.md` — the screen/display driver write-up (panel, pin map, init
  sequence, addressing, fonts, port notes).
- `tools/ra89r_flash.py` — host-side flasher implementing the bootloader protocol
  (`probe` and `flash` subcommands, `--dry-run` to inspect frames, `--baud auto`
  picks the fastest rate the bootloader answers at).  Needs `pyserial`; refuses
  records inside the bootloader's own flash region.
- `tools/ra89r_bootloader_sim.py` — test double for the bootloader: speaks the
  protocol on a PTY and emulates the page erase/program timing, so the flasher
  (including its retry/abort/baud-recovery paths) can be exercised without the
  radio.
- `tools/ra89r_analyze.py` — capstone-based analyzer: function discovery from the
  vector table, `bl`/`blx` sweep and pointer tables, recursive disassembly with
  literal-pool resolution, string scan, peripheral/data reference annotation.
  Needs `capstone` (installed) and the `.meta` sidecar from `tools/ra89r.py decode`.
- `firmware/` — the RA89R custom firmware project (minimum viable bring-up:
  screen + UART), documented in `docs/firmware.md`; build with
  `tools/BUILDING.md`, flash with `tools/FLASHING.md` (the concrete
  flash procedure, including how to enter update mode and how to go back to
  stock).  The build emits `build/<preset>/ra89r_fw.icf`, the file the
  bootloader accepts.  It does **not** use the HAL/LL: the
  drivers in `firmware/App/driver/` are register-level and only depend on the
  CMSIS device header from `PY32F4xx_Firmware/`.  `firmware/tools/extract_fonts.py`
  regenerates the stock font tables from a decoded image, and
  `firmware/tools/preview.c` renders the screen layout on a PC (build it with
  `-DLCD_HOST_TEST`, as documented in `docs/firmware.md`) -- use it to check
  layout changes instead of guessing.
- `work/` — generated artifacts (`*.bin`, `*.meta`, `analysis/`). Rebuild them
  from the stock `.icf` rather than editing them by hand.
- `bootloader.bin` — 16 KiB bootloader dumped from the radio over serial, mapped at
  `0x08000000` (SP `0x20003190`, reset `0x08000145`). It contains the record
  validator at `0x08000BE0` that `tools/ra89r.py`'s check byte reproduces.
- `PY32F4xx_Firmware/` — vendored upstream Puya SDK (`github.com/OpenPuya/PY32F4xx_Firmware`,
  tag `1.4.8`, its own nested git repo). Read-only reference for peripheral/register
  semantics and clock/DMA numbering; do not edit it, and do not add build output to it.
- `PY32F403_Datasheet_V1.8.pdf` and `PY32F4xx_Firmware/Documentation/PY32F403_User_Manual.chm`
  — register/datasheet reference.

## Verified commands

```sh
python3 tools/ra89r.py verify  FIRMWARE_RA89R_20260203_V49.icf   # baseline 0x66, 72 records
python3 tools/ra89r.py verify  FW.icf              # validate every record's check byte
python3 tools/ra89r.py info    FW.icf              # one line per record: address, length, key
python3 tools/ra89r.py decode  FW.icf work/fw.bin  # writes work/fw.bin + work/fw.bin.meta
python3 tools/ra89r.py encode  work/fw.bin out.icf # uses the sidecar, or --like FW.icf

python3 tools/ra89r_analyze.py work/fw.bin   # -> work/analysis/{listing,functions,...}

python3 tools/ra89r.py mkicf my_firmware.bin my_firmware.icf --base 0x08004000
python3 tools/ra89r_flash.py --dry-run flash my_firmware.icf     # inspect frames
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 probe           # is the radio in update mode?
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 flash my_firmware.icf
```

Firmware build (needs the ARM GNU toolchain; the one installed here lives in
`~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi`):

```sh
cd firmware
export ARM_TOOLCHAIN_ROOT=~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi
cmake --preset Debug && cmake --build build/Debug
```

Round-trip check (the fastest way to prove a change did not break the codec):

```sh
python3 tools/ra89r.py decode FW.icf work/fw.bin && python3 tools/ra89r.py encode work/fw.bin work/out.icf && cmp work/out.icf FW.icf
```

`encode` self-checks the records it produced and exits non-zero on failure. Patches
must not change the image size; edit the image in place and re-encode.

## Contracts that are easy to get wrong

- **Image base is `0x08004000`, not `0x08000000`.** The `.icf` carries only the
  application: 72 records, flash `0x08004000..0x0802796C` (145,772 bytes). The
  bootloader is the separate 16 KiB dump at `0x08000000`. `decode` derives the base
  from the lowest record address and writes it into the sidecar; do not re-base by hand.
- **Family baseline** is `0x66` (RA89R), `0x88` (UV8800), `0x90` (TH9000D); `tools/ra89r.py`
  detects it by trying all 256 values. Stock RA89R V49 reports `baseline 0x66 -- all 72
  records valid` — that is the sanity check to run before any analysis.
- **Only size-preserving edits are representable**, and `encode` needs either the
  `<bin>.meta` sidecar from `decode` or `--like <original.icf>` (to copy the record
  layout). The sidecar is version-tagged (`MAGIC = b"RA89RMETA3"`); regenerate rather
  than patch it.
- **`work/analysis/` is heuristic output.** Function entry points come from the
  vector table plus `bl`/`blx`/pointer-table discovery; a function that is only
  reached through a RAM-built pointer (e.g. the RF filter-switch cluster at
  `0x08020260`) may be missing from the listing. Verify against the raw image before
  concluding that code has no callers.

## Driver development workflow (per-peripheral branches)

Every peripheral driver is developed on **its own branch off `develop`**, using
the architecture of the UV-K1/K5V3 project
(`/home/gonzalo/Repos/uv-k1-k5v3-firmware-custom`, `App/driver/<name>.c|h` with a
narrow, testable interface) as the model, and merged back into **`develop`**:

```
main                      stable/import state
develop                   integration: everything merged here
driver/lcd                screen      (panel init variants, drawing, fonts)
driver/uart               console     (USART1, debug output, fault reporting)
driver/keypad             keys        (not started yet)
```

Rules: branch off `develop` (`git switch -c driver/<peripheral> develop`), keep
each driver self-contained under `firmware/App/driver/`, keep it host-testable
where possible (`firmware/tools/preview.c` for the screen), and merge into
`develop` with `git merge --no-ff` when the driver works on hardware.  Tooling,
docs and integration changes (flasher, analysis scripts, this file) go straight
onto `develop`.

## Firmware / flashing

- **The bootloader only starts the application while `0x0805FFF0` holds `0x11`**
  (docs/ra89r_bootloader.md §4c).  It clears that byte when it enters update mode, so
  a flashing tool must set it again or the radio reboots into the bootloader
  (black screen, silent UART).  `tools/ra89r_flash.py` does this by default;
  `firmware/App/main.c` reports the byte at boot over the UART.
- The firmware is linked for flash address **`0x08004000`** with a **368K** flash
  region: the stock bootloader at `0x08000000-0x08003FFF` must stay intact, and
  the stock application lives exactly where our image goes.
- Flashing goes through the stock bootloader over the programming port (USART1,
  PB6/PB7, from 9600 baud): see `docs/ra89r_bootloader.md`.  The bootloader itself
  refuses records in `0x08000000-0x08003FFF`; so does `tools/ra89r_flash.py`.
- Nothing has been run on hardware yet, and the key combination that puts the
  radio into update mode is not recovered — treat "does it run?" as unverified.
- Hardware facts encoded in `firmware/App/board.h` (LCD pins, UART pins/baud) come
  from the stock firmware, not from a board photo or schematic — check them
  first if bring-up misbehaves.

## Port context

The RA89R firmware is being ported onto the F4HWN UV-K1/K5V3 code base
(`/home/gonzalo/Repos/uv-k1-k5v3-firmware-custom`, PY32F071 — same Puya family,
different register map). That repository already contains drivers for the same
classes of part, so read it before writing new ones:

- `App/driver/st7565.c` — 128x64 ST7565 LCD over SPI (mode 3, MSB first) with a
  `gFrameBuffer[7][128]` + `gStatusLine[128]` shadow buffer, and the same `column + 4` offset the RA89R
  firmware uses. Compare it against `docs/ra89r_lcd.md` before writing the RA89R panel
  driver.
- `App/driver/bk4819.c`, `bk4829.c` — RF transceiver (bit-banged 3-wire).
- `App/driver/py25q16.c`, `mb_flash.c` — external SPI NOR flash and firmware slots.
- `App/font.c`, `bitmaps.c`, `ui/` — text/UI layer to adapt.

Cross-referencing the RA89R firmware against that tree is encouraged; editing that
tree is out of scope for this workspace unless the user asks.

## Bring-up debugging

The UART console is the primary debugging channel (the panel may be dark for
reasons that have nothing to do with the code):

- `firmware/App/driver/fault.c` prints the Cortex-M fault status and the stacked
  registers on HardFault/BusFault/MemManage/UsageFault/NMI, then halts — a crash
  is never silent.
- `firmware/App/main.c` prints a boot log (clock, reset cause, vector table,
  app-valid marker, panel variant) before touching the panel, plus a heartbeat
  every 5 s, and offers `i` diagnostics, `d` framebuffer dump, `r`/`s` panel
  re-init variants (stock sequence vs standard commands only) and `v`/`V`
  contrast.
- Console: USART1, PB6/PB7, **115200 8N1** — the same Kenwood jack the
  bootloader uses at 9600.  If the console is silent too, the application is not
  running; check `probe` (still in the bootloader?) and the marker byte.

## External inputs (not in this workspace)

- `FIRMWARE_RA89R_20260203_V49.icf` (this directory) — the stock firmware, identical
  to the copy in `~/Repos/ra_re/`; treat both as read-only inputs and never edit the
  `.icf` in place.
- `~/Repos/ra_re/` also holds the old decoder (`tyt decompiler.py`), the shifted
  decoded images, the CPS installer and `cps_decompiled/`; `~/Repos/h8_re` holds the
  Ghidra project with the RA89R programs (also in shifted coordinates).

## Working rules

- Keep scratch output in `/tmp`.  Generated artifacts live under `work/` and
  `firmware/build/`; both are listed in the root `.gitignore` and are regenerable
  from the stock `.icf` (or from the sources) at any time.
- Hardware claims (which filter is active at a given frequency, what the panel
  controller really is) come from the radio, not from static analysis alone; don't
  state a hardware behaviour as verified unless it was observed on the device.
