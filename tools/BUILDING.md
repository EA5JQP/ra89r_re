# Building — firmware and host tools

Everything here builds from this repository; no vendor IDE, no Windows software.
Flashing the result is a separate document: [`FLASHING.md`](FLASHING.md).

Paths below are relative to the repository root.  The build itself lives in
`firmware/`; the Python tooling lives in `tools/`.

## 1. What needs to be installed

| need | why | this machine |
|---|---|---|
| `arm-none-eabi-gcc` (ARM GNU toolchain) | cross-compile the firmware | 13.3.rel1, at `~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi` — **not on `PATH`** |
| CMake >= 3.22 and Ninja | the firmware build | installed |
| `python3` | the `.icf` wrapper (a POST_BUILD step) and every host tool | installed |
| `gcc` (host) | the PC-side screen preview | installed |
| `pyserial` | only for flashing, not for building | `pip install pyserial` if missing |
| `capstone` | only for `tools/ra89r_analyze.py` | installed |

The toolchain is not on `PATH`, so point the CMake toolchain file at it with
`ARM_TOOLCHAIN_ROOT`, or pass the compiler explicitly
(`-DCMAKE_C_COMPILER=.../bin/arm-none-eabi-gcc`).  If `arm-none-eabi-gcc` *is* on
`PATH`, the variable can be omitted.

## 2. Firmware

```sh
cd firmware
export ARM_TOOLCHAIN_ROOT=~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi

cmake --preset Debug            # -Og -g3
cmake --build build/Debug

cmake --preset Release          # -Os, the one to flash
cmake --build build/Release
```

Or without presets: `cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug .`

The two configurations write to `firmware/build/Debug/` and
`firmware/build/Release/`:

| file | what it is |
|---|---|
| `ra89r_fw.elf` | ELF with symbols (the Debug build is the one to debug) |
| `ra89r_fw.bin` | raw image, linked at `0x08004000` |
| `ra89r_fw.hex` | Intel HEX |
| **`ra89r_fw.icf`** | **the file to flash** — records the stock bootloader accepts |
| `ra89r_fw.icf.meta` | sidecar so `tools/ra89r.py encode/decode` can re-wrap it |
| `ra89r_fw.map` | link map |

The build prints a size report; the bring-up firmware is only a few KB, and the
flash region is 368 KB (`0x08004000`-`0x0805FFFF`).

### How the `.icf` is produced

The link step emits `ra89r_fw.bin` first, then a POST_BUILD step runs

```sh
python3 tools/ra89r.py mkicf ra89r_fw.bin ra89r_fw.icf --base 0x08004000
```

so the wrapped image can never drift from the binary.  By hand, if python3 was
missing at configure time:

```sh
python3 tools/ra89r.py mkicf firmware/build/Release/ra89r_fw.bin \
    firmware/build/Release/ra89r_fw.icf --base 0x08004000
python3 tools/ra89r.py verify firmware/build/Release/ra89r_fw.icf
```

`verify` must print `baseline 0x66` and every record valid — that is the sanity
check before flashing.

### Base address

Nothing here is linked at `0x08000000`: the stock bootloader owns
`0x08000000`-`0x08003FFF` and the application starts at **`0x08004000`**.  That
is set in `firmware/Core/` (linker script origin/length and
`VECT_TAB_OFFSET 0x4000`), in the `mkicf --base` argument, and it must stay that
way or the flash overwrites the bootloader.

## 3. Screen layout on a PC (no radio, no cross toolchain)

`firmware/App/ui.c` is hardware-free and `firmware/tools/preview.c` renders the
same shadow framebuffer the target builds, as ASCII art — use it to check a
layout change instead of guessing:

```sh
cd firmware
gcc -std=c11 -I App -I App/driver -DLCD_HOST_TEST \
    tools/preview.c App/ui.c App/driver/lcd_st7565.c \
    App/driver/font_8x16.c App/driver/font_5x7.c -o /tmp/preview
/tmp/preview            # the boot test card, the 1 s status update, an echo
```

The font tables are lifted byte-for-byte from the stock firmware;
`firmware/tools/extract_fonts.py` regenerates them from a decoded image:

```sh
python3 tools/ra89r.py decode FIRMWARE_RA89R_20260203_V49.icf work/fw.bin
python3 firmware/tools/extract_fonts.py work/fw.bin
```

## 4. Python tooling

Stdlib only, except `tools/ra89r_analyze.py` (capstone) and
`tools/ra89r_flash.py` (pyserial).

```sh
python3 tools/ra89r.py verify  FIRMWARE_RA89R_20260203_V49.icf
python3 tools/ra89r.py decode  FIRMWARE_RA89R_20260203_V49.icf work/fw.bin
python3 tools/ra89r_analyze.py work/fw.bin          # -> work/analysis/

# codec round-trip: the fastest way to prove a decoder change did not break it
python3 tools/ra89r.py decode FW.icf work/fw.bin &&
python3 tools/ra89r.py encode work/fw.bin work/out.icf &&
cmp work/out.icf FW.icf
```

`tools/ra89r_flash.py` and `tools/ra89r_bootloader_sim.py` import `ra89r` from
their own directory, so run them from the repository root (or with `tools/` on
`PYTHONPATH`) — not by copying them elsewhere.

## 5. Troubleshooting the build

| symptom | fix |
|---|---|
| `PY32F403 SDK not found` | the `PY32F4xx_Firmware/` submodule is empty — `git submodule update --init --recursive` |
| `arm-none-eabi-gcc not found` | export `ARM_TOOLCHAIN_ROOT` (step 1) |
| `python3 not found` at configure time | the build still links, but no `.icf`: wrap it by hand with `tools/ra89r.py mkicf` |
| the host preview fails on `__DSB()` | it must be compiled with `-I App -I App/driver` from `firmware/` |
| link overflows flash | the region is 368 KB; check the size report and trim, do not move the base |
