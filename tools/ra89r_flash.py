#!/usr/bin/env python3
"""
ra89r_flash.py -- flash an image into an RA89R over its programming port.

Uses the stock bootloader's serial protocol, reverse engineered from
`bootloader.bin` (0x08000000-0x08003FFF) and the CPS updater
`Radio_UpData_All.exe` -- see `docs/ra89r_bootloader.md` for the evidence.

Protocol shape (all frames are ``FE FE EE EF <cmd> <payload> FD``):

    E0  + b"UP8600BYARR"          handshake; the bootloader answers
                                  ``FE FE EF EE E1 <13 identity bytes> FD``
    E5  + b"BAUDRATE" + 2 digits  pick a baud rate (index into the
                                  bootloader's table 0..9)
    E2  + raw .icf record         program one record; answered PASS/FAIL
    E1  + b"CLEAR"                erase (defined in the CPS updater, unused
                                  by its own flow -- try only if E2 fails)
    E4  + b"EXIT"                 leave the bootloader / start the firmware
    reply                         4-byte ASCII b"PASS" or b"FAIL"

The bootloader only accepts records whose address is outside
0x08000000-0x08003FFF (its own region), so this tool refuses those too.

**The byte at 0x0805FFF0: the bootloader's update-mode request.**  Not a
validity flag, and the polarity is the opposite of what this file used to say:
`0xFF` (the normal value) makes the bootloader start the application, and `0x11`
makes it enter update mode (0x08003328-0x0800335A).  The bootloader *consumes*
the request -- entering update mode writes 0xFF back (0x08000566-0x08000578) --
and the stock application sets 0x11 from its serial command handler when the PC
sends `"Reset"` + `'0'` (0x08015710, then `SYSRESETREQ`), which is how the CPS
reboots a running radio into the bootloader.

This tool writes that byte after the image records by default, as it always has.
It is not needed to flash -- `EXIT` resets the radio and the bootloader consumes
the request and then starts the image, so the radio comes back with 0xFF either
way -- but it changes nothing for the worse and stays the default;
`--no-valid-marker` skips it if the extra update-mode trip is unwanted.

Speed notes (see docs/ra89r_bootloader.md section 5):

  * baud is the main lever -- the bootloader's table reaches 1.024 Mbaud, but it
    runs on the 8 MHz reset clock without OVER8, so ~384 kbaud is the practical
    ceiling (256 kbaud has exactly 0% divisor error).  `--baud auto` (the
    default) sets the fastest rate and then *sweeps* the rates on this side
    until the bootloader answers a harmless E0 ping, so a mis-set baud cannot
    lose the session.
  * the bootloader answers every record with PASS/FAIL, so the host is idle for
    ~7 ms per record (5 ms page erase + 1.5 ms program).  `--window N` sends up
    to N records before draining the acks to hide that (needs validation on
    hardware).

Usage:
    python3 tools/ra89r_flash.py --port /dev/ttyUSB0 probe
    python3 tools/ra89r_flash.py --port /dev/ttyUSB0 flash firmware.icf
    python3 tools/ra89r_flash.py --dry-run flash firmware.icf
"""

import argparse
import sys
import time

import ra89r  # the codec is a sibling in this directory

PREAMBLE = bytes([0xFE, 0xFE, 0xEE, 0xEF])
TERMINATOR = 0xFD
HANDSHAKE_PAYLOAD = b"UP8600BYARR"
ANNOUNCE = bytes([0xFE, 0xFE, 0xEF, 0xEE, 0xE1])
ACK_PASS = b"PASS"
ACK_FAIL = b"FAIL"

CMD_HANDSHAKE = 0xE0
CMD_ERASE = 0xE1
CMD_PROGRAM = 0xE2
CMD_READ = 0xE3
CMD_EXIT = 0xE4
CMD_BAUD = 0xE5

# bootloader.bin 0x080004AC: index -> baud (9600, 19200, 38400, 56000, 57600,
# 115200, 256000, 512000, 1024000, + one unreadable case)
BAUD_TABLE = [9600, 19200, 38400, 56000, 57600, 115200, 256000, 512000, 1024000]

# The update-mode request: 0x11 at the top of the last flash page makes the
# bootloader enter update mode on the next reset; 0xFF (normal) runs the app.
# Records are 256-byte aligned, so the record covering it spans 0x0805FF00..
UPDATE_REQUEST_ADDRESS = 0x0805FFF0
UPDATE_REQUEST_VALUE = 0x11
UPDATE_REQUEST_BASE = UPDATE_REQUEST_ADDRESS & ~0xFF

