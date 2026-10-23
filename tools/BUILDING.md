# Building — firmware and host tools

Everything here builds from this repository; no vendor IDE, no Windows software.
Flashing the result is a separate document: [`FLASHING.md`](FLASHING.md).

Paths below are relative to the repository root.  The build itself lives in
`firmware/`; the Python tooling lives in `tools/`.

## 1. What needs to be installed

| need | why | this machine (verified 2026-10-17) |
|---|---|---|
| `arm-none-eabi-gcc` (ARM GNU toolchain) | cross-compile the firmware | **13.3.rel1**, user-local at `~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi` — **not on `PATH`** |
| CMake ≥ 3.22 and Ninja | the firmware build | CMake 4.3.0 (system); Ninja **1.12.1** at `~/.local/bin/ninja` |
| `python3` | the `.icf` wrapper (a POST_BUILD step) and every host tool | system python3 **without pip**; `pip` installed user-level with `ensurepip`, pyserial/capstone in the user site |
| a host C compiler (`gcc`) | the PC-side preview/test binaries | system has **no gcc**; **Zig 0.13.0** at `~/.local/zig/zig` is used as `CC="zig cc -O2"` |
| `pyserial` | only for flashing, not for building | user site: `~/.local/lib/python3.14/site-packages` |
| `capstone` | only for `tools/ra89r_analyze.py` | user site: `~/.local/lib/python3.14/site-packages` |

The toolchain is not on `PATH`, so point the CMake toolchain file at it with
`ARM_TOOLCHAIN_ROOT`, or pass the compiler explicitly
(`-DCMAKE_C_COMPILER=…/bin/arm-none-eabi-gcc`).  If `arm-none-eabi-gcc` *is* on
`PATH`, the variable can be omitted.

### Installing the prerequisites (Fedora 44, no sudo)

Everything below is user-local because this machine has no sudo and no system
`gcc`/`ninja`; a normal `sudo dnf install gcc cmake ninja-build python3-pip`
(plus `pip install --user pyserial capstone`, and the Arm tarball) is the
simpler route when sudo is available.

```sh
# Ninja (CMake presets use the Ninja generator)
curl -fsSL -o /tmp/ninja.zip \
    https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip
unzip -oq /tmp/ninja.zip -d ~/.local/bin && chmod +x ~/.local/bin/ninja
export PATH="$HOME/.local/bin:$PATH"          # ~/.local/bin is on PATH here already

# ARM GNU toolchain 13.3.rel1 (the path BUILDING/AGENTS expect)
mkdir -p ~/Apps/toolchains
curl -fL -o /tmp/arm-toolchain.tar.xz \
    https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz
tar -xf /tmp/arm-toolchain.tar.xz -C ~/Apps/toolchains

# Python deps for the tools
python3 -m ensurepip --user                    # pip into ~/.local (no sudo)
python3 -m pip install --user pyserial capstone
#   the tools then run as plain `python3 tools/ra89r_flash.py ...`
#   (a venv works too: uv venv ~/.venvs/ra89r && uv pip install --python
#    ~/.venvs/ra89r/bin/python pyserial capstone)

# Host C compiler for the offline checks (system has no gcc)
curl -fL -o /tmp/zig.tar.xz \
    https://ziglang.org/download/0.13.0/zig-linux-x86_64-0.13.0.tar.xz
tar -xf /tmp/zig.tar.xz -C ~/.local && ln -sfn ~/.local/zig-linux-x86_64-0.13.0 ~/.local/zig
#   then:  CC="$HOME/.local/zig/zig cc -O2" tools/check_all.sh
#   (-O2 matters: Zig's default -O0 build of preview_k1 dies with SIGILL)
```

`PY32F4xx_Firmware/` is a submodule: `git submodule update --init` if it is
empty (a plain clone leaves it so).

## 2. Firmware

```sh
cd firmware
export ARM_TOOLCHAIN_ROOT=~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi

cmake --preset Debug            # -Og -g3
cmake --build build/Debug

cmake --preset Release          # -Os, the one to flash
cmake --build build/Release
```

Or without presets: `cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug .

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

The build prints a size report; a Debug build is ~84 KB of flash and ~2.6 KB of
RAM, and the flash region is 368 KB (`0x08004000`-`0x0805FFFF`).

**Verified on this machine (2026-10-17):** `cmake --preset Release &&
cmake --build build/Release` links `ra89r_fw` (102,348 bytes text) and prints
`51 records -> ra89r_fw.icf`; `tools/ra89r.py verify
firmware/build/Release/ra89r_fw.icf` then prints `baseline 0x66 -- all 51
records valid`.  (`cmake --preset Debug` needs its own `build/Debug` configure
first, as usual with presets.)

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

## 3. Host tools (no radio, no cross toolchain)

These compile with the host `gcc` and exercise the hardware-independent parts.
They are the fastest regression check for a change, and they are also how the
screen layout is checked without looking at the panel.

### Screen layout

```sh
cd firmware
gcc -std=c11 -I App -I App/driver -DLCD_HOST_TEST \
    tools/preview.c App/ui.c App/driver/lcd_st7565.c \
    App/driver/font_8x16.c App/driver/font_5x7.c -o /tmp/preview
