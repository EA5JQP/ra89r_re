#!/usr/bin/env python3
"""
ra89r_eeprom_sim.py -- test double for the RA89R EEPROM console.

The `driver/eeprom` firmware exposes the external SPI NOR flash (what the CPS
calls the EEPROM) over the UART console:

    e                 identify the chip and report its size
    E                 stream the whole chip:  "EEPROM DUMP <size>\\n" <raw bytes>
                                          then "\\nEEPROM END <sum>\\n"
    W <size> <sum>    restore:               "EEPROM RESTORE <size>\\n", then
                      <size> raw bytes in 4 KB sectors, each answered with a
                      '.' ack after the sector is erased and programmed, then
                      "\\nEEPROM RESTORE OK <sum>\\n" (or FAIL)

This speaks that protocol on a pseudo terminal with a byte-array chip, so
`tools/ra89r_eeprom.py` can be exercised end to end without a radio:

    python3 tools/ra89r_eeprom_sim.py --pty /tmp/ra89r-eeprom-pty --size 65536 &
    python3 tools/ra89r_eeprom.py --port $(cat /tmp/ra89r-eeprom-pty) backup out.bin
    python3 tools/ra89r_eeprom.py --port $(cat /tmp/ra89r-eeprom-pty) restore out.bin --yes

Options that model the failure paths:

    --corrupt N       flip a byte every N bytes as a restore writes them, so the
                      checksum the firmware reports will not match the host's
    --drop-after N    accept a restore header then stop reading after N bytes
                      (emulates a host that died mid-transfer)

The PTY is put in raw mode: the payload is binary, and ONLCR/echo would shred
it.
"""

import argparse
import os
import sys
import time
import tty

HEADER = b"EEPROM DUMP "
RESTORE = b"EEPROM RESTORE "
END = b"EEPROM END "


class Sim(object):
    def __init__(self, size=2 * 1024 * 1024, verbose=False, corrupt=0,
                 drop_after=0):
        self.chip = bytearray(size)
        # a recognisable, non-uniform pattern so a lost byte is visible
        for i in range(size):
            self.chip[i] = (i * 7 + 0x11) & 0xFF
        self.size = size
        self.verbose = verbose
        self.corrupt = corrupt
        self.drop_after = drop_after
        self.dumps = 0
        self.restores = 0

    def log(self, msg):
        if self.verbose:
            print("[sim] %s" % msg, flush=True)

    @staticmethod
    def checksum(data):
        return sum(data) & 0xFFFFFFFF

    def cmd_report(self, out):
        out.write(b"\neeprom: 0x90 id 0x8514 (the stock looks for the "
                  b"Winbond 0xEF16), 0x9F jedec 0x852015\n")
        out.write(("eeprom: %u bytes (%u KB); 'E' dumps the whole chip as "
                   "binary\n" % (self.size, self.size // 1024)).encode())

    def cmd_dump(self, out):
        self.dumps += 1
        out.write(("\n" + HEADER.decode() + "%u\n" % self.size).encode())
        out.write(bytes(self.chip))
        out.write(("\n" + END.decode() + "%08X\n"
                   % self.checksum(self.chip)).encode())
        self.log("dumped %d bytes" % self.size)

    def run(self, fd):
        buf = bytearray()
        out = os.fdopen(os.dup(fd), "wb", buffering=0)
        while True:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                # The last slave closed (the host tool exited).  Wait for the
                # next open instead of dying, so one simulator can serve a
                # backup followed by a restore.
                time.sleep(0.01)
                continue
            if not chunk:
                continue
            buf += chunk

            while buf:
                # 'e' and 'E' are single-character commands (the firmware
                # triggers on the character, not a line).
                if buf[0:1] == b"e":
                    del buf[:1]
                    self.cmd_report(out)
                    continue
                if buf[0:1] == b"E":
                    del buf[:1]
                    self.cmd_dump(out)
                    continue

                # A restore header is line-based, then switches to raw bytes,
                # 4 KB at a time with a '.' ack after each sector.
                if buf.startswith(b"W ") and b"\n" in buf:
                    line, _, rest = buf.partition(b"\n")
                    buf = bytearray(rest)
                    if not self._begin_restore(line, out):
                        continue
                    received_sum = 0
                    deadline = 0
                    for base in range(0, self.size, 4096):
                        need = min(4096, self.size - base)
                        got = bytearray()
                        while len(got) < need:
                            if self.drop_after and \
                                    base + len(got) >= self.drop_after:
                                self.log("dropping the rest of the transfer")
                                return
                            if buf:
                                take = min(len(buf), need - len(got))
                                got += buf[:take]
                                del buf[:take]
                                continue
                            try:
                                more = os.read(fd, 65536)
                            except OSError:
                                return
                            if not more:
                                deadline += 1
                                if deadline > 2000:
                                    self.log("timed out mid-restore")
                                    return
                                continue
                            buf += more
                        if self.corrupt:
                            for i in range(0, len(got), self.corrupt):
                                got[i] ^= 0xFF
                        self.chip[base:base + need] = got
                        received_sum = (received_sum + sum(got)) & 0xFFFFFFFF
                        out.write(b".")
                    self._finish_restore(received_sum, out)
                    continue

                if b"\n" in buf:
                    line, _, rest = buf.partition(b"\n")
                    buf = bytearray(rest)
                    self.log("unknown command %r" % bytes(line))
                    continue
                break

    def _begin_restore(self, line, out):
        parts = line.split()
        try:
            size = int(parts[1])
            declared = int(parts[2], 16)
        except (IndexError, ValueError):
            out.write(b"\nEEPROM RESTORE ERR bad-header\n")
            return False
        if size != self.size:
            out.write(("\nEEPROM RESTORE ERR size %u, chip is %u\n"
                       % (size, self.size)).encode())
            return False
        out.write(("\n" + RESTORE.decode() + "%u\n" % size).encode())
        self._declared = declared
        self.log("restore started, %d bytes" % size)
        return True

    def _finish_restore(self, sum_received, out):
        self.restores += 1
        verdict = b"OK" if sum_received == self._declared else b"FAIL"
        out.write(("\n" + RESTORE.decode() + verdict.decode() + " %08X\n"
                   % sum_received).encode())
        self.log("restore finished: %s (declared %08X, got %08X)"
                 % (verdict.decode(), self._declared, sum_received))


def main(argv=None):
    ap = argparse.ArgumentParser(description="RA89R EEPROM console test double")
    ap.add_argument("--pty", help="write the pty slave path here")
    ap.add_argument("--size", type=lambda v: int(v, 0), default=2 * 1024 * 1024,
                    help="emulated chip size in bytes (default 2 MB)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--corrupt", type=int, default=0,
                    help="flip a byte every N bytes while restoring")
    ap.add_argument("--drop-after", type=int, default=0,
                    help="stop reading a restore after N bytes")
    a = ap.parse_args(argv)

    master, slave = os.openpty()
    tty.setraw(slave)
    name = os.ttyname(slave)
    if a.pty:
        with open(a.pty, "w") as f:
            f.write(name)
    print("eeprom simulator on %s (ctrl-c to stop)" % name, flush=True)

    sim = Sim(size=a.size, verbose=a.verbose, corrupt=a.corrupt,
              drop_after=a.drop_after)
    try:
        sim.run(master)
    except KeyboardInterrupt:
        pass
    print("[sim] %d dumps, %d restores" % (sim.dumps, sim.restores))
    return 0


if __name__ == "__main__":
    sys.exit(main())