BOOTLOADER_START = 0x08000000
BOOTLOADER_END = 0x08003FFF
FLASH_START = 0x08004000
FLASH_END = 0x0805FFFF


def frame(cmd, payload=b""):
    return PREAMBLE + bytes([cmd]) + payload + bytes([TERMINATOR])


class Transport(object):
    """Thin wrapper over pyserial so the protocol code stays readable."""

    def __init__(self, port, baud, timeout=1.0, dry_run=False):
        self.dry_run = dry_run
        self.garbled = 0
        self.port = port
        self.baud = baud
        self.serial = None
        if dry_run:
            return
        try:
            import serial
        except ImportError:
            raise SystemExit("pyserial is required to talk to the radio: "
                             "pip install pyserial")
        try:
            # a short port timeout keeps our own poll loops responsive; every
            # wait in this file uses an explicit deadline instead
            self.serial = serial.Serial(port, baudrate=baud, bytesize=8,
                                        parity="N", stopbits=1, timeout=0.02)
        except Exception as exc:               # SerialException and friends
            raise SystemExit("cannot open %s: %s" % (port, exc))

    def close(self):
        if self.serial:
            self.serial.close()

    def set_baud(self, baud):
        self.baud = baud
        if self.serial:
            self.serial.baudrate = baud

    def flush_input(self):
        if self.serial:
            self.serial.reset_input_buffer()

    def write(self, data):
        if self.dry_run:
            text = data.hex().upper()
            if len(text) > 60:            # keep the log readable for records
                text = "%s..%s (%d bytes)" % (text[:44], text[-12:], len(data))
            print("    -> %s" % text)
            return
        self.serial.write(data)
        self.serial.flush()

    def read_reply(self, timeout, idle=0.05, limit=32, terminator=None):
        """Collect bytes until `terminator` (if given) or a short idle gap.

        Returns as soon as the reply is complete, so a 4-byte PASS costs
        microseconds instead of a full timeout.
        """
        if self.dry_run:
            return b""
        deadline = time.time() + timeout
        out = bytearray()
        while len(out) < limit:
            chunk = self.serial.read(limit - len(out))
            if chunk:
                out += chunk
                if terminator is not None and terminator in out:
                    break
                deadline = time.time() + idle
                continue
            if time.time() >= deadline:
                break
        return bytes(out)

    def read_ack(self, timeout):
        """Wait for the 4-byte PASS/FAIL reply, returning b"" when it is absent."""
        if self.dry_run:
            return b""
        deadline = time.time() + timeout
        out = bytearray()
        while len(out) < 4 and time.time() < deadline:
            chunk = self.serial.read(4 - len(out))
            if chunk:
                out += chunk
        return bytes(out)


