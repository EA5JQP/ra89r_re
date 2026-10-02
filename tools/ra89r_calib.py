#!/usr/bin/env python3
"""Dump or decode the RA89R's calibration window.

Decode a captured image
-----------------------
The calibration is the factory RF alignment the stock firmware reads at boot.
It lives in the external SPI NOR flash ("the EEPROM") at `0x3000`-`0x38AF`,
inside the codeplug's first 16 KB, and must never be overwritten (see
docs/ra89r_calibration.md).  This prints what is there:

    python3 tools/ra89r_calib.py work/ra89r_eeprom.bin
    python3 tools/ra89r_calib.py work/ra89r_eeprom.bin --stock work/FIRMWARE_RA89R_20260203_V49.bin
    python3 tools/ra89r_calib.py work/ra89r_eeprom.bin --region 0x3000 256
    python3 tools/ra89r_calib.py calib_window.bin --base 0        # a window-only file

The page boundaries and the CPS field labels come from the stock firmware's own
logical-address table (in the MCU image) and the CPS decompilation; the exact
byte-to-field mapping inside each page is *not* settled, so the per-page dump is
raw.  What is certain and what is inferred is recorded in docs/ra89r_calibration.md.

Dump it from the radio (the stock's "mode 3")
---------------------------------------------
The manufacturer's CPS has a hidden alignment window; the radio side of it is
enabled by powering the radio on **holding `3`** (the boot key detector in
`FUN_08021AF8` sets the calibration flag `0x20009FA4`).  With the radio in that
state, the serial port speaks the CPS's protocol and this tool can read it:

    python3 tools/ra89r_calib.py probe  --port /dev/ttyUSB0
    python3 tools/ra89r_calib.py calib  --port /dev/ttyUSB0 calib.bin
    python3 tools/ra89r_calib.py full   --port /dev/ttyUSB0 eeprom.bin
    python3 tools/ra89r_calib.py write  --port /dev/ttyUSB0 calib.bin --yes
    python3 tools/ra89r_calib.py self-test

Protocol (from `AdjWin.cs` and the stock firmware): 57600 8N1.  A 10-byte read
command

    57 FF FF 06 <addr:4 big-endian> <len:2 big-endian>

is answered by the 5 header bytes `52 00 FF xx xx` followed by `<len>` data
bytes (the CPS's `ConReadDatAckCom` / `GetDataTxRxBytePro = 5 + len`).  The CPS
reads `0x3000`-`0x3880` in 512-byte chunks plus 48 bytes at `0x3880`.

The command's 4-byte address field is **only good for the first 64 KB**: the
stock computes the physical address as `(A >> 16) + (A & 0xFFFF)`, i.e. it adds
the high byte instead of shifting it into bit 16.  The calibration and the
codeplug are below `0x10000` and dump exactly; `full` therefore defaults to
`0x0`-`0x10000`.  The rest of the chip has to come from the custom firmware
(`tools/ra89r_eeprom.py`, console `E`).

`write` restores the window from a file (`calib`'s output) using the same link:
`57 FF 00 0A <addr:4 BE> <len:2 BE> <data>`, answered by the 4-byte ACK
`52 00 41 00`.  It goes through the stock's journal, so it is slower than a
read, and it **changes the factory RF alignment** -- it refuses to run without
`--yes`, stays inside the calibration window unless `--force` is given, and can
`--verify` by reading the window back.

Needs pyserial for the UART verbs (`pip install pyserial`).
"""

import argparse
import sys
import time

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

# ---------------------------------------------------------------------------
# The stock's mode-3 ("test mode") serial protocol, from AdjWin.cs.
# ---------------------------------------------------------------------------
MODE3_BAUD = 57600
ENTER_TEST = bytes.fromhex("FEFEEEEFF026980000000000FD")   # ConEntTestCom
EXIT_TEST = bytes.fromhex("FEFEEEEFF126980000000000FD")    # ConExitTestCom
READ_CMD = bytes.fromhex("57FFFF06")                       # ConTxReadCom
READ_ACK = bytes.fromhex("5200FF")                         # ConReadDatAckCom
READ_HEADER = 5                                            # READ_ACK + 2 bytes
WRITE_CMD = bytes.fromhex("57FF000A")                      # ConTxWriteCom + header byte
WRITE_ACK = bytes.fromhex("52004100")                      # ConAckCom
EEPROM_SIZE = 0x200000                                     # PY25Q16HB, 2 MB
MODE3_MAX_ADDR = 0x10000   # the stock mis-addresses reads >= this; only the
                           # first 64 KB is trustworthy (docs/ra89r_calibration.md)


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


