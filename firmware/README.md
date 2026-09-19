# RA89R custom firmware (bring-up stage)

Minimal firmware for the Retevis RA89R: **screen + UART only**.  It exists to
prove the reverse-engineered hardware facts (see `../ra89r_lcd.md`,
`../ra89r_findings.md`) on real silicon before any radio functionality is
brought over from the UV-K1/K5V3 port tree
(`/home/gonzalo/Repos/uv-k1-k5v3-firmware-custom`).

| | |
|---|---|
| MCU | Puya PY32F403xD, Cortex-M4F, 384K flash, 64K SRAM |
| Clock | HSI, 8 MHz (the part's reset default; no PLL yet) |
| Placement | application linked at **0x08004000**; the stock bootloader keeps 0x08000000-0x08003FFF |
| Screen | 128x64 ST7565-family, **bit-banged**: SDA PB15, SCLK PA8, DC PA10, CS PA11, RST PA9 — *working on hardware* |
| Backlight | **GPIOA pin 1**, plain output, level 1 = on (the stock bootloader blinks it) |
| Console | **USART1 on PB6/PB7 (AF2)**, 115200 8N1 — the radio's programming port, *working on hardware* |
| Fonts | the stock 8x16 and 5x7 bitmaps, lifted byte-for-byte from the stock firmware |

## Layout

```
firmware/
  CMakeLists.txt, CMakePresets.json, cmake/arm-none-eabi-toolchain.cmake
  Core/                     MCU support: startup, system_py32f403.c, linker script
  App/
    main.c                  test card, ASCII-art dump, command console
    board.h                 RA89R pin map + clock facts (all from the RE)
    driver/gpio.{c,h}       register-level GPIO helpers (no HAL/LL)
    driver/uart.{c,h}       USART1 console + tiny printf
    driver/systick.{c,h}    millisecond time base
    driver/lcd_st7565.{c,h} bit-banged panel driver + 1 KB shadow framebuffer
    driver/font_8x16.c      generated from the stock image
    driver/font_5x7.c       generated from the stock image
  tools/extract_fonts.py    regenerate the font tables from a decoded .icf
```

`Core/startup_py32f403xx.s`, `Core/system_py32f403.c` and the linker script come
from the vendor SDK template in `../PY32F4xx_Firmware/Templates/PY32F403xx_Templates`
(two small edits: flash origin 0x08004000 / 368K, and `VECT_TAB_OFFSET 0x4000`).

## Build

Needs CMake >= 3.22, Ninja and an arm-none-eabi GCC.  The toolchain used so far
is the official ARM GNU toolchain 13.3.rel1 (the same version the port project
uses), installed at:

```
~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi
```

```sh
cd firmware
export ARM_TOOLCHAIN_ROOT=~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi
cmake --preset Debug          # or Release
cmake --build build/Debug
```

Outputs in `build/<preset>/`: `ra89r_fw.elf`, `ra89r_fw.bin` (raw image for
flashing), `ra89r_fw.hex`, `ra89r_fw.map` and a size report.  A Debug build is
about 7.5 KB of flash and 2.6 KB of RAM, so there is plenty of room.

If `arm-none-eabi-gcc` is on `PATH` you can drop `ARM_TOOLCHAIN_ROOT`.

## What you should see

The screen layout lives in `App/ui.c` and is hardware independent, so it can be
rendered on a PC before flashing anything:

```sh
gcc -std=c11 -I App -I App/driver -DLCD_HOST_TEST tools/preview.c App/ui.c \
    App/driver/lcd_st7565.c App/driver/font_8x16.c App/driver/font_5x7.c -o /tmp/preview
/tmp/preview            # ASCII art of the boot screen, status row, echo row, ...
```

After boot the panel shows a bordered test card (all drawn with the stock fonts
lifted from the radio):

| rows | content | font |
|---|---|---|
| 4-19 | `RA89R LCD` | 8x16 |
| 22-37 | `UART <baud>` | 8x16 |
| 40-46 | `UPT<seconds>`, redrawn once a second | 5x7 |
| 48-54 | `ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789` (clipped at the screen edge) | 5x7 |
| 56-62 | characters typed on the serial port | 5x7 |

Because the flasher ends with `EXIT`, the radio resets and the bootloader jumps
straight into the new application (it only checks that the app's initial stack
pointer looks like SRAM, which it does), so the card should appear within ~50 ms
of the flash finishing.

If instead you get:

* **nothing** -> try `v`/`V` (contrast) over the serial port, then `r` (re-init).
  The default init is the one the stock bootloader uses (`0x08002440`) and that
  bootloader demonstrably drives this panel, so a blank panel after that points
  at power/backlight or the wiring rather than the command sequence.  A SysTick
  failure can no longer hang the boot either (busy-loop fallback).
* **a picture shifted 4 px** -> set `LCD_COLUMN_OFFSET` to 0 in
  `App/driver/lcd_st7565.h`;
* **mirrored / garbled** -> the 0xA1 (SEG direction) / 0xC0 (COM direction) init
  bytes; try 0xA0 / 0xC8;
* **dark background** -> 0xA6 (display mode); try 0xA7;
* **a static card but no `UPT` ticking and no echo** -> the panel side is fine
  and the UART side is wrong (pins/baud) -- the two are independent, which
  isolates the fault.

## What it does

On reset it brings the **UART console up first** (so a dark panel is still
diagnosable), prints a boot log with the clock, reset cause, vector table,
app-valid marker and panel variant, then initialises the panel, paints the test
card and runs a command console.  Any Cortex-M fault prints the fault status and
the stacked registers on the console instead of dying silently
(`App/driver/fault.c`).

| key | action |
|---|---|
| `h` | help |
| `i` | info: clock, link address, pin map, contrast |
| `c` | clear the screen |
| `t` | redraw the test card |
| `d` | dump the framebuffer as ASCII art over the UART (verifies the panel image without a display) |
| `b` | toggle the border |
| `f` | checkerboard fill |
| `p` | animated bar on/off |
| `r` / `s` | re-init the panel: standard sequence (the bootloader's, now the default) / stock-app sequence (eight extra bytes) |
| `v` / `V` | contrast up / down (`0x81`, value) |
| `l` | backlight on/off (GPIOA pin 1) |
| `q` | heartbeat lines on the console (every 5 s) on/off |
| other printable keys | echoed to the UART and shown on the display |

The uptime counter in the corner proves the SysTick time base.

## Verified so far, and what is not

Verified by building and inspecting the image (no hardware run yet):

* links at 0x08004000 with the vector table first (SP `0x20010000`, reset in the
  app region), initial SP/RAM at the top of SRAM;
* the emitted init sequence byte-for-byte matches the stock firmware
  (`E2, A2, A1, C0, A6, F8 01, 2F, 25, 81 19, FF 64 72 B4 90 98 70 FE, 40, AF`),
  and the panel pin dance (BSRR `0x18` / BRR `0x28` on GPIOA/GPIOB) matches the
  reversed pin map;
* font tables are the stock bytes (regenerate with
  `python3 firmware/tools/extract_fonts.py work/FIRMWARE_RA89R_20260203_V49.bin`).

Not verified — needs the radio:

* whether the panel accepts this init/contrast and how the image is aligned
  (`LCD_COLUMN_OFFSET` is 4 in the stock driver and the UV-K1/K5V3 ST7565
  driver; set it to 0 if the picture is shifted);
* whether the programming port really is PB6/PB7 at 115200 (the stock
  *bootloader* configures USART1 on PB6/PB7; the baud it uses is 9600 at
  0x08000BC4, so try both);
* the display/backlight power rails — nothing here turns the backlight on
  (not mapped yet).

## Flashing — via the stock bootloader

The bootloader's serial protocol is documented in `../ra89r_bootloader.md` and
implemented in `../tools/ra89r_flash.py`.  Our image is linked exactly where the
stock application lives (0x08004000), so flashing replaces that region and
leaves the bootloader (0x08000000-0x08003FFF) intact.

```sh
# 1. wrap the built image into records the bootloader accepts
python3 ../ra89r.py mkicf build/Release/ra89r_fw.bin build/Release/ra89r_fw.icf

# 2. put the radio in update mode (the CPS's prompt tells you the key combo)

# 3. check that the bootloader is listening and see its identity
python3 ../tools/ra89r_flash.py --port /dev/ttyUSB0 probe

# 4. flash
python3 ../tools/ra89r_flash.py --port /dev/ttyUSB0 flash build/Release/ra89r_fw.icf

# to go back to stock
python3 ../tools/ra89r_flash.py --port /dev/ttyUSB0 flash ../FIRMWARE_RA89R_20260203_V49.icf
```

Add `--dry-run` to inspect every frame first (no port needed), `--baud keep`
to stay at 9600, `--erase-first` if a flash is refused.  The baud is picked
automatically (`--baud auto`): the bootloader tops out near 384 kbaud because it
runs on the 8 MHz reset clock, and 256000 has 0% divisor error.  Expect ~0.5 s
for this firmware and ~4-6 s for the full stock image (the flash erase/program
alone is ~0.5 s per 146 KB).  See `../ra89r_bootloader.md` section 6.

Still unverified without the radio: the update-mode key combination, and whether
the first `E2` record is accepted on the first try (the record encoding is proven
from the bootloader's own validator, but see open point 3 in
`../ra89r_bootloader.md`).

Fallback route: **SWD** on the PY32F403's SWD pins (PA13 SWDIO / PA14 SWCLK by
default) if the PCB exposes them -- that also gives a recovery path if a flash
goes wrong.

## Next steps

1. On hardware: enter update mode, run `probe`, then flash and confirm the panel
   init/contrast/alignment and the 115200 UART greeting.
2. Bring over the parts of the port tree that are MCU-agnostic (UI layer, fonts
   of the port's own size, settings/EEPROM) once the panel and console work.
3. Keep `../FIRMWARE_RA89R_20260203_V49.icf` (and the bootloader dump) safe as the
   way back to stock.
