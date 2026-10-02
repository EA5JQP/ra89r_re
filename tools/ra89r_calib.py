#!/usr/bin/env python3
"""Decode the RA89R's calibration window from an EEPROM dump.

The calibration is the factory RF alignment the stock firmware reads at boot.
It lives in the external SPI NOR flash ("the EEPROM") at `0x3000`-`0x38AF`,
inside the codeplug's first 16 KB, and must never be overwritten (see
docs/ra89r_calibration.md).  This tool prints what is there:

    python3 tools/ra89r_calib.py work/ra89r_eeprom.bin
    python3 tools/ra89r_calib.py work/ra89r_eeprom.bin --stock work/FIRMWARE_RA89R_20260203_V49.bin
    python3 tools/ra89r_calib.py work/ra89r_eeprom.bin --region 0x3000 256

The page boundaries and the CPS field labels come from the stock firmware's own
logical-address table (in the MCU image) and the CPS decompilation; the exact
byte-to-field mapping inside each page is *not* settled, so the per-page dump is
raw.  What is certain and what is inferred is recorded in docs/ra89r_calibration.md.
"""

import argparse
import sys

# The calibration window and the pages the stock firmware reads it through.
# `start`/`end` are EEPROM (logical) addresses; the names are the CPS's own
# page labels.  The boundaries are derived from the stock's page table
# (`ConTestTableAdd_CHG`) and the per-page lengths (`ConTableLength`).
CALIB_START = 0x3000
CALIB_END = 0x38A0

CALIB_PAGES = [
    (0x3000, 0x34C0, "power", "Freq + POW Low/Mid/Hig"),
    (0x34C0, 0x3520, "Rx tuning", "Freq + Adj"),
    (0x3520, 0x3710, "Tx modulation", "Freq + MOD trim/gain, MIC trim, limit"),
    (0x3710, 0x3860, "squelch", "Freq + W/N Sql1/Sql9"),
    (0x3860, 0x3870, "RSSI", "Freq + RSSI1/RSSI9"),
    (0x3870, 0x3880, "frequency calibration", "Freq + Fre Adj"),
    (0x3880, 0x38A0, "settings/test block", "48 bytes the stock parses at boot"),
]

# The stock firmware keeps the logical EEPROM address of every table in a table
# of 4-byte words in its own flash.  These are the ones this tool names.
STOCK_TABLE_BASE = 0x08025FD0
STOCK_TABLE_END = 0x080260A0
STOCK_TABLE_NAMES = {
    0x08025FD0: "channel names",
    0x08025FD4: "channel-used bitmap",
    0x08025FD8: "scan-allow bitmap",
    0x08025FDC: "band ranges",
    0x08025FE0: "intro lines",
    0x08025FE8: "programming password",
    0x08025FEC: "model name",
    0x08025FF4: "scan-range corners",
    0x08025FF8: "general settings block",
    0x08025FFC: "DTMF channels",
    0x08026000: "DTMF settings",
    0x08026010: "2-tone",
    0x08026014: "2-tone receive",
    0x08026018: "5-tone transmit",
    0x0802601C: "5-tone settings",
    0x08026020: "5-tone receive",
    0x08026024: "FM presets",
    0x08026028: "FM used bitmap",
    0x0802602C: "FM VFO",
    0x08026038: "contact names",
    0x0802603C: "contact ids",
    0x08026040: "calibration page pointer",
    0x08026044: "calibration page pointer",
    0x08026048: "calibration page pointer",
    0x0802604C: "calibration page pointer (squelch/RSSI)",
    0x08026050: "calibration page pointer",
    0x08026054: "calibration page pointer (Rx tuning)",
    0x08026058: "calibration page pointer (Tx modulation)",
    0x0802605C: "calibration page pointer (freq cal)",
    0x08026060: "calibration settings block",
}

# The stock reads the power table per band through these byte-offset tables in
# its own flash (FUN_08010550): addr = 0x3000 + off[band-1],
# len = off[band] - off[band-1].
POWER_OFFSET_TABLES = {
    "A": 0x080248E2,
    "B": 0x080248ED,
}


def hexdump(data, base, length, width=16):
    end = min(len(data), base + length)
    for off in range(base, end, width):
        chunk = data[off:off + width]
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        print(f"    {off:#06x}  {chunk.hex(' '):<47}  {text}")


def load(path):
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError as exc:
        sys.exit(f"cannot read {path}: {exc}")


def print_stock_table(stock):
    """Print the stock's logical-address table (the MCU image, base 0x08004000)."""
    base = 0x08004000
    print(f"stock logical-address table ({STOCK_TABLE_BASE:#x}):")
    for addr in range(STOCK_TABLE_BASE, STOCK_TABLE_END, 4):
        off = addr - base
        if off < 0 or off + 4 > len(stock):
            break
        value = int.from_bytes(stock[off:off + 4], "little")
        name = STOCK_TABLE_NAMES.get(addr, "")
        if name:
            print(f"  {addr:#010x} -> {value:#08x}  {name}")
    print()


def print_power_offsets(stock):
    """Print the byte-offset tables the stock reads the power table through.

    FUN_08010550 computes `addr = 0x3000 + off[i-1]`, `len = off[i] - off[i-1]`
    for a 1-based index; the tables are printed raw because the index's meaning
    (band or frequency row) is not settled -- see docs/ra89r_calibration.md.
    """
    base = 0x08004000
    for label, addr in POWER_OFFSET_TABLES.items():
        off = addr - base
        if off < 0 or off + 48 > len(stock):
            continue
        table = stock[off:off + 48]
        print(f"power offset table {label} ({addr:#x}, 48 bytes):")
        print("  " + " ".join(f"{b:02x}" for b in table))
        print()


def print_pages(data):
    print(f"calibration window {CALIB_START:#06x}-{CALIB_END - 1:#06x} "
          f"({CALIB_END - CALIB_START} bytes):")
    for start, end, name, fields in CALIB_PAGES:
        if start >= len(data):
            continue
        chunk = data[start:min(end, len(data))]
        nonff = sum(1 for b in chunk if b != 0xFF)
        print(f"\n  {start:#06x}-{end - 1:#06x}  {name:22s} "
              f"({nonff}/{len(chunk)} non-0xFF)")
        print(f"    CPS fields: {fields}")
        hexdump(data, start, min(end - start, 64))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="EEPROM dump (work/ra89r_eeprom.bin)")
    ap.add_argument("--stock", help="stock MCU image, for the address tables")
    ap.add_argument("--region", nargs=2, type=lambda v: int(v, 0),
                    metavar=("OFF", "LEN"), help="hexdump a region and stop")
    args = ap.parse_args()

    data = load(args.image)
    print(f"image {len(data)} bytes ({len(data) / 1024:.0f} KB)")

    if args.region:
        hexdump(data, args.region[0], args.region[1])
        return 0

    if args.stock:
        stock = load(args.stock)
        print_stock_table(stock)
        print_power_offsets(stock)

    print_pages(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