def print_pages(data, base=0x3000):
    """Print the calibration pages.

    `base` is the file offset at which the calibration window starts: 0x3000 for
    a whole-chip dump, 0 for a file written by `calib`.
    """
    print(f"calibration window 0x{CALIB_START:04x}-0x{CALIB_END - 1:04x} "
          f"({CALIB_END - CALIB_START} bytes) at file offset {base:#06x}:")
    for start, end, name, fields in CALIB_PAGES:
        rel = base + (start - CALIB_START)
        n = end - start
        chunk = data[rel:rel + n]
        if not chunk:
            continue
        nonff = sum(1 for b in chunk if b != 0xFF)
        print(f"\n  {start:#06x}-{end - 1:#06x}  {name:22s} "
              f"({nonff}/{len(chunk)} non-0xFF)")
        print(f"    CPS fields: {fields}")
        hexdump(data, rel, min(n, 64))


# ---------------------------------------------------------------------------
# Mode-3 UART client
# ---------------------------------------------------------------------------

def build_read_frame(addr, length):
    """The 10-byte `57 FF FF 06 <addr:4 BE> <len:2 BE>` read command."""
    if not 0 <= addr <= 0xFFFFFFFF:
        raise ValueError("address out of range")
    if not 0 < length <= 0xFFFF:
        raise ValueError("length must be 1..65535")
    return READ_CMD + addr.to_bytes(4, "big") + length.to_bytes(2, "big")


def build_write_frame(addr, data):
    """The `57 FF 00 0A <addr:4 BE> <len:2 BE> <data>` write command."""
    data = bytes(data)
    if not 0 <= addr <= 0xFFFFFFFF:
        raise ValueError("address out of range")
    if not 0 < len(data) <= 0xFFFF:
        raise ValueError("length must be 1..65535")
    return WRITE_CMD + addr.to_bytes(4, "big") + len(data).to_bytes(2, "big") + data


