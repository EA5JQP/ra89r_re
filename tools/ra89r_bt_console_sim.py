#!/usr/bin/env python3
"""Test double for the RA89R console's Bluetooth debug channel.

Speaks the `!` line protocol of firmware/App/app/bt_debug.c on a PTY -- the
same `BT <ms> <kind> <body>` replies, character echo included -- with a fake
linked headset behind it, so tools/ra89r_bt.py can be exercised with no radio
(`python3 tools/ra89r_bt.py --selftest` does exactly that).

    python3 tools/ra89r_bt_console_sim.py --pty /tmp/ra89r-bt-pty --button side &
    python3 tools/ra89r_bt.py --port "$(cat /tmp/ra89r-bt-pty)" ptt-test

The fake headset "presses" its button 0.6 s after the first `!t1` and
releases it 1 s later; `--button` picks what the module reports for that:

    ptt      +IM_EAR_PTT_KEYDOWN / +IM_EAR_PTT_KEYUP   (the stock's events)
    side     +IM_EAR_SIDE_SINGLE1 / +IM_EAR_SIDE_SINGLE0 (seen on this radio)
    unknown  a line no parser prefix matches
    none     nothing at all

The PTT bookkeeping follows app/bt.c and driver/tx.c: KEYDOWN/UP set and clear
the headset flag, SIDE_SINGLE1 toggles it, SIDE_SINGLE0 is not parsed, and the
flag keys TX only when the PTT type allows the headset (0 BT, 1 radio, 2 both).
Keep the reply formats in step with bt_debug.c -- a sim that drifts stops
proving anything.
"""
import argparse
import os
import select
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ra89r_bt import classify  # noqa: E402  (the same parser table)


class Sim:
    def __init__(self, fd, button, ptt_type, linked):
        self.fd = fd
        self.t0 = time.monotonic()
        self.button = button
        self.type = ptt_type
        self.en = True
        self.link = linked
        self.sco = linked
        self.ptt = False
        self.tx = False
        self.trace = False
        self.line = None
        self.rx = 0
        self.raw = b""
        self.pending = []          # (due, module line)
        self.pressed = False

    def ms(self):
        return int((time.monotonic() - self.t0) * 1000)

    def out(self, text):
        os.write(self.fd, text.encode())

    def emit(self, kind, body):
        self.out("BT %d %s %s\n" % (self.ms(), kind, body))

    def allow(self, radio=False):
        if self.type == 1:
            return radio
        if self.type == 2:
            return radio or self.ptt
        return self.ptt

    def status(self):
        self.emit("S", "en=%d state=%s link=%d sco=%d resume=0 ptt=%d "
                       "radio_ptt=0 type=%d allow=%d tx=%d bt_tx=%d found=0 "
                       "rx=%d ver=YBT100_SIM"
                  % (self.en, "Linked" if self.link else "Ready", self.link,
                     self.sco, self.ptt, self.type, self.allow(), self.tx,
                     self.tx, self.rx))

    def module(self, text):
        """A line from the fake module, through the firmware's parser."""
        self.rx += len(text) + 2
        self.raw = (self.raw + text.encode() + b"\r\n")[-64:]
        ev = classify(text)
        if self.trace:
            self.emit("<", "%s [%s]" % (text, ev))
        before = (self.ptt, self.tx)
        if ev == "EAR_PTT_DOWN":
            self.ptt = True
        elif ev == "EAR_PTT_UP":
            self.ptt = False
        elif ev == "EAR_SIDE_SINGLE":
            self.ptt = not self.ptt
        self.tx = self.allow()
        if self.trace and (self.ptt, self.tx) != before:
            self.emit("P", "headset=%d radio=0 type=%d allow=%d tx=%d"
                      % (self.ptt, self.type, self.allow(), self.tx))

    def send(self, text):
        if self.trace:
            self.emit(">", text)
        self.pending.append((time.monotonic() + 0.05, "+OK"))

    def run(self, cmd):
        c = cmd[:1].lower()
        if c == "s":
            self.status()
        elif c == "t":
            self.trace = cmd[1:2] != "0" if len(cmd) > 1 else not self.trace
            self.emit("T", "trace on" if self.trace else "trace off")
            if self.trace:
                self.status()
                if not self.pressed and self.link:
                    self.pressed = True
                    self.schedule_press()
        elif c == "a":
            self.send(cmd[1:])
            self.emit("A", "sent")
        elif c == "i":
            self.emit("I", "injected")
            self.module(cmd[1:])
        elif c == "r":
            self.emit("R", "restarting module")
            now = time.monotonic()
            self.pending += [(now + 0.3, "+IM_READY"), (now + 0.4, "+OK")]
        elif c == "l":
            self.emit("L", " ".join("%02X" % b for b in self.raw) or "(empty)")
        else:
            self.emit("H", "!s status  !t1/!t0 trace  !a<AT> send  "
                           "!i<line> inject  !r restart  !l rx bytes  !? help")

    def schedule_press(self):
        now = time.monotonic()
        press, release = {
            "ptt": ("+IM_EAR_PTT_KEYDOWN", "+IM_EAR_PTT_KEYUP"),
            "side": ("+IM_EAR_SIDE_SINGLE1", "+IM_EAR_SIDE_SINGLE0"),
            "unknown": ("+IM_EAR_KEY_SHORT", None),
            "none": (None, None),
        }[self.button]
        if press:
            self.pending.append((now + 0.6, press))
        if release:
            self.pending.append((now + 1.6, release))

    def feed(self, data):
        for b in data:
            ch = chr(b)
            self.out("\n" if ch in "\r\n" else ch)       # the console's echo
            if self.line is None:
                if ch == "!":
                    self.line = ""
                continue
            if ch in "\r\n":
                cmd, self.line = self.line, None
                self.run(cmd)
            elif ch == "\x1b":
                self.line = None
            elif ch >= " ":
                self.line += ch

    def tick(self):
        now = time.monotonic()
        due = [p for p in self.pending if p[0] <= now]
        self.pending = [p for p in self.pending if p[0] > now]
        for _, text in sorted(due):
            self.module(text)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pty", help="write the pty slave path here")
    ap.add_argument("--button", choices=["ptt", "side", "unknown", "none"],
                    default="ptt")
    ap.add_argument("--ptt-type", type=int, choices=[0, 1, 2], default=0)
    ap.add_argument("--unlinked", action="store_true",
                    help="BT on but no headset linked")
    a = ap.parse_args(argv)

    master, slave = os.openpty()
    name = os.ttyname(slave)
    if a.pty:
        with open(a.pty, "w") as f:
            f.write(name + "\n")
    else:
        print(name, flush=True)

    sim = Sim(master, a.button, a.ptt_type, not a.unlinked)
    try:
        while True:
            r, _, _ = select.select([master], [], [], 0.02)
            if r:
                sim.feed(os.read(master, 256))
            sim.tick()
    except (KeyboardInterrupt, OSError):
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
