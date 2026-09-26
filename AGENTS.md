# ra89r_re — Retevis RA89R firmware reverse engineering

Target: Retevis RA89R / RA89G (same family as TYT UV8800 / TH9000D) on a Puya
PY32F403 (Cortex-M4F, 384 KB flash at `0x08000000`, 64 KB SRAM at `0x20000000`).
This directory holds the two stock `.icf` samples, the codec, the analysis tooling
and the write-ups; larger reference material (the old decodes, the Ghidra project,
the CPS sources) lives outside the workspace (see "Reference inputs").

## Layout and ownership

- `ra89r.py` — the authoritative `.icf` codec, and the only source of truth for the
  container format. Its module docstring documents the record layout, the per-family
  baselines, and the bootloader routine the check byte was derived from. Stdlib only
  (`argparse`, `os`, `struct`, `sys`) — keep it that way.
- `ra89r_findings.md` — the main write-up: hardware identification, address map,
  RF/band data, UI strings, open points. Addresses in it are for the **current**
  decode; older notes/quoted addresses use a shifted coordinate system, see
  "Address drift" in that file.
- `ra89r_bootloader.md` — the flashing protocol of the stock bootloader, with
  the evidence from `bootloader.bin` and from the decompiled CPS updater.
- `ra89r_lcd.md` — the screen/display driver write-up (panel, pin map, init
  sequence, addressing, fonts, port notes).
- `ra89r_battery.md` — the companion gauge chip: the bus, the protocol, the voltage
  arithmetic, the configuration block, and the full troubleshooting log (what has
  been eliminated and how, so it is not re-derived).
- `ra89r_led.md` — the status LED (a transmit/receive indicator on MCU `PA13`/`PA14`,
  confirmed on the radio) and the backlight (GPIOA pin 5), with the pin searches
  that came up empty -- including the earlier "it is the RF chip's" reading -- and
  the pin map that settled them.