class Mode3:
    """Reads the stock radio's calibration/EEPROM over the mode-3 serial link.

    `transport` only needs `reset()`, `write(bytes)` and `read(n)`; that keeps
    the protocol testable without hardware (see FakeTransport).
    """

    def __init__(self, transport, timeout=3.0, log=None):
        self.transport = transport
        self.timeout = timeout
        self.log = log or (lambda *a: None)

    def enter_test_mode(self):
        """Send the CPS's enter-test-mode frame (the radio may ignore it)."""
        self.log(f"enter test mode: {ENTER_TEST.hex(' ')}")
        self.transport.reset()
        self.transport.write(ENTER_TEST)
        time.sleep(0.2)
        self.transport.reset()

    def exit_test_mode(self):
        self.log(f"exit test mode: {EXIT_TEST.hex(' ')}")
        self.transport.write(EXIT_TEST)

    def _read_reply(self, length):
        """Read one reply: [echo] + `52 00 FF xx xx` + `length` data bytes.

        Resyncs on the header, so a command echo (or any leading noise) is
        tolerated.  Returns the data, or None on timeout/short reply.
        """
        want = READ_HEADER - len(READ_ACK) + length   # 2 header bytes + data
        buf = bytearray()
        deadline = time.time() + self.timeout
        while READ_ACK not in buf:
            if time.time() > deadline:
                return None
            chunk = self.transport.read(256)
            if chunk:
                buf += chunk
                deadline = time.time() + self.timeout
        rest = bytes(buf[buf.index(READ_ACK) + len(READ_ACK):])
        while len(rest) < want:
            if time.time() > deadline:
                break
            chunk = self.transport.read(min(4096, want - len(rest)))
            if chunk:
                rest += chunk
                deadline = time.time() + self.timeout
        if len(rest) < want:
            return None
        return rest[2:2 + length]

    def read(self, addr, length):
        frame = build_read_frame(addr, length)
        self.log(f"read 0x{addr:06x} len {length}: {frame.hex(' ')}")
        self.transport.reset()
        self.transport.write(frame)
        return self._read_reply(length)

    def read_region(self, addr, length, chunk=512, progress=False):
        """Read `length` bytes from `addr` in `chunk`-sized commands.

        Returns `(data, next_addr)`: `next_addr == addr + length` on success,
        otherwise the address at which the radio stopped answering (the partial
        data is still returned so a failed `full` dump is not lost).
        """
        out = bytearray()
        cur = addr
        end = addr + length
        while cur < end:
            n = min(chunk, end - cur)
            data = self.read(cur, n)
            if data is None:
                return bytes(out), cur
            out += data
            cur += len(data)
            if progress:
                sys.stderr.write(f"\r  {cur - addr:8d}/{length} "
                                 f"({(cur - addr) * 100 // length:3d}%) "
                                 f"0x{cur:06x}")
                sys.stderr.flush()
        if progress:
            sys.stderr.write("\r  " + " " * 40 + "\r")
            sys.stderr.flush()
        return bytes(out), cur

    def _read_ack(self):
        """Wait for the 4-byte write ACK `52 00 41 00`; None on timeout."""
        buf = bytearray()
        deadline = time.time() + self.timeout
        while WRITE_ACK not in buf:
            if time.time() > deadline:
                return None
            chunk = self.transport.read(256)
            if chunk:
                buf += chunk
                deadline = time.time() + self.timeout
        return WRITE_ACK

    def write(self, addr, data):
        frame = build_write_frame(addr, data)
        self.log(f"write 0x{addr:06x} len {len(data)}: {frame[:10].hex(' ')} + data")
        self.transport.reset()
        self.transport.write(frame)
        return self._read_ack() is not None

    def write_region(self, addr, data, chunk=512, progress=False):
        """Write `data` at `addr` in `chunk`-sized commands.

        Returns `(written, next_addr)`: `written == len(data)` on success,
        otherwise the number of bytes written before the radio stopped acking.
        """
        off = 0
        cur = addr
        total = len(data)
        while off < total:
            n = min(chunk, total - off)
            if not self.write(cur, data[off:off + n]):
                return off, cur
            off += n
            cur += n
            if progress:
                sys.stderr.write(f"\r  {off:8d}/{total} "
                                 f"({off * 100 // total:3d}%) 0x{cur:06x}")
                sys.stderr.flush()
        if progress:
            sys.stderr.write("\r  " + " " * 40 + "\r")
            sys.stderr.flush()
        return off, cur

    def raw_exchange(self, frame, timeout=None):
        """Send `frame` and return everything that arrives (for `probe`)."""
        self.transport.reset()
        self.transport.write(frame)
        buf = bytearray()
        deadline = time.time() + (timeout or self.timeout)
        while time.time() < deadline:
            chunk = self.transport.read(256)
            if chunk:
                buf += chunk
                deadline = time.time() + (timeout or self.timeout)
        return bytes(buf)


class SerialTransport:
    def __init__(self, port, baud, timeout=0.2):
        try:
            import serial
        except ImportError:
            sys.exit("pyserial is required for the UART verbs: pip install pyserial")
        self._ser = serial.Serial(port, baud, timeout=timeout)

    def reset(self):
        self._ser.reset_input_buffer()

    def write(self, data):
        self._ser.write(data)

    def read(self, n):
        return self._ser.read(n)

    def close(self):
        self._ser.close()


class FakeTransport:
    """Emulates the radio's mode-3 replies from an in-memory image (offline tests)."""

    def __init__(self, memory, echo=False):
        self.memory = memory
        self.echo = echo
        self.out = bytearray()

    def reset(self):
        self.out.clear()

    def write(self, data):
        data = bytes(data)
        if data == ENTER_TEST:
            self.out += bytes.fromhex("52004100")
        elif data[:len(WRITE_CMD)] == WRITE_CMD and len(data) >= 10:
            addr = int.from_bytes(data[4:8], "big")
            length = int.from_bytes(data[8:10], "big")
            payload = data[10:10 + length]
            if self.echo:
                self.out += data
            self.memory[addr:addr + length] = payload
            self.out += WRITE_ACK
        elif data[:len(READ_CMD)] == READ_CMD and len(data) == 10:
            addr = int.from_bytes(data[4:8], "big")
            length = int.from_bytes(data[8:10], "big")
            payload = self.memory[addr:addr + length]
            if self.echo:
                self.out += data
            self.out += READ_ACK + b"\x00\x00" + payload

    def read(self, n):
        chunk = bytes(self.out[:n])
        del self.out[:n]
        return chunk

    def close(self):
        pass


