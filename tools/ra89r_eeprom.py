#!/usr/bin/env python3
"""Dump the RA89R's external SPI NOR flash -- the storage the CPS calls the EEPROM.

The firmware's console carries two commands for this:

    e   identify the chip (0x90 manufacturer/device id and the 0x9F JEDEC id)
    E   stream the whole chip as raw binary, wrapped in
            EEPROM DUMP <size>\\n ...<size bytes>... \\nEEPROM END <checksum>\\n

This tool drives that over the console and saves the bytes, so the chip's content
can be inspected offline (the CPS offset map is in ra89r_findings.md).

    python3 tools/ra89r_eeprom.py --port /dev/ttyUSB0 id
    python3 tools/ra89r_eeprom.py --port /dev/ttyUSB0 dump eeprom.bin

Needs pyserial.  A 4 MB part at 115200 takes about six minutes.
"""

import argparse
import re
import sys
import time

try:
    import serial
except ImportError:                                     # pragma: no cover
    sys.exit("pyserial is required: pip install pyserial")

HEADER = b"EEPROM DUMP "
FOOTER = b"EEPROM END "


def open_port(args):
    return serial.Serial(args.port, args.baud, timeout=0.2)


def read_for(ser, seconds):
    """Read whatever arrives within `seconds`, extending on every byte seen."""
    buf = bytearray()
    deadline = time.time() + seconds
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            deadline = time.time() + seconds
    return bytes(buf)


def cmd_id(ser, args):
    ser.reset_input_buffer()
    ser.write(b"e")
    text = read_for(ser, args.timeout)
    sys.stdout.write(text.decode("latin-1"))
    return 0


def cmd_dump(ser, args):
    ser.reset_input_buffer()
    ser.write(b"E")

    # Sync on the header.  Anything before it is console echo or a refusal line.
    buf = bytearray()
    deadline = time.time() + args.timeout
    while HEADER not in buf:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            deadline = time.time() + 5.0
        elif time.time() > deadline:
            sys.stdout.write(buf.decode("latin-1"))
            sys.exit("no dump header arrived -- did the chip answer?  ('e' reports)")

    start = buf.index(HEADER) + len(HEADER)
    rest = bytes(buf[start:])
    while b"\n" not in rest:
        rest += ser.read(256)

    size_line, _, tail = rest.partition(b"\n")
    try:
        size = int(size_line.strip())
    except ValueError:
        sys.exit(f"bad size line from the firmware: {size_line!r}")

    data = bytearray(tail)
    last = time.time()
    while len(data) < size:
        chunk = ser.read(min(1 << 16, size - len(data)))
        if chunk:
            data += chunk
            last = time.time()
            if args.progress and len(data) % (256 * 1024) < len(chunk):
                sys.stderr.write(f"\r  {len(data) * 100 // size:3d}%  {len(data)}/{size}")
                sys.stderr.flush()
        elif time.time() - last > 10.0:
            sys.exit(f"\ntimed out after {len(data)} of {size} bytes")
    data = bytes(data[:size])
    if args.progress:
        sys.stderr.write("\r  100%  done            \n")

    foot = bytearray()
    deadline = time.time() + 10.0
    while FOOTER not in foot and time.time() < deadline:
        foot += ser.read(256)

    match = re.search(rb"EEPROM END ([0-9A-Fa-f]{8})", bytes(foot))
    if not match:
        sys.exit(f"dump ended without a footer: {bytes(foot)!r}")
    claimed = int(match.group(1), 16)
    actual = sum(data) & 0xFFFFFFFF
    if claimed != actual:
        sys.exit(f"checksum mismatch: firmware 0x{claimed:08X}, host 0x{actual:08X}")

    with open(args.output, "wb") as fh:
        fh.write(data)

    print(f"wrote {len(data)} bytes to {args.output} (checksum 0x{actual:08X} ok)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="console port, e.g. /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200, help="console baud (default 115200)")
    ap.add_argument("--timeout", type=float, default=3.0,
                    help="seconds to wait for a reply (default 3)")
    ap.add_argument("--no-progress", dest="progress", action="store_false",
                    help="do not print dump progress on stderr")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_id = sub.add_parser("id", help="identify the chip")
    p_id.set_defaults(func=cmd_id)

    p_dump = sub.add_parser("dump", help="stream the whole chip to a file")
    p_dump.add_argument("output", help="file to write")
    p_dump.set_defaults(func=cmd_dump)

    args = ap.parse_args()
    with open_port(args) as ser:
        return args.func(ser, args)


if __name__ == "__main__":
    sys.exit(main())
