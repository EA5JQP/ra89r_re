# Flashing the RA89R — step by step

Everything below is in this repository and needs no Windows software.  The
protocol is documented in `../ra89r_bootloader.md`.

**What gets written:** the application records, and nothing else.  The byte at
`0x0805FFF0` is the bootloader's *update-mode request*, not a validity flag:
`0xFF` (the normal value, and what a flash now leaves behind) makes it start the
application, and `0x11` tells it to enter update mode.  `ra89r_flash.py` writes
`0x11` there by default, as it always has: not for validity, but as an
update-mode request, which the bootloader consumes on the post-`EXIT` reset
before starting the image -- so it is harmless either way, and
`--no-valid-marker` skips it.  Everything stays inside
`0x08004000`-`0x0805FFFF`.
The stock bootloader (`0x08000000`-`0x08003FFF`) is never touched — `ra89r_flash.py`
refuses those addresses and the bootloader rejects them too.  A bad flash is
therefore recoverable: re-flash the stock image.

## 1. Hardware

* The radio, plus the **Kenwood-style programming cable** (the one the CPS uses).
  **USB-C cannot flash this radio** — it goes to the MCU's USB peripheral and
  nothing in the stock firmware enables it (`../ra89r_bootloader.md` §1).
* A 3.3 V USB-serial cable/adapter (CH340/FTDI/CP210x all work).  The radio's
  UART is 3.3 V; do not feed 5 V into the jack.

## 2. Software

Already present in this environment:

```sh
arm-none-eabi-gcc 13.3     # ~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi
cmake + ninja
python3 + pyserial
```

If pyserial is missing: `pip install pyserial` (or `python3-pyserial`).

## 3. Build (produces the flashable image)

```sh
cd firmware
export ARM_TOOLCHAIN_ROOT=~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi
cmake --preset Release
cmake --build build/Release
```

Outputs in `firmware/build/Release/`:

| file | what it is |
|---|---|
| `ra89r_fw.elf` | debug/symbols |
| `ra89r_fw.bin` | raw image (0x08004000…) |
| **`ra89r_fw.icf`** | **the file to flash** — records the bootloader accepts |
| `ra89r_fw.icf.meta` | sidecar for `ra89r.py encode/decode` |
| `ra89r_fw.map` | link map |

`ra89r_fw.icf` is generated automatically by the build.  Manually, if needed:

```sh
python3 ../ra89r.py mkicf build/Release/ra89r_fw.bin build/Release/ra89r_fw.icf --base 0x08004000
python3 ../ra89r.py verify build/Release/ra89r_fw.icf     # -> baseline 0x66, all N records valid
```

## 4. Back up what is on the radio

The stock firmware is already in this repository
(`FIRMWARE_RA89R_20260203_V49.icf`), and flashing it back is one command (step 8).
Worth doing as well, if you want a copy of the *current* contents:

```sh
python3 ../ra89r.py decode FIRMWARE_RA89R_20260203_V49.icf /tmp/stock_backup.bin
```

## 5. Connect and find the port

Plug the cable into the radio's jack (radio off) and the PC:

```sh
ls /dev/serial/by-id/                 # most stable naming
python3 -c "import serial.tools.list_ports as l; print([p.device for p in l.comports()])"
```

Use the resulting device, e.g. `/dev/ttyUSB0`.  If you get "permission denied",
add yourself to the serial group and re-login:

```sh
sudo usermod -aG dialout "$USER"      # then log out/in
```

## 6. Put the radio into update mode

This is the one thing the firmware cannot tell us: the bootloader samples board
signals at power-up to decide whether to run the application or stay in update
mode, but which physical key drives that line is not encoded in the image.

* **Try first:** with the radio **off**, hold the **side key** (the small
  top/side button, or PTT on some models) and switch it on.
* **Authoritative source:** open the stock CPS (`RA89R+V1.0`) and start its
  firmware-update function — it displays the key combination for this model.
  You do not have to let it flash; just read the instruction.
* Either way, confirm with `probe` (below): the bootloader announces itself; the
  running application answers differently, and `probe` says which one you got.

## 7. Probe, then flash

```sh
cd /home/gonzalo/Repos/ra89r_re

# 1) is the bootloader listening?
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 probe
#    exit 0 -> "bootloader detected -- safe to flash"
#    exit 2 -> "that is the application (...)": the radio is running normally,
#              go back to step 6
#    exit 1 -> nothing answered: cable/jack/power problem (see the hints)

# 2) optional: look at the exact frames without touching the radio
python3 tools/ra89r_flash.py --dry-run flash firmware/build/Release/ra89r_fw.icf

# 3) flash (picks the fastest baud automatically)
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 flash firmware/build/Release/ra89r_fw.icf
```

Expected output:

```
baseline 0x66, 4 records, 0x08004000..0x08005800
handshake (E0) at 9600 baud ...
  bootloader answered, identity 3836303042...
switching baud (index 8 = 1024000) ...
  talking at 256000 baud (index 6)
    1/4  0x08004000    2048 bytes
    ...
sending EXIT
done: 4 records, 7024 bytes in 0.6 s (...)
```

(the rate it settles on is whatever your adapter and the bootloader agree on;
**256000** is the expected value and the practical ceiling — the bootloader runs
on the 8 MHz reset clock, so 512000/1024000 cannot be generated and the link dies
until the radio is reset.  `--baud auto` starts at 256000 and walks down to 9600
if needed, so a failed switch cannot lose the session.)

## 8. After the flash

`EXIT` makes the bootloader reset the MCU; on reset it checks that the
application's initial stack pointer looks like SRAM and jumps into it, so the
new firmware starts by itself within ~50 ms.  The panel should show the test
card described in `README.md` ("What you should see").

Open a serial terminal on the same port at **115200 8N1** (that is the new
firmware's console — the bootloader used 9600, so switch after flashing) and
press `h` for the command list.

## 9. Going back to stock

```sh
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 flash FIRMWARE_RA89R_20260203_V49.icf
```

Then cycle the radio into update mode again, or just power-cycle it: the stock
application runs from `0x08004000` like ours does.

## Troubleshooting

| symptom | cause / fix |
|---|---|
| `probe` says **nothing answered** | cable in the wrong jack (must be the Kenwood one), radio not in update mode, port wrong, or the adapter's driver missing |
| `probe` says **that is the application** | the radio is running normally — enter update mode (step 6) |
| `record N ... was rejected by the bootloader` | the record failed the bootloader's checksum, or the link desynced; the tool resyncs and retries. If it keeps failing, try `--erase-first`, then `--baud 5` (115200) or `--baud keep` (9600) |
| `link lost (no answer to the resync handshake)` | power-cycle the radio back into update mode and re-run |
| `permission denied` | `usermod -aG dialout` (step 5) |
| flashing is slow | you are pinned to a low rate: `--baud auto` (default) is fastest; see `../ra89r_bootloader.md` §6 |
| screen blank after flashing | over the serial console send `v` a few times (contrast), then check the troubleshooting list in `README.md` |
| screen shows something but the console is silent | set the terminal to 115200 8N1; if still silent, the UART side needs checking (`firmware/App/board_pins.h`) |

## Doing it without a radio (dry run / regression)

The bootloader is also implemented as a test double, which is how the tool was
verified here:

```sh
python3 tools/ra89r_bootloader_sim.py --pty /tmp/ra89r-pty &
python3 tools/ra89r_flash.py --port $(cat /tmp/ra89r-pty) flash firmware/build/Release/ra89r_fw.icf
```