def update_request_record(baseline):
    """A record that sets the update-mode request (see the module docstring)."""
    payload = bytearray(b"\xFF" * (UPDATE_REQUEST_ADDRESS - UPDATE_REQUEST_BASE + 1))
    payload[-1] = UPDATE_REQUEST_VALUE
    a = UPDATE_REQUEST_BASE >> 8
    header = bytes([(len(payload) >> 8) & 0xFF, len(payload) & 0xFF,
                    (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF, 0x00])
    return UPDATE_REQUEST_BASE, ra89r.Record(header, bytes(payload), baseline).encode()


def add_update_request(records, baseline):
    for addr, raw in records:
        if addr <= UPDATE_REQUEST_ADDRESS < addr + len(raw) - 7:
            return False
    records.append(update_request_record(baseline))
    return True


def load_records(icf_path):
    """Read an .icf and return [(address, raw_record_bytes), ...]."""
    raws = ra89r.read_raw_records(icf_path)
    baseline = ra89r.detect_baseline(raws)
    out = []
    for raw in raws:
        rec, stored = ra89r.parse_record(raw, baseline)
        if stored != rec.check:
            raise SystemExit("%s: record at 0x%08X has a bad check byte"
                             % (icf_path, rec.address))
        out.append((rec.address, raw))
    print("baseline 0x%02X, %d records, 0x%08X..0x%08X"
          % (baseline, len(out), out[0][0], out[-1][0]))
    return out, baseline


def check_addresses(records, allow_bootloader):
    bad = [a for a, _ in records
           if a < FLASH_START and not allow_bootloader or a > FLASH_END]
    if bad:
        raise SystemExit("refusing to flash outside 0x%08X-0x%08X: %s%s"
                         % (FLASH_START, FLASH_END,
                            ", ".join("0x%08X" % a for a in bad[:4]),
                            " (the bootloader answers FAIL there anyway; use "
                            "--allow-bootloader-region to try)" if not allow_bootloader else ""))


def do_handshake(io, args_or_timeout, verbose=True):
    """Send E0 and return the bootloader's identity bytes (or None).

    The E0 frame is idempotent in the bootloader (it only rebuilds and sends the
    announcement), so this doubles as a safe ping.
    """
    timeout = (args_or_timeout.timeout if hasattr(args_or_timeout, "timeout")
               else args_or_timeout)
    io.flush_input()
    if verbose:
        print("handshake (E0) at %d baud ..." % io.baud)
    io.write(frame(CMD_HANDSHAKE, HANDSHAKE_PAYLOAD))
    reply = io.read_reply(timeout, limit=32, terminator=TERMINATOR)
    if not reply.startswith(ANNOUNCE):
        if verbose:
            print("  no answer (got %s)" % (reply.hex().upper() or "nothing"))
        return None
    ident = reply[len(ANNOUNCE):-1]
    if verbose:
        print("  bootloader answered, identity %s" % ident.hex().upper())
    return ident


def ping(io, timeout=0.35):
    """Cheap 'are you there at this baud?' probe: an E0 handshake."""
    return do_handshake(io, timeout, verbose=False) is not None


def wait_ack(io, timeout, what, strict=False):
    """Wait for the 4-byte PASS/FAIL reply; True/False/None (None = timeout).

    Cheap USB-serial adapters sometimes clip the first byte of the reply on a
    high-rate link (`\xf8ASS` instead of `PASS`).  The remaining three bytes are
    unambiguous, so accept them and count the damage -- the caller drops to a
    lower baud if it keeps happening.
    """
    data = io.read_ack(timeout)
    if data == ACK_PASS:
        return True
    if data == ACK_FAIL:
        return False
    if strict:
        if data:
            io.garbled += 1
            print("  %s: damaged reply %r (needs a clean one)" % (what, data))
        return None
    # damage can clip the head ('\xf8ASS') or the tail ('PA\xff\xff'); the first
    # two bytes still say which verdict it was
    if len(data) == 4 and data[:2] in (ACK_PASS[:2], ACK_FAIL[:2]):
        io.garbled += 1
        verdict = data[:2] == ACK_PASS[:2]
        print("  %s: damaged reply %r -> %s" % (what, data,
                                                "PASS" if verdict else "FAIL"))
        return verdict
    if len(data) == 4 and data[1:] in (ACK_PASS[1:], ACK_FAIL[1:]):
        io.garbled += 1
        verdict = data[1:] == ACK_PASS[1:]
        print("  %s: damaged reply %r -> %s" % (what, data,
                                                "PASS" if verdict else "FAIL"))
        return verdict
    if data:
        io.garbled += 1
        print("  %s: unexpected reply %r" % (what, data))
    return None


# The bootloader runs on the 8 MHz reset clock and does not set OVER8, so the
# USART tops out near 500 kbaud: 256000 has exactly 0% divisor error, 512000 and
# 1024000 cannot be generated and leave the link broken until a reset.  The sweep
# therefore ends at index 0 (9600) so a failed switch always recovers.
BAUD_CANDIDATES = [6, 5, 4, 2, 1, 0]           # fastest first, 9600 last
BAUD_RISKY = {7: 512000, 8: 1024000}

# Where 'auto' starts.  Not the fastest rate in the table: a rate is probed with
# three short pings, and a link that answers those can still corrupt the several
# kilobytes of a record plus its reply -- 256000 did exactly that on a USB-serial
# adapter, while the same cable has carried this radio's 115200 console all
# along.  So start where the hardware is already proven and let the ladder step
# down from there; --baud 6 asks for the fast one explicitly.
AUTO_START_INDEX = 5                           # 115200


def link_quality(io, pings=3, timeout=0.35):
    """How many clean E0 answers come back at the current rate.

    The E0 handshake is idempotent (the bootloader only rebuilds and re-sends its
    announcement), so this is a safe way to measure the link before spending a
    whole image on it.
    """
    clean = 0
    for _ in range(pings):
        io.flush_input()
        io.write(frame(CMD_HANDSHAKE, HANDSHAKE_PAYLOAD))
        reply = io.read_reply(timeout, limit=32, terminator=TERMINATOR)
        if reply.startswith(ANNOUNCE):
            clean += 1
    return clean


def baud_ladder(index):
    """The order to sweep after asking for rate `index`.

    The rate asked for first, then the *slower* ones, and only then the faster
    ones -- a faster rate is only worth trying if nothing at or below the request
    answered at all, which means the request plainly did not take.

    This ordering is the whole point.  `BAUD_CANDIDATES` is fastest-first, and
    the sweep used to be `[index] + candidates`, so a caller stepping *down*
    because the fast link was damaging replies would find the fast rate still
    answering a ping and be handed it straight back: the log would say
    "switching baud (index 4 = 57600)" and then "talking at 256000 baud".
    """
    return ([i for i in BAUD_CANDIDATES if i <= index] +
            [i for i in BAUD_CANDIDATES if i > index])


def set_baud(io, index, args, verbose=True):
    """Ask the bootloader to switch baud, then find the rate it is really at.

    The command itself is not acknowledged, so after sending it we sweep the
    candidate rates and take the first that answers *cleanly*.  A wrong guess
    costs nothing: the ping is harmless and the sweep covers every rate in the
    bootloader's table (including 9600, if it ignored us).  The sweep never
    offers a faster rate than the one asked for unless nothing slower answers
    (see baud_ladder).
    """
    if index in BAUD_RISKY:
        print("warning: index %d (%s baud) needs more than the bootloader's 8 MHz "
              "clock; if the link dies, power-cycle the radio"
              % (index, BAUD_RISKY[index]))
    if verbose:
        print("switching baud (index %d = %s) ..."
              % (index, BAUD_TABLE[index] if index < len(BAUD_TABLE) else "?"))
    io.set_baud(BAUD_TABLE[0])                  # the command is always at 9600
    io.write(frame(CMD_BAUD, b"BAUDRATE" + b"%02d" % index))
    time.sleep(args.baud_settle)

    requested_ok = False
    fallback = None                             # slowest rate that at least answers
    for i in baud_ladder(index):
        baud = BAUD_TABLE[i] if i < len(BAUD_TABLE) else None
        if not baud:
            continue
        io.set_baud(baud)
        if not ping(io):
            continue
        if i == index:
            requested_ok = True
        if fallback is None or baud < BAUD_TABLE[fallback]:
            fallback = i
        clean = link_quality(io, args.quality_pings)
        if verbose:
            print("  %d baud: %d/%d clean probes%s"
                  % (baud, clean, args.quality_pings,
                     "" if clean == args.quality_pings else " -- too noisy, "
                     "step down"))
        if clean == args.quality_pings:
            if verbose:
                print("  talking at %d baud (index %d)" % (baud, i))
            return i
    if fallback is not None:
        # Nothing answered cleanly.  Stay where the caller asked to be if that
        # rate at least answers, otherwise take the slowest that does -- a noisy
        # link is retried either way, and the slow one is the better bet.
        choice = index if requested_ok else fallback
        print("  every rate was noisy; continuing at %d baud (index %d) anyway "
              "(damaged replies are retried)" % (BAUD_TABLE[choice], choice))
        io.set_baud(BAUD_TABLE[choice])
        return choice
    print("  no rate answered -- power-cycle the radio and retry with --baud 0")
    return None


def classify(ident):
    """Tell the bootloader's announcement apart from the application's reply.

    Both answer the E0 handshake with FE FE EF EE E1.  The bootloader's identity
    starts with 0x56 (on the RA89R: 56 11 16 04 15 followed by ASCII "8600" and
    "0000"); the updater tests exactly that byte (`RxBuffer[...+5] == 86`) to
    recognise the bootloader, and switches on model codes such as 0x2900, 0x3100,
    0x8500 for the running application (AckDataChkPro).
    """
    if not ident:
        return "unknown"
    if ident[0] == 0x56:
        return "bootloader"
    if len(ident) >= 2:
        return "application (model code %02X%02X)" % (ident[0], ident[1])
    return "unknown"


def cmd_probe(args):
    io = Transport(args.port, args.dry_run and 9600 or 9600, args.timeout, args.dry_run)
    try:
        ident = do_handshake(io, args)
        if ident is None:
            print("nothing answered at 9600 8N1.\n"
                  "  * is the cable on the Kenwood-style jack (USB-C cannot "
                  "flash this radio)?\n"
                  "  * did you put the radio into update mode first?  Try "
                  "powering it on while holding the PTT / side key, then probe "
                  "again; the CPS's update dialog shows the exact key combo.")
            return 1
        what = classify(ident)
        if what != "bootloader":
            print("that is the %s, not the bootloader.\n"
                  "The stock firmware is running: cycle the radio into update "
                  "mode (power on while holding the PTT / side key)." % what)
            return 2
        print("bootloader detected -- safe to flash")
        return 0
    finally:
        io.close()


def cmd_flash(args):
    records, baseline = load_records(args.icf)
    if not args.no_valid_marker:
        if add_update_request(records, baseline):
            print("adding the update-mode request (%02X at 0x%08X); the "
                  "bootloader consumes it on the next reset and starts the image"
                  % (UPDATE_REQUEST_VALUE, UPDATE_REQUEST_ADDRESS))
    check_addresses(records, args.allow_bootloader_region)
    # 'auto' starts where the hardware is proven and the ladder steps down from
    # there (see AUTO_START_INDEX); 'keep' stays at 9600
    baud_index = (AUTO_START_INDEX if args.baud == "auto"
                  else int(args.baud)) if args.baud != "keep" else 0

    if args.dry_run:
        io = Transport(None, 9600, dry_run=True)
        print("dry run: %d records, %d bytes"
              % (len(records), sum(len(r) for _, r in records)))
        do_handshake(io, args)
        print("  (bootloader would answer %s + identity + FD)"
              % ANNOUNCE.hex().upper())
        io.write(frame(CMD_BAUD, b"BAUDRATE" + b"%02d" % baud_index))
        for addr, raw in records:
            print("  record 0x%08X  %d bytes" % (addr, len(raw) - 7))
            io.write(frame(CMD_PROGRAM, raw))
        io.write(frame(CMD_EXIT, b"EXIT"))
        return 0

    io = Transport(args.port, 9600, args.timeout, args.dry_run)
    try:
        if do_handshake(io, args) is None:
            print("no bootloader answer at 9600 -- see the probe message")
            return 1

        if args.baud != "keep":
            if set_baud(io, baud_index, args) is None:
                return 1

        if args.erase_first:
            print("sending ERASE first")
            io.write(frame(CMD_ERASE, b"CLEAR"))
            time.sleep(1.0)
            wait_ack(io, args.timeout, "erase")

        for attempt_index, baud_index in enumerate(
                [baud_index] + [i for i in BAUD_CANDIDATES if i < baud_index]):
            if attempt_index:
                print("link was unreliable; restarting at %d baud"
                      % BAUD_TABLE[baud_index])
                io.garbled = 0
                if set_baud(io, baud_index, args) is None:
                    continue
            status = flash_records(io, records, args)
            if status == "ok":
                return 0
            if status == "rejected":
                return 1
        print("could not get a clean link at any baud rate -- power-cycle the "
              "radio back into update mode and retry with --baud 0 (9600)")
        return 1
    finally:
        io.close()


def flash_records(io, records, args):
    """Program every record.  Returns "ok", "rejected" or "unreliable"."""
    if True:
        started = time.time()
        pending = []                    # (index, addr, attempts) awaiting an ack
        confirm = []                    # records whose ack arrived damaged
        for index, (addr, raw) in enumerate(records):
            while len(pending) >= args.window:
                if not drain_ack(io, pending, args, records):
                    return 1
            io.write(frame(CMD_PROGRAM, raw))
            pending.append([index, addr, 1])
            if args.progress:
                print("  %3d/%d  0x%08X  %6d bytes"
                      % (index + 1, len(records), addr, len(raw) - 7))
            if args.window == 1:
                if not drain_ack(io, pending, args, records, confirm):
                    return "rejected" if io.garbled == 0 else "unreliable"
        while pending:
            if not drain_ack(io, pending, args, records, confirm):
                return "rejected" if io.garbled == 0 else "unreliable"

        # Confirmation pass: re-write (idempotent) every record whose PASS came
        # back damaged and insist on an exact PASS this time.  Without it a
        # damaged reply is only *probable* evidence, not proof.
        for index in confirm:
            addr, raw = records[index]
            for attempt in range(1, args.retries + 1):
                io.write(frame(CMD_PROGRAM, raw))
                if wait_ack(io, args.timeout, "confirm %d" % index, strict=True):
                    print("  record %d (0x%08X) confirmed" % (index, addr))
                    break
            else:
                print("record %d (0x%08X) could not be confirmed" % (index, addr))
                return "unreliable"

        print("sending EXIT")
        io.write(frame(CMD_EXIT, b"EXIT"))
        elapsed = time.time() - started
        print("done: %d records, %d bytes in %.1f s (%.1f kB/s)%s"
              % (len(records), sum(len(r) for _, r in records), elapsed,
                 sum(len(r) for _, r in records) / max(elapsed, 1e-3) / 1024.0,
                 (" [%d damaged replies]" % io.garbled) if io.garbled else ""))
        return "ok"


def drain_ack(io, pending, args, records, confirm=None):
    """Read the ack for the oldest pending record; retry or abort on FAIL."""
    index, addr, attempts = pending.pop(0)
    before = io.garbled
    ack = wait_ack(io, args.timeout, "record %d" % index)
    if ack and confirm is not None and io.garbled > before:
        # the write was acknowledged, but the reply was damaged -- ask for it
        # again so the record ends up with an *exact* PASS
        confirm.append(index)
    if ack:
        return True
    if attempts < args.retries:
        # A FAIL can mean the bootloader's parser lost framing (its read length
        # comes from the record header, so one bad length desyncs the link).
        # The E0 handshake is harmless and makes it resync on a preamble.
        print("  record %d at 0x%08X failed, resyncing and retrying (%d/%d)"
              % (index, addr, attempts + 1, args.retries))
        if not ping(io):
            print("  link lost (no answer to the resync handshake) -- "
                  "power-cycle the radio and re-run")
            return False
        io.write(frame(CMD_PROGRAM, records[index][1]))
        pending.insert(0, [index, addr, attempts + 1])
        return True
    print("record %d at 0x%08X was rejected by the bootloader" % (index, addr))
    print("the image was NOT fully written; check --erase-first / --baud-index")
    return False


def main(argv=None):
    ap = argparse.ArgumentParser(description="flash an RA89R via its bootloader")
    ap.add_argument("--port", help="serial port, e.g. /dev/ttyUSB0")
    ap.add_argument("--timeout", type=float, default=1.5,
                    help="per-reply timeout in seconds (default 1.5)")
    ap.add_argument("--baud", default="auto",
                    help="'auto' (default: start at 115200 and step down if the "
                         "link damages replies), 'keep' (stay at 9600), or a "
                         "bootloader table index 0..8 (0=9600, 4=57600, "
                         "5=115200, 6=256000, 7=512000, 8=1024000)")
    ap.add_argument("--baud-index", type=int,
                    help="deprecated alias for --baud <index>")
    ap.add_argument("--quality-pings", type=int, default=3,
                    help="clean E0 probes required at a rate before the image is "
                         "sent at it (default 3; noisy links automatically step "
                         "down)")
    ap.add_argument("--baud-settle", type=float, default=1.1,
                    help="seconds to wait after the baud command; the bootloader "
                         "itself delays 1000 ms (default 1.1)")
    ap.add_argument("--window", type=int, default=1,
                    help="send up to N records before draining acks (default 1 "
                         "= wait for PASS after each; >1 needs hardware checks)")
    ap.add_argument("--retries", type=int, default=3)
    ap.add_argument("--dry-run", action="store_true",
                    help="print the frames instead of using the port")
    ap.add_argument("--no-valid-marker", action="store_true",
                    help="do not write 0x11 to 0x0805FFF0; that byte is the "
                         "bootloader's update-mode request (0xFF, the normal "
                         "value, is what runs the app, and a request is consumed "
                         "on the next reset), so skipping it is harmless")
    ap.add_argument("--erase-first", action="store_true",
                    help="send the E1/CLEAR command before programming")
    ap.add_argument("--allow-bootloader-region", action="store_true",
                    help="do not refuse records in 0x08000000-0x08003FFF")
    ap.add_argument("--quiet-progress", dest="progress", action="store_false")
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("probe", help="handshake only (is the radio in update mode?)")
    p.set_defaults(fn=cmd_probe)

    f = sub.add_parser("flash", help="program an .icf image")
    f.add_argument("icf", help=".icf produced by tools/ra89r.py mkicf or the stock CPS")
    f.set_defaults(fn=cmd_flash)

    args = ap.parse_args(argv)
    if args.baud_index is not None:
        args.baud = str(args.baud_index)
    if not getattr(args, "fn", None):
        ap.print_help()
        return 2
    if not args.dry_run and not args.port:
        print("error: --port is required (or use --dry-run)", file=sys.stderr)
        return 2
    try:
        return args.fn(args)
    except ra89r.IcfError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