# ---------------------------------------------------------------------------
# Decode path
# ---------------------------------------------------------------------------

def decode_main(argv):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="EEPROM dump (work/ra89r_eeprom.bin)")
    ap.add_argument("--stock", help="stock MCU image, for the address tables")
    ap.add_argument("--base", type=lambda v: int(v, 0), default=CALIB_START,
                    help="file offset of the calibration window (default 0x3000; "
                         "use 0 for a window written by `calib`)")
    ap.add_argument("--region", nargs=2, type=lambda v: int(v, 0),
                    metavar=("OFF", "LEN"), help="hexdump a region and stop")
    args = ap.parse_args(argv)

    data = load(args.image)
    print(f"image {len(data)} bytes ({len(data) / 1024:.0f} KB)")

    if args.region:
        hexdump(data, args.region[0], args.region[1])
        return 0

    if args.stock:
        stock = load(args.stock)
        print_stock_table(stock)
        print_power_offsets(stock)

    print_pages(data, args.base)
    return 0


# ---------------------------------------------------------------------------
# Radio path
# ---------------------------------------------------------------------------

def _open_radio(args):
    if not args.port:
        sys.exit("--port is required (e.g. --port /dev/ttyUSB0)")
    transport = SerialTransport(args.port, args.baud)
    radio = Mode3(transport, timeout=args.timeout, log=lambda *a: None)
    return radio


def _enter(radio, args):
    if args.enter:
        radio.enter_test_mode()


def _finish(radio, args):
    if args.exit:
        radio.exit_test_mode()


def cmd_probe(radio, args):
    _enter(radio, args)
    frame = build_read_frame(0x3000, 16)
    print(f"TX {frame.hex(' ')}")
    reply = radio.raw_exchange(frame)
    if not reply:
        print("no reply -- is the radio powered on holding '3'?  (mode 3)")
        return 1
    print(f"RX {len(reply)} bytes: {reply.hex(' ')}")
    if READ_ACK in reply:
        print("header 52 00 FF seen: the read protocol answered.")
    else:
        print("no 52 00 FF header: unexpected framing.")
    return 0


def _dump(radio, args, start, length, what):
    if args.dry_run:
        if args.enter:
            print(f"enter test mode: {ENTER_TEST.hex(' ')}")
        print(f"read {what}: 0x{start:06x}..0x{start + length:06x} "
              f"({length} bytes, {args.chunk}-byte chunks)")
        print(f"  first frame: {build_read_frame(start, min(args.chunk, length)).hex(' ')}")
        n = (length + args.chunk - 1) // args.chunk
        if n > 1:
            print(f"  ... {n} chunks in total")
        if args.exit:
            print(f"exit test mode: {EXIT_TEST.hex(' ')}")
        return 0

    _enter(radio, args)
    print(f"reading {what}: 0x{start:06x}..{start + length:06x} "
          f"({length} bytes, {args.chunk}-byte chunks)")
    data, stopped = radio.read_region(start, length, args.chunk, progress=True)
    _finish(radio, args)

    with open(args.output, "wb") as fh:
        fh.write(data)

    complete = stopped >= start + length
    if complete:
        print(f"wrote {len(data)} bytes to {args.output}")
    else:
        print(f"RADIO STOPPED ANSWERING at 0x{stopped:06x} "
              f"({len(data)} of {length} bytes); wrote the partial file")
    return 0 if complete else 2


def cmd_calib(radio, args):
    start = args.start if args.start is not None else CALIB_START
    length = args.length if args.length is not None else (CALIB_END - CALIB_START)
    rc = _dump(radio, args, start, length, "calibration")
    if rc == 0 and not args.dry_run:
        data = load(args.output)
        print()
        print_pages(data, base=0)
    return rc


def cmd_full(radio, args):
    start = args.start if args.start is not None else 0
    length = args.length if args.length is not None else MODE3_MAX_ADDR
    if start + length > MODE3_MAX_ADDR and not args.dry_run:
        print(f"warning: the stock mis-addresses reads at/above "
              f"0x{MODE3_MAX_ADDR:06x}; bytes past 0x{MODE3_MAX_ADDR:06x} are "
              f"NOT trustworthy.  Use tools/ra89r_eeprom.py on the custom "
              f"firmware for the whole chip.")
    return _dump(radio, args, start, length, "readable EEPROM")


