# ra89r_re — Retevis RA89R firmware reverse engineering

Target: Retevis RA89R / RA89G (same family as TYT UV8800 / TH9000D) on a Puya
PY32F403 (Cortex-M4F, 384 KB flash at `0x08000000`, 64 KB SRAM at `0x20000000`).
This repository holds the codec and analysis tooling (`tools/`), the write-ups
(`docs/`, indexed by `docs/README.md`) and the custom firmware (`firmware/`).
The two stock `.icf` samples are reference inputs kept on disk but **not tracked**
(they are in `.gitignore`); larger reference material (the old decodes, the Ghidra
project, the CPS sources) lives outside the workspace (see "Reference inputs").

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
- `docs/ra89r_battery.md` — the battery sense (ADC channel 9 / `PB1`) plus the
  record of the earlier "companion gauge" dead end: the bus on `PC14`/`PB2` is
  the BK1080 FM receiver (below), not a gauge.
- `docs/ra89r_bk1080.md` — the BK1080 FM receiver on `PC14`/`PB2`: the I2C
  framing, the register map the stock uses, the 68-byte init block, the tuning
  word, and the RSSI/seek path, with the driver on branch `driver/bk1080`.
- `docs/ra89r_keypad.md` — the 20-button ADC-ladder key matrix and its F4HWN
  `KEY_Code_e` mapping.
- `docs/ra89r_pins.md` — the board-wide MCU pin inventory: every GPIO line the
  stock touches and the lines it leaves free (`PC0`–`PC12`, `PD2`, GPIOE), with
  the helper model and evidence, for picking a pin for a new feature.
- `docs/ra89r_beeper.md` — the beeper: a DAC tone on `PA4`, played by TIM4 + DMA.
- `docs/ra89r_led.md` — the status LED (a transmit/receive indicator on MCU `PA13`/`PA14`,
  confirmed on the radio) and the backlight (GPIOA pin 5), with the pin searches
  that came up empty -- including the earlier "it is the RF chip's" reading -- and
  the pin map that settled them.
