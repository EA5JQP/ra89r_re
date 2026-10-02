# Backing up and restoring the EEPROM

The radio keeps its codeplug, its settings and its **factory RF calibration** on
an external SPI NOR flash (the CPS calls it the EEPROM).  The calibration has no
defaults: erase it and the radio misbehaves.  Before changing anything on that
chip, take a full backup; this document is the procedure and the exact commands.

`tools/ra89r_eeprom.py` does the host side.  The firmware side is the
**`driver/eeprom` branch** — a small bring-up build whose only job is the EEPROM,
so recovery never depends on the application.  Its console commands:

| key | what |
|---|---|
| `e` | identify the chip and report its size |
| `E` | stream the whole chip as raw binary |
| `W` | restore the whole chip (wait for the host) |
| `Z` | one-shot write validation on an empty sector |

## 1. Build the backup firmware

Builds the `driver/eeprom` branch; needs the ARM GNU toolchain (see AGENTS.md).

```sh
cd firmware
export ARM_TOOLCHAIN_ROOT=~/Apps/toolchains/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi
cmake --preset Release && cmake --build build/Release
```

The file the bootloader accepts is `firmware/build/Release/ra89r_fw.icf`.  A
prepared copy of the current build is at `work/ra89r_eeprom_backup.icf`; rebuild
it from source rather than trusting a stale copy.

## 2. Flash it

Put the radio in update mode (see `firmware/FLASHING.md`), then one line flashes
the prepared backup firmware:

```sh
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 flash work/ra89r_eeprom_backup.icf
```

That file is the `driver/eeprom` build wrapped as an `.icf` (rebuild it with
step 1 if it is stale; the rebuilt file is
`firmware/build/Release/ra89r_fw.icf`).  Add `probe` in place of `flash` to
check the radio is in update mode first.  The radio reboots into the backup
firmware and its console answers at 115200 on the same Kenwood jack.

## 3. Back up the whole chip

```sh
python3 tools/ra89r_eeprom.py --port /dev/ttyUSB0 backup stock_eeprom.bin
```

That streams the whole 2 MB; at 115200 it takes a few minutes.  The tool checks
the firmware's checksum against its own and refuses to write the file if they
differ.  **Keep this file** — it is the recovery image.  Verify it is 2,097,152
bytes:

```sh
stat -c '%s bytes' stock_eeprom.bin
sha256sum stock_eeprom.bin
```

Optionally inspect the calibration window in it:

```sh
python3 tools/ra89r_calib.py stock_eeprom.bin --stock work/FIRMWARE_RA89R_20260203_V49.bin
```

## 4. Validate that writing works (once)

The write path has to be proven before a restore can be trusted.  This test finds
an **empty** sector in the erased tail (it refuses a sector that holds anything),
writes a pattern, reads it back, compares, then erases it again so the initial
value is restored.  It touches nothing that holds data, so it is safe; run it
once:

```sh
python3 tools/ra89r_eeprom.py --port /dev/ttyUSB0 writetest
```

It prints `EEPROM WRITETEST PASS` (or `FAIL` with the address that differed).
On the console the same thing is the `Z` key.  If it fails, do not restore —
the write path is not working yet.

The same firmware logic can be exercised on a PC before touching the radio:
`firmware/tools/test_eeprom_console.c` compiles `App/driver/eeprom_console.c`
against fake UART and SPI, and checks the restore round-trip, the size guard,
the checksum verdict and the empty-sector validation:

```sh
cd firmware && gcc -std=c11 -I App -I App/driver tools/test_eeprom_console.c \
    App/driver/eeprom_console.c -o /tmp/test_eeprom_console && /tmp/test_eeprom_console
```

## 5. Restore

```sh
python3 tools/ra89r_eeprom.py --port /dev/ttyUSB0 restore stock_eeprom.bin --yes --verify
```

* the image must be exactly the chip size, or the tool refuses;
* `--yes` is required — a restore overwrites the whole 2 MB;
* `--verify` re-dumps afterwards and compares, which catches a write that
  silently did not take;
* `--dry-run` prints what would be sent without touching the chip.

The firmware acks each 4 KB sector, so a long erase cannot lose the host's bytes;
a damaged transfer is reported as `EEPROM RESTORE FAIL`.

## 6. Going back to the application

Flashing the radio's normal firmware is independent of the EEPROM: the EEPROM
backup/restore never touches the MCU's flash.  To leave the backup firmware,
flash the stock image or your custom build:

```sh
python3 tools/ra89r_flash.py --port /dev/ttyUSB0 flash <application>.icf
```

## Safety rules

* The calibration window (`0x3000`-`0x38AF`) and the `TYTDXC` signature (`0x3FF0`)
  are the two regions no writer may assume it owns; see `docs/ra89r_codeplug.md`
  and `docs/ra89r_calibration.md`.
* A restore overwrites exactly what you give it — use a backup taken from this
  radio, not another unit's.
* Keep `stock_eeprom.bin` and its checksum somewhere outside the repo.
