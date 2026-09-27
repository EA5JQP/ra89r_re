#!/usr/bin/env python3
"""
ra89r_bootloader_sim.py -- test double for the stock RA89R bootloader.

Speaks the protocol documented in docs/ra89r_bootloader.md on a pseudo terminal, so
`tools/ra89r_flash.py` can be exercised end to end without a radio:

    python3 tools/ra89r_bootloader_sim.py --pty /tmp/ra89r-pty &
    python3 tools/ra89r_flash.py --port $(cat /tmp/ra89r-pty) flash fw.icf

What it checks (the parts that matter for correctness):

  * E0  handshake -> the 19-byte announcement
  * E5  BAUDRATE + 2 digits -> accepted (no ack, like the real thing)
  * E2  the payload is a raw .icf record: baseline 0x66, header length, the
        byte-sum check, and the address range (it refuses 0x08000000-0x08003FFF
        exactly like the bootloader does)
  * E4  EXIT
  * replies PASS/FAIL (4 ASCII bytes)

It also emulates the timing that limits real flashing: 5 ms page erase +
1.5 ms program per 2048-byte page (datasheet figures), so timing measurements
from a simulation are representative of the serial+flash part.

Limitations: a PTY has no baud rate, so the simulator answers at whatever rate
the host asks for -- the real ceiling comes from the bootloader running on the
8 MHz reset clock (~384 kbaud; see docs/ra89r_bootloader.md section 5).
"""

import argparse
import os
import struct
import sys
import time

import ra89r  # the codec is a sibling in this directory

PREAMBLE = bytes([0xFE, 0xFE, 0xEE, 0xEF])
TERMINATOR = 0xFD
ANNOUNCE = bytes([0xFE, 0xFE, 0xEF, 0xEE, 0xE1])
IDENTITY = bytes([0x55, 0x50, 0x38, 0x36, 0x30])     # enough for the updater's check
PASS = b"PASS"
FAIL = b"FAIL"

CMD_HANDSHAKE, CMD_ERASE, CMD_PROGRAM, CMD_READ, CMD_EXIT, CMD_BAUD = (
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5)

BOOTLOADER_START, BOOTLOADER_END = 0x08000000, 0x08003FFF
PAGE = 0x800
ERASE_MS = 5.0          # datasheet: page erase time
PROGRAM_MS = 1.5        # datasheet: page programming time
BASELINE = 0x66


