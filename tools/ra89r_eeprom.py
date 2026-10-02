#!/usr/bin/env python3
"""Back up and restore the RA89R's external SPI NOR flash -- the storage the
CPS calls the EEPROM.

The firmware's console carries three commands for this (see
tools/ra89r_eeprom_sim.py for the exact framing):

    e               identify the chip (0x90 manufacturer/device id and the 0x9F
                    JEDEC id) and report its size
    E               stream the whole chip as raw binary, wrapped in
                        EEPROM DUMP <size>\\n ...<size bytes>... \\nEEPROM END <sum>\\n
    W <size> <sum>  restore the whole chip from raw binary, wrapped in
                        EEPROM RESTORE <size>\\n ...<size bytes>... \\nEEPROM RESTORE OK|FAIL <sum>\\n

This tool drives them over the console, so the chip's content can be saved
(`backup`, an alias of `dump`) and put back (`restore`).  The CPS offset map for
the contents is in ra89r_findings.md; the calibration window it must never lose
is in ra89r_codeplug.md.

    python3 tools/ra89r_eeprom.py --port /dev/ttyUSB0 id
    python3 tools/ra89r_eeprom.py --port /dev/ttyUSB0 backup eeprom.bin
    python3 tools/ra89r_eeprom.py --port /dev/ttyUSB0 restore eeprom.bin --yes

Needs pyserial.  A 2 MB part at 115200 takes a few minutes each way.
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
RESTORE = b"EEPROM RESTORE "


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


def read_until(ser, needle, timeout):
    """Read until `needle` appears, or time out.  Returns everything read."""
    buf = bytearray()
    deadline = time.time() + timeout
    while needle not in buf:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            deadline = time.time() + timeout
        elif time.time() > deadline:
            break
    return bytes(buf)


def read_match(ser, pattern, timeout):
    """Read until `pattern` matches the buffer.  Returns (buffer, match)."""
    buf = bytearray()
    deadline = time.time() + timeout
    while True:
        match = re.search(pattern, bytes(buf))
        if match:
            return bytes(buf), match
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            deadline = time.time() + timeout
        elif time.time() > deadline:
            return bytes(buf), None


def chip_size(ser, args):
    """Ask the firmware to identify the chip and return its byte count."""
    ser.reset_input_buffer()
    ser.write(b"e")
    text = read_until(ser, b"bytes", args.timeout)
    match = re.search(rb"eeprom:\s*(\d+)\s*bytes", text)
    if not match:
        sys.exit(f"no chip size in the report: {text!r}")
    return int(match.group(1))


def cmd_id(ser, args):
    ser.reset_input_buffer()
    ser.write(b"e")
    text = read_for(ser, args.timeout)
    sys.stdout.write(text.decode("latin-1"))
    return 0


def _read_dump(ser, args):
    """Send 'E' and return the chip's bytes, verifying the firmware checksum."""
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

    return data


def cmd_dump(ser, args):
    data = _read_dump(ser, args)
    with open(args.output, "wb") as fh:
        fh.write(data)
    print(f"wrote {len(data)} bytes to {args.output} "
          f"(checksum 0x{sum(data) & 0xFFFFFFFF:08X} ok)")
    return 0


def cmd_restore(ser, args):
    with open(args.image, "rb") as fh:
        data = fh.read()

    size = len(data)
    checksum = sum(data) & 0xFFFFFFFF

    if args.dry_run:
        print(f"dry run: chip is {chip_size(ser, args)} bytes; would send "
              f"'W {size} {checksum:08X}' and {size} bytes")
        return 0

    chip = chip_size(ser, args)
    if size != chip:
        sys.exit(f"refusing: image is {size} bytes but the chip is {chip} "
                 f"bytes")

    if not args.yes:
        sys.exit(f"refusing to overwrite the whole {size}-byte EEPROM without "
                 f"--yes")

    ser.reset_input_buffer()
    ser.write(f"W {size} {checksum:08X}\n".encode())

    header, match = read_match(ser, rb"EEPROM RESTORE ([^\n]*)\n", args.timeout)
    if not match:
        sys.exit(f"no restore header from the firmware: {header!r}")
    reply = match.group(1)
    if reply.startswith(b"ERR"):
        sys.exit(f"firmware refused the restore: {reply[4:].decode('latin-1')}")
    if int(reply) != size:
        sys.exit(f"firmware accepted {reply.decode()} bytes, not {size}")

    # The chip is written as the bytes arrive.
    ser.write(data)

    tail, match = read_match(ser, rb"EEPROM RESTORE (OK|FAIL) ([0-9A-Fa-f]{8})\n",
                             max(args.timeout, 30.0))
    if not match:
        sys.exit(f"restore ended without a verdict: {tail!r}")

    verdict = match.group(1)
    got = int(match.group(2), 16)
    if verdict != b"OK" or got != checksum:
        sys.exit(f"restore failed: firmware says {verdict.decode()} "
                 f"0x{got:08X}, host sent 0x{checksum:08X}")

    print(f"wrote {size} bytes (checksum 0x{checksum:08X} ok)")

    if args.verify:
        readback = _read_dump(ser, args)
        if readback != data:
            sys.exit("verify failed: the chip did not read back as written")
        print("verify: read-back matches")

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

    p_backup = sub.add_parser("backup", help="alias of dump")
    p_backup.add_argument("output", help="file to write")
    p_backup.set_defaults(func=cmd_dump)

    p_restore = sub.add_parser("restore", help="write a whole image back")
    p_restore.add_argument("image", help="file to restore (must match the chip size)")
    p_restore.add_argument("--yes", action="store_true",
                           help="confirm overwriting the whole chip")
    p_restore.add_argument("--verify", action="store_true",
                           help="re-dump afterwards and compare")
    p_restore.add_argument("--dry-run", action="store_true",
                           help="print the frames without writing anything")
    p_restore.set_defaults(func=cmd_restore)

    args = ap.parse_args()
    with open_port(args) as ser:
        return args.func(ser, args)


if __name__ == "__main__":
    sys.exit(main())