- `ra89r_eeprom.md` — the external SPI NOR flash, i.e. what the CPS calls the
  EEPROM: the part and its pins, the SPI command set, what is actually on the
  chip (the flat codeplug, the firmware's journal at `0x20000`, the blob area),
  and the planned write-validation test.
- `ra89r_bk4829.md` — the RF transceiver on chip select `PB8`: its identity check
  (`0x4829`), its boot register sequence, how it differs from the UV-K1/K5V3
  driver, and the driver on branch `driver/bk4829`.
- `ra89r_bk4815.md` — the second RF transceiver, on `PB13`: its identity check
  (`0x4816`), its differently framed register access, and its boot sequence plus
  18-register table.
- `ra89r_rfpath.md` — the RF path the two share: the bit-banged bus, the boot
  bring-up order, and what actually powers the RF section.
- `ra89r_rffeatures.md` — the stock's feature routines above the part: where AF,
  AGC, the CTCSS/CDCSS/DTMF/scramble/VOX group and the sleep/idle/mode-restore
  states live, by register.

**Every feature gets its own `ra89r_<feature>.md`**, next to the code, holding more
than a summary: the protocol or register semantics, the evidence for each hardware
claim, what has already been ruled out with the evidence that ruled it out, and what
is still open.  `ra89r_findings.md` stays the cross-cutting write-up (hardware
identification, address map, UI strings); a feature doc is where a feature's own
detail lives once it has one, including its dead ends.
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
  Needs `capstone` (installed) and the `.meta` sidecar from `ra89r.py decode`.
- `firmware/` — the RA89R custom firmware project (minimum viable bring-up:
  screen + UART), with its own README and `firmware/FLASHING.md` (the concrete
  flash procedure, including how to enter update mode and how to go back to
  stock).  The build emits `build/<preset>/ra89r_fw.icf` (wrapped by a POST_BUILD
  step that calls `ra89r.py mkicf`), the file the bootloader accepts.  It does
  **not** use the HAL/LL: the drivers in `firmware/App/driver/` are register-level
  and only depend on the CMSIS device header from `PY32F4xx_Firmware/`.  Board
  facts are split so the screen layout also builds on a PC:
  `firmware/App/board_pins.h` holds the numeric pins/baud/clock and includes no
  MCU header, `firmware/App/board.h` adds the peripheral instances (GPIOA,
  USART1) and needs the device header.  `firmware/App/ui.c` is the hardware-free
  layout, `firmware/tools/extract_fonts.py` regenerates the stock font tables from
  a decoded image, and `firmware/tools/preview.c` renders the layout on a PC
  (build it with `-DLCD_HOST_TEST`, as documented in `firmware/README.md`) -- use
  it to check layout changes instead of guessing.
- `work/` — generated artifacts (`*.bin`, `*.meta`, `analysis/`, `analysis_boot/`).
  Rebuild them from the stock `.icf` rather than editing them by hand.
- `bootloader.bin` — 16 KiB bootloader dumped from the radio over serial, mapped at
  `0x08000000` (SP `0x20003190`, reset `0x08000145`). It contains the record
  validator at `0x08000BE0` that `ra89r.py`'s check byte reproduces.
- `PY32F4xx_Firmware/` — vendored upstream Puya SDK (`github.com/OpenPuya/PY32F4xx_Firmware`,
  tag `1.4.8`, its own nested git repo). Read-only reference for peripheral/register
  semantics and clock/DMA numbering; do not edit it, and do not add build output to it.
- `PY32F403_Datasheet_V1.8.pdf` and `PY32F4xx_Firmware/Documentation/PY32F403_User_Manual.chm`
  — register/datasheet reference.

## Verified commands

```sh
python3 ra89r.py verify  FIRMWARE_RA89R_20260203_V49.icf          # baseline 0x66, 72 records
python3 ra89r.py verify  Ra89G_R_UpDataFile20260401_V52_10W_Enable.icf  # same, the RA89G sample
python3 ra89r.py verify  FW.icf              # validate every record's check byte
python3 ra89r.py info    FW.icf              # one line per record: address, length, key
python3 ra89r.py decode  FW.icf work/fw.bin  # writes work/fw.bin + work/fw.bin.meta
python3 ra89r.py encode  work/fw.bin out.icf # uses the sidecar, or --like FW.icf

python3 tools/ra89r_analyze.py work/FIRMWARE_RA89R_20260203_V49.bin  # -> work/analysis/

python3 ra89r.py mkicf my_firmware.bin my_firmware.icf --base 0x08004000
python3 tools/ra89r_flash.py --dry-run flash my_firmware.icf     # inspect frames
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 probe           # is the radio in update mode?
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 flash my_firmware.icf
```

Firmware build (needs the ARM GNU toolchain; the one installed here lives in
`~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi` and is
**not** on `PATH`, so `ARM_TOOLCHAIN_ROOT` is required):

```sh
cd firmware
export ARM_TOOLCHAIN_ROOT=~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi
cmake --preset Debug && cmake --build build/Debug
```

Offline checks that need no radio (the flash and layout regressions):

```sh
# flasher against the bootloader test double (protocol + timing, on a PTY)
python3 tools/ra89r_bootloader_sim.py --pty /tmp/ra89r-pty &
python3 tools/ra89r_flash.py --port "$(cat /tmp/ra89r-pty)" flash firmware/build/Debug/ra89r_fw.icf

# the RF register layers and the K1-compatible one on a PC (53 checks)
cd firmware && gcc -std=c11 -I App -I App/driver tools/test_rf.c \
    App/driver/bk4829.c App/driver/bk4815.c App/driver/bk4819.c -o /tmp/test_rf && /tmp/test_rf

# screen layout on a PC, then eyeball the ASCII art (see firmware/README.md)
cd firmware && gcc -std=c11 -I App -I App/driver -DLCD_HOST_TEST \
    tools/preview.c App/ui.c App/driver/lcd_st7565.c \
    App/driver/font_8x16.c App/driver/font_5x7.c -o /tmp/preview && /tmp/preview
```

Round-trip check (the fastest way to prove a change did not break the codec):

```sh
python3 ra89r.py decode FW.icf work/fw.bin && python3 ra89r.py encode work/fw.bin work/out.icf && cmp work/out.icf FW.icf
```

`encode` self-checks the records it produced and exits non-zero on failure. Patches
must not change the image size; edit the image in place and re-encode.

## Contracts that are easy to get wrong

- **Image base is `0x08004000`, not `0x08000000`.** The `.icf` carries only the
  application: 72 records, flash `0x08004000..0x0802796C` (145,772 bytes). The
  bootloader is the separate 16 KiB dump at `0x08000000`. `decode` derives the base
  from the lowest record address and writes it into the sidecar; do not re-base by hand.
- **Family baseline** is `0x66` (RA89R), `0x88` (UV8800), `0x90` (TH9000D); `ra89r.py`
  detects it by trying all 256 values. Stock RA89R V49 reports `baseline 0x66 -- all 72
  records valid` — that is the sanity check to run before any analysis.
- **Two stock images ship here and both verify `0x66`/72 records**: the RA89R V49
  (`FIRMWARE_RA89R_20260203_V49.icf`, 145,772 bytes) and the newer RA89G V52
  10 W build (`Ra89G_R_UpDataFile20260401_V52_10W_Enable.icf`, 146,064 bytes).
  They are different builds (83% of bytes differ at equal offsets), so use the
  RA89G one only as a *cross-check* and compare by content, never by offset.
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
driver/lcd                screen      -- merged; working on hardware (init, fonts, layout)
driver/uart               console     -- merged; working on hardware (115200, faults)
driver/backlight          lamp        -- merged; GPIOA pin 5 (the panel backlight), 'l'
driver/keypad             keys        -- merged; 20 buttons, K5V3 KEY_Code_e, 'k' monitor
driver/led                LED         -- merged and VALIDATED: the indicator is
                                         PA13 (red) / PA14 (green), both active
                                         high, measured on the radio; console 'L'
                                         cycles off/red/green/both, see ra89r_led.md
driver/battery            gauge       -- OPEN, unmerged: the bus is silent for us
                                         although the stock reads it; see ra89r_battery.md
driver/eeprom             storage     -- OPEN, unmerged: the external SPI NOR flash
                                         ("EEPROM") reads and dumps; the write test has
                                         not run yet, see ra89r_eeprom.md
driver/audiocontrol       audio       -- OPEN, unmerged: the K1 audio-path callback
                                         drives PC13 (the amp-enable candidate the
                                         stock holds HIGH), plus pin tests: `A`
                                         steps the PA13/PA14 status-LED pair and
                                         `C` toggles PC13 -- see ra89r_led.md
driver/bk4829             RF          -- MERGED: the shared 3-wire bus, both
                                         transceivers, the stock register tables and
                                         the K1-compatible BK4819 interface.  Ids, all
                                         configuration writes, the frequency path and
                                         the RSSI response to a carrier are validated
                                         on the radio, see ra89r_bk4829.md and
                                         ra89r_bk4815.md
```

The open features have their own write-ups, and they are the places to start:

| feature | doc | state |
|---|---|---|
| keypad | `ra89r_keypad.md` | done: 20 buttons, validated on the radio |
| backlight | `ra89r_led.md` | done: GPIOA pin 5, confirmed on the radio |
| status LED | `ra89r_led.md` | done: `PA13` red, `PA14` green, active high (measured); `driver/led.c`, 'L' |
| battery gauge | `ra89r_battery.md` | protocol decoded and implemented; the chip never answers |
| beeper | `ra89r_beeper.md` | traced (TIM4 + a tone generator, its pin is PA4); not written |
| EEPROM (SPI NOR) | `ra89r_eeprom.md` | read + full dump validated on the radio; write test pending |
| RF transceivers | `ra89r_bk4829.md`, `ra89r_bk4815.md`, `ra89r_rfpath.md` | done for the BK4829: ids, all writes, tuning and an RSSI response to a carrier validated on the radio; the BK4815's RF role is still open |

Rules: branch off `develop` (`git switch -c driver/<peripheral> develop`), keep
each driver self-contained under `firmware/App/driver/`, keep it host-testable
where possible (`firmware/tools/preview.c` for the screen), and merge into
`develop` with `git merge --no-ff` **only after the change has been validated on
the radio**.  Building, passing a host/simulator test, or looking right in the
disassembly is not validation: unvalidated work stays on the
`driver/<peripheral>` branch (several commits if needed) so `develop` never
carries something we cannot stand behind.  Tooling, docs and integration changes
(flasher, analysis scripts, this file) go straight onto `develop`.

Screen, UART, backlight and keypad are merged and confirmed on the radio: the panel
shows the test card, the console logs and answers commands at 115200, `l` switches the
backlight (GPIOA pin 5), and the keypad reader decodes all 20 buttons and returns the
K5V3/F4HWN `KEY_Code_e` the port needs, with its ADC running free-running through DMA
like the stock application and a `k` console monitor to re-check any button.

Three features are unfinished and parked on their own branches, each with a doc:
the **battery gauge** (protocol decoded, chip silent -- `ra89r_battery.md`), the
**EEPROM** (read and dumped, write test pending -- `ra89r_eeprom.md`) and the
**RF transceivers** (merged: the bus, both parts and the K1-compatible interface
are validated on the radio -- ids, all writes, tuning and an RSSI response to a
carrier -- with the BK4815's RF role still open, see `ra89r_bk4829.md` and
`ra89r_bk4815.md`).
The **status LED** turned out to be MCU lines after all -- `PA13`/`PA14`, measured
on the radio -- and the branch that found them is `driver/audiocontrol`, which also
keeps the `PC13` amplifier-enable candidate (`ra89r_led.md`).  The **beeper** is traced but not written (TIM4 plus a tone
generator, its pin is PA4 = `DAC_OUT1`).

## Firmware / flashing

- **Known discrepancy, unresolved:** on the radio this firmware reports
  `app-valid marker at 0x0805FFF0 = 0xff (expected 0x11)`, while `tools/ra89r_flash.py`
  writes `0x11` there by default.  Either the read or the write is landing somewhere
  else.  It matters because a cleared marker is what leaves the radio sitting in the
  bootloader; one power cycle tells which of the two it is.
- **The bootloader only starts the application while `0x0805FFF0` holds `0x11`**
  (ra89r_bootloader.md §4c).  It clears that byte when it enters update mode, so
  a flashing tool must set it again or the radio reboots into the bootloader
  (black screen, silent UART).  `tools/ra89r_flash.py` does this by default;
  `firmware/App/main.c` reports the byte at boot over the UART.
- The firmware is linked for flash address **`0x08004000`** with a **368K** flash
  region: the stock bootloader at `0x08000000-0x08003FFF` must stay intact, and
  the stock application lives exactly where our image goes.
- Flashing goes through the stock bootloader over the programming port (USART1,
  PB6/PB7, from 9600 baud): see `ra89r_bootloader.md`.  The bootloader itself
  refuses records in `0x08000000-0x08003FFF`; so does `tools/ra89r_flash.py`.
- **The bring-up firmware runs on the radio** (see the branch table above), so a
  bring-up or flashing change can be checked against real behaviour, not only the
  simulator.  Still missing: the documented **key combination for update mode**
  (`firmware/FLASHING.md` §6, `ra89r_bootloader.md` §8 point 1), and the
  backlight's on-level.
- Board facts come from the stock firmware, not from a board photo or schematic:
  the numeric values live in `firmware/App/board_pins.h` (which must stay free of
  SDK includes so the PC preview can use it), the peripheral instances in
  `firmware/App/board.h`.  Check them first if bring-up misbehaves.

## Port context

The RA89R firmware is being ported onto the F4HWN UV-K1/K5V3 code base
(`/home/gonzalo/Repos/uv-k1-k5v3-firmware-custom`, PY32F071 — same Puya family,
different register map). That repository already contains drivers for the same
classes of part, so read it before writing new ones:

- `App/driver/st7565.c` — 128x64 ST7565 LCD over SPI (mode 3, MSB first) with a
  `gFrameBuffer[7][128]` + `gStatusLine[128]` shadow buffer, and the same `column + 4` offset the RA89R
  firmware uses. Compare it against `ra89r_lcd.md` before writing the RA89R panel
  driver.
- `App/driver/bk4819.c`, `bk4829.c` — RF transceiver (bit-banged 3-wire).
- `App/driver/py25q16.c`, `mb_flash.c` — external SPI NOR flash and firmware slots.
- `App/font.c`, `bitmaps.c`, `ui/` — text/UI layer to adapt.

Cross-referencing the RA89R firmware against that tree is encouraged; editing that
tree is out of scope for this workspace unless the user asks.

### The driver interface the port still needs

The F4HWN application is written against **one** RF driver interface,
`App/driver/bk4819.h`: about 80 `BK4819_*` entry points plus the enums
(`BK4819_AF_Type_t`, `BK4819_FilterBandwidth_t`, `BK4819_CssScanResult_t`,
`BK4819_GPIO_PIN_t`, `BK4819_REGISTER_t`), the `RegisterSpec` named-register
table in `bk4819-regs.h`, and the global `gRxIdleMode`.  That tree ships **two
implementations of that single API** -- `bk4819.c` and `bk4829.c`, defining the
same symbol names -- so the chip is a build-time choice, not an API difference.

Our drivers are three layers with about ten entry points between them
(`rf_bus_*`; `bk4829_*`/`bk4815_*` detect, read, write, configure).  So the gap
is the application-level API, and it splits three ways.

**1. The register map already matches, on the BK4829.**  From the stock image,
not from assumption:

| reg | F4HWN meaning | RA89R stock |
|---|---|---|
| `0x38`/`0x39` | `BK4819_SetFrequency` lo/hi | written raw in `FUN_08017158` (the helper `FUN_0800F670` is a two-instruction `nop; bx lr`) |
| `0x32` | `BK4819_SetFrequencyScan` (`0x244` = disabled) | `FUN_08016DE8` writes `0x32 = 0x244` |
| `0x33` | `BK4819_ToggleGpioOut`, bit `0x40 >> Pin` | `FUN_080137D4` is its only writer, same bit order |
| `0x67 & 0x1ff` | `BK4819_GetRSSI` | read that way in `FUN_080052B8` |
| `0x13` | squelch/level | `FUN_080052B8` walks it in eight steps |

Frequencies are in 10 Hz units on both sides (the RA89R's band table reads
10,800,000 for 108 MHz; F4HWN's constants do the same), so `SetFrequency` is a
straight drop-in against the BK4829.

This also forces a correction in `ra89r_bk4829.md`: register `0x33` is the
**chip's GPIO-output register**, not a band/filter bitfield.  Bits 0..6 are the
outputs in `0x40 >> pin` order and the paired bit `14 - n` is cleared per driven
pin, which is what made it look like a filter selector; the GPIOs do control
front-end paths, just not through a bitfield.

**2. Located in the stock image, by register.**  These were listed here as "not
visible in the stock image", which was wrong: the stock implements all of them and
they are simply found by taking each feature's register set from the K1 driver and
asking which stock functions write those registers.  `ra89r_rffeatures.md` has the
first pass — the AF source/mute switch (`FUN_08015F48`), the tone player writing
`0x71` (`FUN_08005D2C`), the AGC gain table (`0x10`–`0x14`, whose five values match
the K1's `InitAGC` exactly) with `0x13` as the runtime step the squelch walks, the
CTCSS/tail registers `0x51`/`0x52` fed from the codeplug (`FUN_08006DE4`), the
BK4815 compander `0x28` (`FUN_0801C15C`), the scramble enable on `0x31` bit 1 with
its code word in `0x71` (`FUN_08019E2C`), VOX (`FUN_0801754A`, register set
identical to the K1's `EnableVox`) and the mode restore that pulses `0x37` from
`0x9D00` to `0x9D1F` (`FUN_0800CE1C`).

That is location and register content, not validation: none of it has run on the
radio, `0x30`'s bit fields are unmapped, and DTMF has no chip-side routine at all
(the stock writes no second tone register, so it is most likely generated on the MCU
side -- the inference is recorded, not the routine).  `SetupPowerAmplifier` still needs the TX-power register, unidentified (`0x7d`/
`0x30` are the candidates).  For these the port keeps using the K1 implementation's
sequences, now with a stock reference to compare against rather than a hypothesis
with nothing behind it.  The BK4815 is a separate matter: it has *no*
`0x38`/`0x39` path at all -- the stock never writes frequency registers to it,
and its tuning word goes to `0x22` as `(x << 16) / 0x4822` in `FUN_08005C34`, an
encoding still to work out.  With the port bound to the BK4829 that is out of
scope for now.

**3. The architectural gap -- decided.**  F4HWN assumes one transceiver; the
RA89R has two on a shared bus and the stock picks between them per channel
(`0x20000303`).  The `BK4819_*` layer binds to the **BK4829**, which carries
frequency, RSSI, the GPIO lines, squelch and the T/R set and is therefore the
F4HWN-equivalent part; the BK4815 keeps its own calls and stays out of this port.
That is the lower-code option and the one the evidence supports; a per-call
dispatcher remains the fallback if the BK4815 turns out to be what radiates above
134 MHz.

**Done on `driver/bk4829`** (bound to the BK4829, one transceiver): the K1
interface now exists here as `firmware/App/driver/bk4819.c` / `.h` /
`bk4819-regs.h`, imported from the K1's own implementation with its copyright
headers and an adaptation note, because its register sequences are a working
driver for this part family.  Four things changed and nothing else: the
transport is `rf_bus.c` (select `PB8`) instead of the K1's file-static pins; the
delay is local; the two audio-path calls go through a registered callback; and
the four `gEeprom` reads became driver-local setters.  The console's `K` runs its
init, tunes 145.7500 MHz and turns RX on -- and did, on the radio: the frequency
registers hold the split word and `0x67` reports a plausible noise floor where an
unconfigured part read zero (ra89r_bk4829.md).

That layer is validated on the radio as far as its own behaviour goes: the ids
answer, every configuration register stores what is written, it tunes 145.7500 MHz
exactly, and `0x67` responds to a carrier (236 keyed, 57 released, straddling the
stock's own squelch marks).  What that does *not* finish: the entry points the stock
image had not been asked about (AF, signalling, AGC, idle states) still run the K1's
sequences here.  `ra89r_rffeatures.md` now locates the stock's own routines for
them, which gives each one a reference to compare against, but none of that has
been exercised on the radio either -- the `X`/`R`/`S` commands and the host test can
check registers and RSSI, not audio or signalling.  And one thing was deliberately
*not* ported: F4HWN's transport, whose chip select is a file-static define with no
way to address two parts.

## Bring-up debugging

The UART console is the primary debugging channel (the panel may be dark for
reasons that have nothing to do with the code):

- `firmware/App/driver/fault.c` prints the Cortex-M fault status and the stacked
  registers on HardFault/BusFault/MemManage/UsageFault/NMI, then halts — a crash
  is never silent.
- `firmware/App/main.c` brings the console up **before** the panel, prints a boot
  log (the CFGR/CR/FLASH_ACR and the USART1 CR1/CR2/CR3 the bootloader left,
  clock, reset cause, app-valid marker) and then runs a command console: `i`
  diagnostics, `d` framebuffer dump as ASCII, `r`/`s` panel re-init variants
  (`r` = standard sequence, i.e. the bootloader-proven one **and the default**;
  `s` = the stock application's variant, 8 extra bytes), `v`/`V` contrast,
  `l` backlight on/off, `q` heartbeat on/off, plus `h`/`c`/`t`/`b`/`f`/`p`.
- Console: USART1, PB6/PB7, **115200 8N1** — the same Kenwood jack the
  bootloader uses at 9600.  If the console is silent too, the application is not
  running; check `probe` (still in the bootloader?) and the marker byte.

## Reference inputs (also outside the tooling)

- `FIRMWARE_RA89R_20260203_V49.icf` (this directory) — the stock RA89R firmware,
  identical to the copy in `~/Repos/ra_re/`; treat both as read-only inputs and
  never edit the `.icf` in place.
- `Ra89G_R_UpDataFile20260401_V52_10W_Enable.icf` (this directory) — the RA89G
  V52 "10 W enable" distributor build of the same family; the second codec sample
  and the cross-check for anything the two models disagree on.
- `~/Repos/ra_re/` also holds the old decoder (`tyt decompiler.py`), the shifted
  decoded images, the CPS installer and `cps_decompiled/`; `~/Repos/h8_re` holds the
  Ghidra project with the RA89R programs (also in shifted coordinates).

## Remote

`origin` is `git@github.com:EA5JQP/ra89r_re.git` and every branch is pushed there
(`main`, `develop`, `driver/*`), so work in progress is backed up without having
to merge it first.  `PY32F4xx_Firmware/` is a **submodule** -- a reference to the
upstream `OpenPuya/PY32F4xx_Firmware`, with the URL in `.gitmodules` -- so
nothing of its ~150 MB is uploaded, and a clone needs
`git clone --recurse-submodules` (a plain clone leaves that directory empty).

On this host the sandbox mounts `/etc/ssh/*` owned by `nobody`, so ssh refuses to
read its own system config ("Bad owner or permissions on
/etc/ssh/ssh_config.d/20-systemd-ssh-proxy.conf") and every push fails until that
is bypassed:

```sh
GIT_SSH_COMMAND="ssh -F /dev/null -o BatchMode=yes" git push origin --all
```

## Working rules

- Keep scratch output in `/tmp`.  Generated artifacts live under `work/` and
  `firmware/build/`; both are listed in the root `.gitignore` and are regenerable
  from the stock `.icf` (or from the sources) at any time.
- Hardware claims (which filter is active at a given frequency, what the panel
  controller really is) come from the radio, not from static analysis alone; don't
  state a hardware behaviour as verified unless it was observed on the device.