/tmp/preview                # ASCII art of the boot/test screens
```

### All offline checks (the entry point)

```sh
cd firmware
CC="$HOME/.local/zig/zig cc -O2" tools/check_all.sh     # gcc may be used instead
```

It runs every host test below plus the target build when `ARM_TOOLCHAIN_ROOT`
is set, and ends with `== all checks done ==`.  On this machine it prints
`103 checks, 0 failed` for `test_rf`, `12` for `test_beeper`, `86` for
`test_bk1080`, `all scan_dual checks passed`, `all cat parser checks passed`,
and the K1 screen dump.  The `-O2` matters for Zig: its default `-O0` build of
`preview_k1` dies with SIGILL.

### RF register layers (individually)

```sh
cd firmware
gcc -std=c11 -I tools/host -I App -I App/driver tools/test_rf.c \
    App/driver/bk4829.c App/driver/bk4815.c App/driver/bk4819.c \
    App/driver/pa.c App/driver/tx.c tools/host/host_led.c -o /tmp/test_rf
/tmp/test_rf                # register tables, framing and the TX/PA order
```

(`driver/bk4819.c` mirrors the status LED through `driver/led.c`, which needs the
target's GPIO registers, so the host stand-in `tools/host/host_led.c` replaces
it.  `driver/pa.c` is linked for the `0x33` regression; `driver/tx.c` so
`tx_start()`'s register order can be asserted.)

### The ported K1 application (individually)

This links the whole app core, not just the screens: it decodes the codeplug the
host's RAM flash holds, round-trips the settings blob and drives the K1's own key
path, rendering the VFO and menu screens as ASCII.

```sh
cd firmware
gcc -std=c11 -I tools/host -I App -I App/driver \
    -DPY32F403xD -include App/k1_features.h -DST7565_HOST_TEST \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    tools/preview_k1.c tools/host/host_hw.c tools/host/host_bk4819.c \
    tools/host/host_beeper.c \
    App/ui/main.c App/ui/menu.c App/ui/ui.c App/ui/status.c App/ui/welcome.c \
    App/ui/battery.c App/ui/scanner.c App/ui/helper.c App/ui/inputbox.c \
    App/ui/fmradio.c \
    App/app/menu.c App/app/action.c App/app/app.c App/app/main.c \
    App/app/generic.c App/app/common.c App/app/chFrScanner.c App/app/dtmf.c \
    App/app/scanner.c App/app/fm.c App/radio.c App/functions.c App/audio.c \
    App/misc.c App/driver/py25q16.c \
    App/board.c App/settings.c App/version.c App/dcs.c App/frequencies.c \
    App/helper/battery.c App/helper/boot.c App/driver/system.c App/font.c \
    App/bitmaps.c App/driver/st7565.c App/driver/keyboard.c \
    App/driver/backlight.c App/driver/audio_path.c App/driver/scheduler.c \
    App/app/scan_dual.c \
    -o /tmp/preview_k1
/tmp/preview_k1
```

`tools/host/` is a stand-in for the vendor device header (CMSIS's `__DSB()` is
ARM assembly, so a PC build cannot use the real one) — see `NOTICE`.

## 4. Python tooling

Stdlib only, except the radio/analysis tools: `tools/ra89r_flash.py`,
`tools/ra89r_bootloader_sim.py`, `tools/rf_harness.py`,
`tools/ra89r_eeprom.py`, `tools/look4sat_cat.py` (pyserial) and
`tools/ra89r_analyze.py` (capstone).  On this machine those two are installed
in the user site (step 1), so plain `python3` runs everything:

```sh
python3 tools/ra89r.py verify  FIRMWARE_RA89R_20260203_V49.icf
python3 tools/ra89r.py decode  FIRMWARE_RA89R_20260203_V49.icf work/fw.bin
python3 tools/ra89r_analyze.py work/FIRMWARE_RA89R_20260203_V49.bin

# codec round-trip: the fastest way to prove a decoder change did not break it
python3 tools/ra89r.py decode FW.icf work/fw.bin &&
python3 tools/ra89r.py encode work/fw.bin work/out.icf &&
cmp work/out.icf FW.icf
```

(Or `source ~/.venvs/ra89r/bin/activate` once; `ra89r.py` itself is stdlib-only
and runs under the system python as well.)

`tools/ra89r_flash.py` and `tools/ra89r_bootloader_sim.py` import `ra89r` from
their own directory, so run them from the repository root (or with `tools/` on
`PYTHONPATH`) — not by copying them elsewhere.

## 5. Troubleshooting the build

| symptom | fix |
|---|---|
| `PY32F403 SDK not found` | the `PY32F4xx_Firmware/` submodule is empty — `git submodule update --init --recursive` |
| `arm-none-eabi-gcc not found` | export `ARM_TOOLCHAIN_ROOT` (step 1) |
| `ninja: command not found` | `~/.local/bin` is not on `PATH` — `export PATH="$HOME/.local/bin:$PATH"` |
| `ModuleNotFoundError: serial` (or `capstone`) | `python3 -m ensurepip --user && python3 -m pip install --user pyserial capstone` |
| `preview_k1` dies with SIGILL under Zig | build with `zig cc -O2` (Zig's `-O0` miscompiles it on this machine); gcc is unaffected |
| `python3 not found` at configure time | the build still links, but no `.icf`: wrap it by hand with `tools/ra89r.py mkicf` |
| `warning: python3 not found …` in the CMake output | same as above |
| the host previews fail on `__DSB()` | they must be compiled with `-I tools/host`, which shadows the vendor header |
| link overflows flash | the region is 368 KB; check the size report and trim, do not move the base |
