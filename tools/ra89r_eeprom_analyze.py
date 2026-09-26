#!/usr/bin/env python3
"""Make sense of a dumped RA89R EEPROM image (the external SPI NOR flash).

    python3 tools/ra89r_eeprom.py --port /dev/ttyUSB0 dump eeprom.bin
    python3 tools/ra89r_eeprom_analyze.py eeprom.bin

The chip is a 2 MB Puya P25Q16.  What the CPS calls the EEPROM -- channels,
names, band ranges, settings, tone tables -- lives in the first few tens of KB;
the landmarks below come from the old CPS decompilation (see ra89r_findings.md,
"CPS (programming software)"), which was never re-verified, so treat them as
places to look, not as a settled layout.

Default output is an overview: the block structure, the landmark table and a
sample of the readable text.  Use --strings / --region / --blocks to go deeper.
"""

import argparse
import re
import sys
from collections import Counter

# Offsets the CPS uses, per ra89r_findings.md.  Names are the doc's.
LANDMARKS = [
    ("channel names", 4416),
    ("channels", 7936),
    ("band ranges", 8000),
    ("radio name", 8048),
    ("freq code", 8080),
    ("CTCSS/DCS", 8208),
    ("settings", 8224),
    ("DTMF", 8256),
    ("DTMF (2)", 8480),
    ("2-tone", 8544),
    ("5-tone", 8800),
    ("contacts", 10496),
    ("contacts (2)", 11520),
]

# Strings the first dump showed, used as anchor points to locate the tables.
ANCHORS = [b"RETEVISRA89R", b"RA89_Plus", b"TH-8600", b"TM-72A", b"VHF_UHF", b"CH-01"]

BLOCK = 4096


def load(path):
    with open(path, "rb") as fh:
        return fh.read()


def printable_runs(data, minimum):
    out = []
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minimum, data):
        out.append((m.start(), m.group().decode("latin-1")))
    return out


def hexdump(data, base, length, width=16):
    end = min(len(data), base + length)
    for off in range(base, end, width):
        chunk = data[off:off + width]
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        print(f"    {off:#08x}  {chunk.hex(' '):<47}  {text}")


def cmd_blocks(data):
    """Per-block byte statistics: erased, uniform and repeated blocks stand out."""
    print(f"block map ({BLOCK}-byte blocks, {len(data) // BLOCK} of them):")
    prev = None
    for off in range(0, len(data) - BLOCK + 1, BLOCK):
        block = data[off:off + BLOCK]
        counts = Counter(block)
        top, top_n = counts.most_common(1)[0]
        tag = ""
        if top_n == BLOCK:
            tag = f"ALL 0x{top:02X}"
        elif top_n > BLOCK * 3 // 4:
            tag = f"{top_n * 100 // BLOCK}% 0x{top:02X}"
        repeat = " == previous" if prev is not None and block == prev else ""
        if tag or repeat:
            print(f"  {off:#08x}  {tag}{repeat}")
        prev = block


def cmd_strings(data, minimum):
    for off, text in printable_runs(data, minimum):
        print(f"  {off:#08x}  {text}")


def cmd_region(data, offset, length):
    print(f"region {offset:#x} ({offset}), {length} bytes:")
    hexdump(data, offset, length)
    print("  next printable text:")
    for off, text in printable_runs(data[offset:offset + length * 4], 5)[:12]:
        print(f"    {offset + off:#08x}  {text}")


def cmd_overview(data):
    print(f"image {len(data)} bytes ({len(data) / 1024:.0f} KB)")
    if len(data) != 2 * 1024 * 1024:
        print("  note: a P25Q16 is 2 MB; this is a different size")

    erased = data.count(0xFF)
    print(f"  0xFF bytes: {erased} ({erased * 100 // max(len(data), 1)}%)")

    print("\nanchors seen in the first dump:")
    for needle in ANCHORS:
        at = data.find(needle)
        print(f"  {needle.decode():14s} {'not found' if at < 0 else hex(at)}")

    print("\nCPS landmarks (from the old CPS decompilation):")
    for name, off in LANDMARKS:
        chunk = data[off:off + 32]
        text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        print(f"  {off:>6}  {name:16s}  {chunk.hex(' ')}  {text}")

    print("\nfirst readable strings (>= 8 chars, first 40):")
    for off, text in printable_runs(data, 8)[:40]:
        print(f"  {off:#08x}  {text}")

    print("\nuse --blocks, --strings and --region to go further")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="file written by tools/ra89r_eeprom.py dump")
    ap.add_argument("--blocks", action="store_true", help="per-block byte statistics")
    ap.add_argument("--strings", nargs="?", type=int, const=6, default=None,
                    metavar="MIN", help="list printable runs (default min 6 chars)")
    ap.add_argument("--region", nargs=2, type=lambda v: int(v, 0), metavar=("OFF", "LEN"),
                    help="hexdump a region")
    ap.add_argument("--all", action="store_true", help="overview plus blocks plus strings")
    args = ap.parse_args()

    data = load(args.image)

    if args.all:
        cmd_overview(data)
        print()
        cmd_blocks(data)
        print()
        cmd_strings(data, 6)
        return 0
    if args.region:
        cmd_region(data, args.region[0], args.region[1])
        return 0
    if args.strings is not None:
        cmd_strings(data, args.strings)
        return 0
    if args.blocks:
        cmd_blocks(data)
        return 0
    cmd_overview(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