def cmd_write(radio, args):
    data = load(args.file)
    if not data:
        sys.exit("refusing to write an empty file")
    start = args.start if args.start is not None else CALIB_START
    if args.length is not None and args.length != len(data):
        sys.exit(f"--length {args.length} does not match the file ({len(data)} bytes)")
    end = start + len(data)

    if args.dry_run:
        if args.enter:
            print(f"enter test mode: {ENTER_TEST.hex(' ')}")
        print(f"write {len(data)} bytes to 0x{start:06x}..0x{end:06x} "
              f"({args.chunk}-byte chunks)")
        print(f"  first frame: "
              f"{build_write_frame(start, data[:min(args.chunk, len(data))])[:10].hex(' ')} + data")
        n = (len(data) + args.chunk - 1) // args.chunk
        if n > 1:
            print(f"  ... {n} chunks in total")
        if args.exit:
            print(f"exit test mode: {EXIT_TEST.hex(' ')}")
        return 0

    if not args.yes:
        sys.exit("refusing to write the factory calibration without --yes")
    if (start < CALIB_START or end > CALIB_END) and not args.force:
        sys.exit(f"0x{start:06x}..0x{end:06x} is outside the calibration window "
                 f"0x{CALIB_START:06x}-0x{CALIB_END - 1:06x}; use --force to override")

    print(f"WARNING: writing {len(data)} bytes of factory RF alignment to "
          f"0x{start:06x}..0x{end:06x}")
    _enter(radio, args)
    written, stopped = radio.write_region(start, data, args.chunk, progress=True)
    if written < len(data):
        print(f"\nwrite stopped at 0x{stopped:06x} ({written} of {len(data)} bytes)")
        _finish(radio, args)
        return 2

    if args.verify:
        print(f"verifying 0x{start:06x}..0x{end:06x} ...")
        back, _ = radio.read_region(start, len(data), args.chunk, progress=True)
        if back != data:
            bad = next((i for i in range(min(len(back), len(data)))
                        if back[i] != data[i]), None)
            where = f"0x{start + bad:06x}" if bad is not None else "length mismatch"
            print(f"\nVERIFY FAILED at {where}")
            _finish(radio, args)
            return 3
        print("verified: the radio reads back what was written")

    _finish(radio, args)
    print(f"wrote {len(data)} bytes to 0x{start:06x}..0x{end:06x}")
    return 0


# ---------------------------------------------------------------------------
# Offline self-test
# ---------------------------------------------------------------------------

def self_test():
    memory = bytes((i * 7 + 3) & 0xFF for i in range(0x4000))

    # 1. frame encoding matches the CPS's StrHexAutoAdd0Pro construction
    assert build_read_frame(0x3000, 512) == bytes.fromhex("57FFFF06000030000200")
    assert build_read_frame(0x1234, 48) == bytes.fromhex("57FFFF06000012340030")

    # 2. a clean reply is parsed, including its 2 unknown header bytes
    radio = Mode3(FakeTransport(memory), timeout=1.0)
    assert radio.read(0x3000, 16) == memory[0x3000:0x3010]

    # 3. a command echo before the header is tolerated
    radio = Mode3(FakeTransport(memory, echo=True), timeout=1.0)
    assert radio.read(0x3100, 64) == memory[0x3100:0x3140]

    # 4. a region is assembled from chunks, including a partial last chunk
    radio = Mode3(FakeTransport(memory), timeout=1.0)
    data, stopped = radio.read_region(0x3000, 0x8A0, chunk=512)
    assert stopped == 0x3000 + 0x8A0
    assert data == memory[0x3000:0x3000 + 0x8A0]

    # 5. silence stops the dump and returns the partial data
    class Silent(FakeTransport):
        def write(self, data):
            pass

    radio = Mode3(Silent(memory), timeout=0.2)
    data, stopped = radio.read_region(0x3000, 0x1000, chunk=512)
    assert data == b"" and stopped == 0x3000

    # 6. write frame encoding matches the CPS's ConTxWriteCom construction
    assert build_write_frame(0x3000, b"\xaa\xbb") == bytes.fromhex("57FF000A000030000002aabb")

    # 7. a write is applied and the ACK resyncs past an echo
    mem = bytearray(memory)
    radio = Mode3(FakeTransport(mem, echo=True), timeout=1.0)
    assert radio.write(0x3000, b"\x11\x22\x33\x44")
    assert bytes(mem[0x3000:0x3004]) == b"\x11\x22\x33\x44"

    # 8. write_region chunks, mutates the image and reports the stop address
    mem = bytearray(memory)
    radio = Mode3(FakeTransport(mem), timeout=1.0)
    payload = bytes((i * 3 + 1) & 0xFF for i in range(0x300))
    n, stopped = radio.write_region(0x3000, payload, chunk=128)
    assert n == len(payload) and stopped == 0x3300
    assert bytes(mem[0x3000:0x3300]) == payload

    print("self-test: ok (read/write frames, reply/ack parsing, echo resync, "
          "chunking, timeout)")
    return 0