- `docs/ra89r_eeprom.md` — the external SPI NOR flash, i.e. what the CPS calls the
  EEPROM: the part and its pins, the SPI command set, what is actually on the
  chip (the flat codeplug, the firmware's journal at `0x20000`, the blob area),
  and the planned write-validation test.
- `docs/ra89r_codeplug.md` — the *contents* of that chip's first 16 KB: the 21-byte
  channel records, the two channel bitmaps, the tone encoding, the band ranges,
  the 32-byte general-settings block field by field, the calibration window and
  the signature, each with the CPS source line that proves it.  The authority for
  anything the port reads or writes there.
- `docs/ra89r_bk4829.md` — the RF transceiver on chip select `PB8`: its identity check
  (`0x4829`), its boot register sequence, how it differs from the UV-K1/K5V3
  driver, and the driver on branch `driver/bk4829`.
- `docs/ra89r_bk4815.md` — the second RF transceiver, on `PB13`: its identity check
  (`0x4816`), its differently framed register access, and its boot sequence plus
  18-register table.
- `docs/ra89r_rfpath.md` — the RF path the two share: the bit-banged bus, the boot
  bring-up order, and what actually powers the RF section.
- `docs/ra89r_rffeatures.md` — the stock's feature routines above the part: where AF,
  AGC, the CTCSS/CDCSS/DTMF/scramble/VOX group and the sleep/idle/mode-restore
  states live, by register.
- `docs/ra89r_port.md` — the port itself: what the RA89R side already provides, the
  fixes and the missing modules the K1 application needs before it can run, the
  board facts to re-point, and the order to do it in.  The `port` branch's
  working document.

**Every feature gets its own `ra89r_<feature>.md`**, next to the code, holding more
than a summary: the protocol or register semantics, the evidence for each hardware
claim, what has already been ruled out with the evidence that ruled it out, and what
is still open.  `docs/ra89r_findings.md` stays the cross-cutting write-up (hardware
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
  Needs `capstone` (installed) and the `.meta` sidecar from `tools/ra89r.py decode`.
- `firmware/` — the RA89R custom firmware project (K1/F4HWN port in progress),
  documented in `docs/ra89r_port.md` (current) and `docs/firmware.md` (the
  original screen+UART bring-up, which the port has outgrown -- it still says
  8 MHz and screen-only, so trust the code and `ra89r_port.md`); build with
  `tools/BUILDING.md`, flash with `tools/FLASHING.md` (the concrete
  flash procedure, including how to enter update mode and how to go back to
  stock).  The build emits `build/<preset>/ra89r_fw.icf` (wrapped by a POST_BUILD
  step that calls `tools/ra89r.py mkicf`), the file the bootloader accepts.  It does
  **not** use the HAL/LL: the drivers in `firmware/App/driver/` are register-level
  and only depend on the CMSIS device header from `PY32F4xx_Firmware/`.  Board
  facts are split so the screen layout also builds on a PC:
  `firmware/App/board_pins.h` holds the numeric pins/baud/clock and includes no
  MCU header, `firmware/App/board.h` adds the peripheral instances (GPIOA,
  USART1) and needs the device header.  `firmware/App/ui.c` is the hardware-free
  layout, `firmware/tools/extract_fonts.py` regenerates the stock font tables from
  a decoded image, and `firmware/tools/preview.c` renders the layout on a PC
  (build it with `-DLCD_HOST_TEST`, as documented in `docs/firmware.md`) -- use
  it to check layout changes instead of guessing.
- `work/` — generated artifacts (`*.bin`, `*.meta`, `analysis/`, `analysis_boot/`).
  Rebuild them from the stock `.icf` rather than editing them by hand.
- `bootloader.bin` — 16 KiB bootloader dumped from the radio over serial, mapped at
  `0x08000000` (SP `0x20003190`, reset `0x08000145`). It contains the record
  validator at `0x08000BE0` that `tools/ra89r.py`'s check byte reproduces.
- `PY32F4xx_Firmware/` — vendored upstream Puya SDK (`github.com/OpenPuya/PY32F4xx_Firmware`,
  tag `1.4.8`, its own nested git repo). Read-only reference for peripheral/register
  semantics and clock/DMA numbering; do not edit it, and do not add build output to it.
- `PY32F403_Datasheet_V1.8.pdf` and `PY32F4xx_Firmware/Documentation/PY32F403_User_Manual.chm`
  — register/datasheet reference.
- `LICENSE` — **Apache License 2.0**, the same license as the UV-K1/K5V3 (F4HWN)
  project this firmware is ported from, which carries DualTachyon's original
  UV-K5 copyright.  Imported files keep their own copyright headers; the
  per-file attribution list lives in `NOTICE`.

## Verified commands

```sh
python3 tools/ra89r.py verify  FIRMWARE_RA89R_20260203_V49.icf          # baseline 0x66, 72 records
python3 tools/ra89r.py verify  Ra89G_R_UpDataFile20260401_V52_10W_Enable.icf  # same, the RA89G sample
python3 tools/ra89r.py verify  FW.icf              # validate every record's check byte
python3 tools/ra89r.py info    FW.icf              # one line per record: address, length, key
python3 tools/ra89r.py decode  FW.icf work/fw.bin  # writes work/fw.bin + work/fw.bin.meta
python3 tools/ra89r.py encode  work/fw.bin out.icf # uses the sidecar, or --like FW.icf

python3 tools/ra89r_analyze.py work/FIRMWARE_RA89R_20260203_V49.bin  # -> work/analysis/

python3 tools/ra89r.py mkicf my_firmware.bin my_firmware.icf --base 0x08004000
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

# the RF register layers, the K1-compatible one and the PA/RX path on a PC
# (62 checks).  `driver/bk4819.c` mirrors the status LED onto `driver/led.c`,
# which needs the target's GPIO registers, so the host LED stand-in goes in its
# place; `driver/pa.c` is linked for the `0x33` regression, with its two
# `pa_init()` GPIO calls stubbed in the test (so `driver/gpio.c` is not pulled
# in).
cd firmware && gcc -std=c11 -I tools/host -I App -I App/driver tools/test_rf.c \
    App/driver/bk4829.c App/driver/bk4815.c App/driver/bk4819.c \
    App/driver/pa.c App/driver/tx.c tools/host/host_led.c -o /tmp/test_rf && /tmp/test_rf

# the beeper's tone math (the timer reload and the sine table) on a PC.  It links
# nothing else: driver/beeper.h is device-header free on purpose, so the tone
# the DAC will play can be checked without a radio.
cd firmware && gcc -std=c11 -I App -I App/driver tools/test_beeper.c \
    -o /tmp/test_beeper && /tmp/test_beeper

# screen layout on a PC, then eyeball the ASCII art (see docs/firmware.md)
cd firmware && gcc -std=c11 -I App -I App/driver -DLCD_HOST_TEST \
    tools/preview.c App/ui.c App/driver/lcd_st7565.c \
    App/driver/font_8x16.c App/driver/font_5x7.c -o /tmp/preview && /tmp/preview

# the ported K1 application on a PC: it renders the VFO/menu screens as ASCII,
# decodes the codeplug the host's RAM flash holds, round-trips the settings blob
# and runs the K1's own key path (CheckKeys).  It links the whole app core, not
# just the screens -- port_state_init() is the K1's boot sequence.
cd firmware && gcc -std=c11 -I tools/host -I App -I App/driver \
    -DPY32F403xD -include App/k1_features.h -DST7565_HOST_TEST \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    tools/preview_k1.c tools/host/host_hw.c tools/host/host_bk4819.c \
    tools/host/host_beeper.c \
    App/ui/main.c App/ui/menu.c App/ui/ui.c App/ui/status.c App/ui/welcome.c \
    App/ui/battery.c App/ui/scanner.c App/ui/helper.c App/ui/inputbox.c \
    App/ui/fmradio.c \
    App/app/menu.c App/app/action.c App/app/app.c App/app/main.c \
    App/app/generic.c App/app/common.c App/app/chFrScanner.c App/app/dtmf.c \
    App/app/scanner.c App/app/fm.c App/radio.c App/functions.c App/audio.c App/misc.c \
    App/driver/py25q16.c \
    App/board.c App/settings.c App/version.c App/dcs.c App/frequencies.c \
    App/helper/battery.c App/helper/boot.c App/driver/system.c App/font.c App/bitmaps.c \
    App/driver/st7565.c App/driver/keyboard.c App/driver/backlight.c \
    App/driver/scheduler.c \
    -o /tmp/preview_k1 && /tmp/preview_k1
# (tools/host is a test double for the device header: CMSIS's __DSB() is ARM
#  assembly, so a PC build cannot use the real one -- see NOTICE)
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
                                         cycles off/red/green/both, see docs/ra89r_led.md
driver/battery            gauge       -- OPEN, unmerged: the bus is silent for us
                                         although the stock reads it; see docs/ra89r_battery.md
driver/eeprom             storage     -- OPEN, unmerged: the external SPI NOR flash
                                         ("EEPROM") reads and dumps; the write test has
                                         not run yet, see docs/ra89r_eeprom.md
driver/beeper             beeper      -- MERGED: the DAC tone (driver/beeper.c,
                                         PA4/DAC_OUT1, TIM4 + DMA1 channel 3), heard
                                         on the radio (the power-on sound and the key
                                         beeps), see docs/ra89r_beeper.md
driver/audiocontrol       audio       -- MERGED.  It receives and transmits: the K1
                                         bring-up plus the audio path are audible on
                                         a second radio, the squelch mutes the
                                         chip's AF on the stock's 0xB4/0xCF marks,
                                         and **PTT sends voice that a second radio
                                         hears** (SIDE1 sends the chip's own DTMF
                                         tone as a reference).  The measured transmit
                                         chain lives in `driver/pa.c` (the PB14/TIM1
                                         bias PWM, the band-path pins, the chip's PA
                                         enable in `0x33` and `0x36 = 0x8822` -- the
                                         register the stock never writes and the K1
                                         import zeroed) and `driver/tx.c` (tune,
                                         `0x7D`, PrepareTransmit, the `0x50 = 0x3B20`
                                         unmute the import was not sending, the mic
                                         gain in `0x40`).  See docs/ra89r_rfpath.md
port                      integration -- OPEN: the K1/F4HWN VFO+menu port, off
                                         develop and not end-to-end validated,
                                         though it runs on the radio and its
                                         recent fixes came from those runs.  The
                                         K1's own ui/main.c (VFO), ui/menu.c (menu +
                                         MenuList[]), ui/status.c, ui/welcome.c,
                                         ui/ui.c, ui/helper.c, ui/inputbox.c and its
                                         font/bitmap/table modules (font.c, bitmaps.c,
                                         dcs.c, frequencies.c, version.c,
                                         helper/battery.c, app/menu.c, app/action.c)
                                         compile, link and render;
                                         driver/st7565.c gives the K1's buffer layout
                                         over this repo's bit-banged panel;
                                         driver/keyboard.c is the K1 keyboard
                                         interface over our keypad reader and the
                                         K1's app/main.c routes the keys; ui/status.c
                                         draws the status line.  driver/py25q16.c
                                         backs settings with the external SPI NOR
                                         flash, giving the K1 its own EEPROM image in
                                         the erased band.  The K1 GUI is
                                         what the radio boots into, straight into the
                                         VFO, in its double-channel layout
                                         (`TWO_ROW_UI` in k1_features.h
                                         forces `ui/main.c`'s `isMainOnly()`
                                         false; `gEeprom.DUAL_WATCH` stays OFF so
                                         the receiver follows the selected VFO --
                                         dual-watch would toggle it);
                                         the two VFOs land on the first two channels
                                         the codeplug has, because settings.c
                                         decodes the stock's own 21-byte records,
                                         bitmaps and tones (docs/ra89r_codeplug.md) and
                                         settings.c is the K1's SETTINGS_* interface
                                         over it; console '4' shows the K1
                                         boot screen, '1' returns to the GUI,
                                         '2'/'3'/'G'/'M'/'4' select screens, '0' hands
                                         the panel back to the bring-up screens,
                                         '5' saves settings, '6' runs the flash write
                                         test, 'e' dumps the flash.
                                         preview_k1.c renders and key-drives the same
                                         screens on a PC (see "Offline checks").
                                         The app core is in as well (radio.c,
                                         functions.c, audio.c, misc.c + a port
                                         driver/system.c), and driver/tx.c /
                                         driver/rx.c drive the measured chains:
                                         UP/DOWN retune the receiver, PTT keys the
                                         transmitter, the squelch sets
                                         FUNCTION_INCOMING/RECEIVE.
                                         radio.c's own chip sequences stay unused
                                         until they are compared with the stock.
                                         The K1's app loop and key layer are in too
                                         (app/app.c CheckKeys + APP_Update, app/main.c
                                         MAIN_ProcessKeys, generic/common/chFrScanner/
                                         dtmf/scanner), so the menu items act and the
                                         keys follow the K1 -- with PTT deliberately
                                         left on the measured tx chain
                                         (GPIO_IsPttPressed returns false).  The
                                         application owns the panel and the keys (its
                                         gUpdateDisplay -> GUI_DisplayScreen path
                                         draws, APP_TimeSlice10ms -> CheckKeys handles
                                         them, the menu view is built at boot), and
                                         EXIT switches VFO A/B because this radio has
                                         no A/B key.  See
                                         docs/ra89r_port.md for the layer table and what is
                                         next: mapping the stock's 32-byte general
                                         settings block (docs/ra89r_codeplug.md) into
                                         EEPROM_Config_t, so squelch, backlight and
                                         the power-on display follow the stock radio
driver/bk4829             RF          -- MERGED: the shared 3-wire bus, both
                                         transceivers, the stock register tables and
                                         the K1-compatible BK4819 interface.  Ids, all
                                         configuration writes, the frequency path and
                                         the RSSI response to a carrier are validated
                                         on the radio, see docs/ra89r_bk4829.md and
                                         docs/ra89r_bk4815.md
```

The open features have their own write-ups, and they are the places to start:

| feature | doc | state |
|---|---|---|
| keypad | `docs/ra89r_keypad.md` | done: 20 buttons, validated on the radio |
| backlight | `docs/ra89r_led.md` | done: GPIOA pin 5, confirmed on the radio |
| status LED | `docs/ra89r_led.md` | done: `PA13` red, `PA14` green, active high (measured); `driver/led.c`, 'L' |
| battery gauge | `docs/ra89r_battery.md` | protocol decoded and implemented; the chip never answers |
| beeper | `docs/ra89r_beeper.md` | done: `driver/beeper.c`, DAC tone on PA4/`DAC_OUT1` via TIM4 + DMA1 ch3; validated on the radio |
| EEPROM (SPI NOR) | `docs/ra89r_eeprom.md` | read + full dump validated on the radio; write test pending |
| RF transceivers | `docs/ra89r_bk4829.md`, `docs/ra89r_bk4815.md`, `docs/ra89r_rfpath.md` | done for the BK4829: ids, all writes, tuning and an RSSI response to a carrier validated on the radio; the BK4815's RF role is still open |
| transmit / PA | `docs/ra89r_rfpath.md` | done: voice heard on a second radio; `driver/pa.c` + `driver/tx.c`, with the register table in the doc |

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

The **battery gauge** and the **EEPROM** are the unfinished features, each parked
on its own branch with a doc: the gauge's protocol is decoded but the chip never
answers (`docs/ra89r_battery.md`), and the SPI NOR ("EEPROM") reads and dumps but
its write test has not run (`docs/ra89r_eeprom.md`).  The **RF transceivers** are
merged and validated on the radio: the bus, both parts and the K1-compatible
interface -- ids, all writes, tuning and an RSSI response to a carrier -- with the
BK4815's RF role still open, see `docs/ra89r_bk4829.md` and `docs/ra89r_bk4815.md`.
The **status LED** turned out to be MCU lines after all -- `PA13`/`PA14`, measured
on the radio (`docs/ra89r_led.md`).  **Transmit works**: the `driver/audiocontrol` work
is merged, and the transmit chain it measured -- the `0x36` PA-CTL and bias, the
`0x50` unmute, the `0x33` GPIO state, the band pins, the PB14/TIM1 bias PWM and the
microphone gain -- is now `driver/pa.c` and `driver/tx.c`, with voice heard on a
second receiver.  The **beeper** works too (`driver/beeper.c`): the stock's DAC
tone on PA4 = `DAC_OUT1`, played by TIM4 + DMA1 channel 3, with the K1's
`AUDIO_PlayBeep` routed through it instead of the RF chip's tone generator and
the amplifier raised around the beep -- validated on the radio
(`docs/ra89r_beeper.md`).

## Firmware / flashing

- **`0x0805FFF0` is the bootloader's *update-mode request*, not an
  application-valid flag** (docs/ra89r_bootloader.md §4c).  `0xFF` is the normal value
  and makes the bootloader start the application; `0x11` makes it enter update mode,
  and it consumes the request (writes `0xFF` back) on the way in.  The stock
  application sets `0x11` when the PC sends `Reset` + `'0'` over its serial command
  channel (0x08015710, then `SYSRESETREQ`) -- that is how the CPS reboots a *running*
  radio into the bootloader, and the key combination (`tools/FLASHING.md` §6) is
  the manual equivalent.  `tools/ra89r_flash.py` still writes `0x11` there by
  default, as it always has (`--no-valid-marker` skips it): the bootloader consumes
  the request on the next reset and starts the image, so the flash ends with the
  radio running and `0xFF` in flash either way.  `firmware/App/main.c` reports the
  byte at boot.  Nothing validates the image.
- **The bootloader also enters update mode when PB9 and PA2 are both low at reset**
  (docs/ra89r_bootloader.md §4c), whatever that byte says: the reset vector leads through
  the decision, and `EXIT` is a `SYSRESETREQ`, so every flash ends at it.  The launch
  trampoline (0x0800335A) only checks that vector[0] looks like SRAM, and the copy at
  0x08003382 -- what runs after update mode returns -- checks nothing at all.
- The firmware is linked for flash address **`0x08004000`** with a **368K** flash
  region: the stock bootloader at `0x08000000-0x08003FFF` must stay intact, and
  the stock application lives exactly where our image goes.
- Flashing goes through the stock bootloader over the programming port (USART1,
  PB6/PB7, from 9600 baud): see `docs/ra89r_bootloader.md`.  The bootloader itself
  refuses records in `0x08000000-0x08003FFF`; so does `tools/ra89r_flash.py`.
- **The bring-up firmware runs on the radio** (see the branch table above), so a
  bring-up or flashing change can be checked against real behaviour, not only the
  simulator.  Still missing: the documented **key combination for update mode**
  (`tools/FLASHING.md` §6, `docs/ra89r_bootloader.md` §8 point 1).
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
  firmware uses. Compare it against `docs/ra89r_lcd.md` before writing the RA89R panel
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

This also forces a correction in `docs/ra89r_bk4829.md`: register `0x33` is the
**chip's GPIO-output register**, not a band/filter bitfield.  Bits 0..6 are the
outputs in `0x40 >> pin` order and the paired bit `14 - n` is cleared per driven
pin, which is what made it look like a filter selector; the GPIOs do control
front-end paths, just not through a bitfield.

**2. Located in the stock image, by register.**  These were listed here as "not
visible in the stock image", which was wrong: the stock implements all of them and
they are simply found by taking each feature's register set from the K1 driver and
asking which stock functions write those registers.  `docs/ra89r_rffeatures.md` has the
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
unconfigured part read zero (docs/ra89r_bk4829.md).

That layer is validated on the radio as far as its own behaviour goes: the ids
answer, every configuration register stores what is written, it tunes 145.7500 MHz
exactly, and `0x67` responds to a carrier (236 keyed, 57 released, straddling the
stock's own squelch marks).  What that does *not* finish: the entry points the stock
image had not been asked about (AF, signalling, AGC, idle states) still run the K1's
sequences here.  `docs/ra89r_rffeatures.md` now locates the stock's own routines for
them, which gives each one a reference to compare against, but none of that has
been exercised on the radio either -- the `X`/`R`/`S` commands and the host test can
check registers and RSSI, not audio or signalling -- except for the AF and PA
halves, which are now measured on the radio: the K1 `SetAF`/`RX_TurnOn` sequence
produces audible receive audio, and the transmit chain is the one `driver/pa.c`
and `driver/tx.c` implement (voice heard on a second receiver).  The stock's
CTCSS, scramble, VOX and compander routines are still only *located*.  And one thing was deliberately
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
  clock, reset cause, update request) and then runs a command console: `i`
  diagnostics, `d` framebuffer dump as ASCII, `r`/`s` panel re-init variants
  (`r` = standard sequence, i.e. the bootloader-proven one **and the default**;
  `s` = the stock application's variant, 8 extra bytes), `v`/`V` contrast,
  `l` backlight on/off, `q` heartbeat on/off, plus `h`/`c`/`t`/`b`/`f`/`p` and
  the RF commands `R`/`W`/`X`/`K` (probe both parts, replay their boot config,
  read every configured register back, K1 bring-up).
- Console: USART1, PB6/PB7, **115200 8N1** — the same Kenwood jack the
  bootloader uses at 9600.  If the console is silent too, the application is not
  running; run `probe` (still in the bootloader at 9600?) and check the
  0x0805FFF0 byte the boot log prints.

## Reference inputs (also outside the tooling)

- `FIRMWARE_RA89R_20260203_V49.icf` (this directory, **gitignored / not tracked**)
  — the stock RA89R firmware,
  identical to the copy in `~/Repos/ra_re/`; treat both as read-only inputs and
  never edit the `.icf` in place.
- `Ra89G_R_UpDataFile20260401_V52_10W_Enable.icf` (this directory, **gitignored /
  not tracked**) — the RA89G
  V52 "10 W enable" distributor build of the same family; the second codec sample
  and the cross-check for anything the two models disagree on.
  Neither `.icf` is in the git history: the container is a vendor artifact, not a
  source, and `.gitignore` excludes `*.icf` so a decode cannot be committed by
  accident.
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
- The port runs the MCU at **48 MHz** (HSI x6, 1 flash wait; `BOARD_PLL_MUL` /
  `BOARD_FLASH_WS` in `firmware/App/board_pins.h`).  It once stayed at the 8 MHz
  reset default, which made every bit-banged bus ~6x slower than the K1 it was
  compared against -- suspect clock/bus timing before the algorithm when the
  port is slow.
- In the K1 app loop, nothing on the 10 ms slice may touch the external SPI NOR
  flash, and a timeout does not belong in a backlight fade; both mistakes
  shipped once and made the radio unusable.
- Hardware claims (which filter is active at a given frequency, what the panel
  controller really is) come from the radio, not from static analysis alone; don't
  state a hardware behaviour as verified unless it was observed on the device.