class Sim(object):
    def __init__(self, verbose=False, fail_first_n=0, mutate_every=0,
                 ignore_baud=False, mute_after_baud=0.0):
        self.ignore_baud = ignore_baud
        self.mute_after_baud = mute_after_baud
        self.mute_until = 0.0
        self.verbose = verbose
        self.remaining_failures = fail_first_n
        self.mutate_every = mutate_every
        self.pages = set()
        self.records = 0
        self.rejected = 0
        self.exited = False
        self.baud_index = 0

    def log(self, msg):
        if self.verbose:
            print("[sim] %s" % msg, flush=True)

    # -- protocol ---------------------------------------------------------
    def handle(self, cmd, payload):
        if cmd == CMD_HANDSHAKE:
            if time.time() < self.mute_until:
                # emulate the host listening at a rate the bootloader is not
                # using (a PTY cannot do that by itself)
                self.log("E0 ping dropped (muted: wrong baud emulation)")
                return None
            self.log("E0 handshake -> announcement")
            return ANNOUNCE + IDENTITY + bytes(4) + bytes([TERMINATOR])

        if cmd == CMD_BAUD:
            if self.ignore_baud:
                self.log("E5 BAUDRATE ignored (simulating a bootloader that "
                         "stays at 9600)")
                return None
            digits = payload[len(b"BAUDRATE"):]
            try:
                self.baud_index = int(digits.decode("ascii"))
            except ValueError:
                self.baud_index = -1
            self.log("E5 BAUDRATE index %d (no ack, like the real bootloader)"
                     % self.baud_index)
            if self.mute_after_baud:
                self.mute_until = time.time() + self.mute_after_baud
                self.log("  muting E0 pings for %.1f s (wrong-baud emulation)"
                         % self.mute_after_baud)
            time.sleep(1.0)                    # the bootloader delays 1000 ms
            return None

        if cmd == CMD_ERASE:
            self.log("E1 %s -> PASS" % payload.decode("ascii", "replace"))
            self.pages.clear()
            return PASS

        if cmd == CMD_READ:
            self.log("E3 read (not implemented by this build)")
            return FAIL

        if cmd == CMD_EXIT:
            self.log("E4 EXIT -> PASS")
            self.exited = True
            return PASS

        self.log("unknown command 0x%02X" % cmd)
        return FAIL

    def program(self, record):
        if self.remaining_failures > 0:
            self.remaining_failures -= 1
            self.rejected += 1
            self.log("E2 record %d -> FAIL (simulated failure)" % self.records)
            return FAIL
        if len(record) < 8:
            self.rejected += 1
            return FAIL
        if self.mutate_every and (self.records + 1) % self.mutate_every == 0:
            # corrupt a payload byte: the frame length stays valid, the check
            # byte no longer sums to zero (this is what a bad record looks like)
            record = record[:8] + bytes([record[8] ^ 0x01]) + record[9:]
        header = bytes(b ^ BASELINE for b in record[:6])
        length = (header[0] << 8) | header[1]
        if len(record) != 6 + length + 1:
            self.rejected += 1
            self.log("E2 length mismatch -> FAIL")
            return FAIL
        key = _key(header)
        payload = [b ^ key for b in record[6:6 + length]]
        check_plain = record[6 + length] ^ key
        if (sum(header) + sum(payload) + check_plain) & 0xFF:
            self.rejected += 1
            self.log("E2 bad check byte -> FAIL")
            return FAIL
        addr = ((header[2] << 16) | (header[3] << 8) | header[4]) * 0x100
        if BOOTLOADER_START <= addr <= BOOTLOADER_END:
            self.rejected += 1
            self.log("E2 0x%08X is inside the bootloader -> FAIL" % addr)
            return FAIL

        pages = set(range(addr // PAGE, (addr + length + PAGE - 1) // PAGE))
        fresh = pages - self.pages
        time.sleep((len(fresh) * ERASE_MS + len(pages) * PROGRAM_MS) / 1000.0)
        self.pages |= pages
        self.records += 1
        self.log("E2 0x%08X len %d -> PASS (%d pages, %d erased)"
                 % (addr, length, len(pages), len(fresh)))
        return PASS

    # -- framing ----------------------------------------------------------
    # The program frame is *length driven*: the record header says how long the
    # payload is (the stock records contain 726 bytes of 0xFD inside their
    # payloads, so an FD search could never work).  The trailing FD is a sanity
    # byte that is simply skipped if present.  The fixed commands (E0/E1/E3/E4/
    # E5) carry short ASCII payloads and are terminated by FD.
    def run(self, fd):
        buf = bytearray()
        while not self.exited:
            try:
                chunk = os.read(fd, 512)
            except OSError:
                break
            if not chunk:
                continue
            buf += chunk
            while True:
                start = buf.find(PREAMBLE)
                if start < 0:
                    del buf[:max(0, len(buf) - 4)]
                    break
                if len(buf) < start + 5:
                    del buf[:start]
                    break
                cmd = buf[start + 4]
                if cmd == CMD_PROGRAM:
                    if len(buf) < start + 11:
                        del buf[:start]
                        break
                    header = bytes(b ^ BASELINE for b in buf[start + 5:start + 11])
                    length = (header[0] << 8) | header[1]
                    if length > 0x4000:            # implausible: resync
                        self.log("E2 implausible length %d, resyncing" % length)
                        del buf[:start + 4]
                        break
                    need = 11 + length + 1
                    if len(buf) < start + need:
                        del buf[:start]
                        break
                    record = bytes(buf[start + 5:start + need])
                    pos = start + need
                    if pos < len(buf) and buf[pos] == TERMINATOR:
                        pos += 1
                    del buf[:pos]
                    reply = self.program(record)
                    if reply:
                        os.write(fd, reply)
                    continue
                end = buf.find(bytes([TERMINATOR]), start + 5)
                if end < 0:
                    del buf[:start]
                    break
                payload = bytes(buf[start + 5:end])
                del buf[:end + 1]
                reply = self.handle(cmd, payload)
                if reply:
                    os.write(fd, reply)


def _key(header):
    key = BASELINE
    for b in header:
        key ^= b
    return key


def main(argv=None):
    ap = argparse.ArgumentParser(description="RA89R bootloader test double")
    ap.add_argument("--pty", help="write the pty slave path here (e.g. /tmp/ra89r-pty)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--fail-first-n", type=int, default=0,
                    help="answer FAIL to the first N program records")
    ap.add_argument("--mutate-every", type=int, default=0,
                    help="corrupt every Nth record before validating it")
    ap.add_argument("--mute-after-baud", type=float, default=0.0,
                    help="drop E0 pings for this many seconds after BAUDRATE, to "
                         "emulate the host guessing the wrong rate")
    ap.add_argument("--ignore-baud", action="store_true",
                    help="accept the BAUDRATE command but stay at 9600 (tests the "
                         "host's baud sweep recovery)")
    a = ap.parse_args(argv)

    master, slave = os.openpty()
    name = os.ttyname(slave)
    if a.pty:
        with open(a.pty, "w") as f:
            f.write(name)
    print("bootloader simulator on %s (ctrl-c to stop)" % name, flush=True)

    sim = Sim(verbose=a.verbose, fail_first_n=a.fail_first_n,
              mutate_every=a.mutate_every, ignore_baud=a.ignore_baud,
              mute_after_baud=a.mute_after_baud)
    try:
        sim.run(master)
    except KeyboardInterrupt:
        pass
    print("[sim] %d records programmed, %d rejected, exit=%s"
          % (sim.records, sim.rejected, sim.exited))
    return 0


if __name__ == "__main__":
    sys.exit(main())