# ---------------------------------------------------------------------------

RADIO_VERBS = {"probe", "calib", "dump", "full", "write", "self-test"}


def radio_main(argv):
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--port", help="serial port, e.g. /dev/ttyUSB0")
    common.add_argument("--baud", type=int, default=MODE3_BAUD,
                        help=f"baud (default {MODE3_BAUD})")
    common.add_argument("--chunk", type=int, default=512,
                        help="read chunk size in bytes (default 512)")
    common.add_argument("--timeout", type=float, default=3.0,
                        help="seconds to wait for each reply (default 3)")
    common.add_argument("--enter-test-mode", dest="enter", action="store_true",
                        default=True, help="send the F0 enter frame (default)")
    common.add_argument("--no-enter-test-mode", dest="enter", action="store_false",
                        help="do not send the F0 enter frame")
    common.add_argument("--exit-test-mode", dest="exit", action="store_true",
                        default=False, help="send the F1 exit frame at the end")
    common.add_argument("--dry-run", action="store_true",
                        help="print the frames without opening the port")
    common.add_argument("--start", type=lambda v: int(v, 0), default=None,
                        help="override the start address")
    common.add_argument("--length", type=lambda v: int(v, 0), default=None,
                        help="override the length")

    ap = argparse.ArgumentParser(
        prog="ra89r_calib.py",
        description="Dump the RA89R calibration / EEPROM over the mode-3 UART.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("probe", parents=[common],
                   help="one 16-byte read; diagnose framing/baud/mode")
    p_calib = sub.add_parser("calib", parents=[common],
                             help="read 0x3000-0x38A0 (calibration + test block)")
    p_calib.add_argument("output", help="file to write")
    p_dump = sub.add_parser("dump", parents=[common], help="alias of `calib`")
    p_dump.add_argument("output", help="file to write")
    p_full = sub.add_parser("full", parents=[common],
                            help="read the first 64 KB (the stock cannot address higher)")
    p_full.add_argument("output", help="file to write")
    p_write = sub.add_parser("write", parents=[common],
                             help="write the calibration window from a file")
    p_write.add_argument("file", help="window file (calib's output)")
    p_write.add_argument("--yes", action="store_true",
                         help="confirm writing the factory calibration")
    p_write.add_argument("--force", action="store_true",
                         help="allow addresses outside the calibration window")
    p_write.add_argument("--verify", action="store_true",
                         help="read the window back and compare after writing")
    sub.add_parser("self-test", help="run the offline protocol self-test")

    args = ap.parse_args(argv)

    if args.cmd == "self-test":
        return self_test()

    handlers = {"probe": cmd_probe, "calib": cmd_calib, "dump": cmd_calib,
                "full": cmd_full, "write": cmd_write}

    if args.dry_run:
        # No port needed; the handlers print the frames and return.
        if args.cmd == "probe":
            print(f"TX {build_read_frame(0x3000, 16).hex(' ')}")
            return 0
        return handlers[args.cmd](None, args)

    radio = _open_radio(args)
    try:
        return handlers[args.cmd](radio, args)
    finally:
        try:
            radio.transport.close()
        except Exception:
            pass


def main():
    argv = sys.argv[1:]
    if argv and argv[0] in RADIO_VERBS:
        return radio_main(argv)
    return decode_main(argv)


if __name__ == "__main__":
    sys.exit(main())
