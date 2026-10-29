#!/usr/bin/env python3
"""Debug the RA89R's Bluetooth module (YBT100) from a PC, through the radio.

The firmware's UART console (USART1, 115200 8N1 -- the Kenwood jack or the
USB-C/CH340 port) carries a Bluetooth debug channel: lines that start with '!'
(firmware/App/app/bt_debug.h).  Every reply is a line `BT <ms> <kind> <body>`,
so this tool picks them out of whatever else the console prints.  It can:

    python3 tools/ra89r_bt.py --port /dev/ttyUSB0 status
    python3 tools/ra89r_bt.py --port /dev/ttyUSB0 trace --log /tmp/bt.log
    python3 tools/ra89r_bt.py --port /dev/ttyUSB0 send 'AT+CONN_STATE?'
    python3 tools/ra89r_bt.py --port /dev/ttyUSB0 inject '+IM_EAR_PTT_KEYDOWN'
    python3 tools/ra89r_bt.py --port /dev/ttyUSB0 restart
    python3 tools/ra89r_bt.py --port /dev/ttyUSB0 ptt-test
    python3 tools/ra89r_bt.py --port /dev/ttyUSB0 shell

  status    one status line: enabled, state, link, SCO, the headset/radio PTT
            flags, the PTT type and whether it lets the source key TX, TX
  trace     stream every line the module sends (with the event the firmware's
            parser made of it -- [NONE] means no prefix matched), every
            command sent to it, and every PTT/TX transition
  send      a raw AT command to the module, then show what comes back
  inject    feed a line to the firmware as if the module had sent it: the PTT
            path can be exercised with no headset at all
  ptt-test  guided: press the headset's PTT when asked; the tool says where
            the chain module -> parser -> PTT flag -> PTT type -> TX stops
  shell     interactive: `s` status, `t1`/`t0` trace, `AT...` send,
            `+IM...` inject, `r` restart, `l` raw bytes, `q` quit
  sniff     no firmware involved: read a USB-UART adapter tapped onto the
            module's own TX line (PB11 side, 115200) and classify each line

`--selftest` runs the commands against tools/ra89r_bt_console_sim.py, the
console's test double, so the tool is checked with no radio attached.

Only pyserial is needed (the same dependency as tools/ra89r_flash.py).  See
docs/ra89r_bt_debug.md.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover -- the sim imports this file for its table
    serial = list_ports = None


def need_serial():
    if serial is None:
        sys.exit("pyserial is required: python3 -m pip install pyserial")

BAUD = 115200
RECORD_RE = re.compile(r"^BT (\d+) (\S)(?: (.*))?$")

# The firmware's parser table (firmware/App/driver/bluetooth.c, the stock's
# FUN_08022974 order): first matching prefix wins.  Keep in step with it --
# `sniff` classifies with this, and the sim uses it to act like the firmware.
EVENT_PREFIXES = [
    ("RDTP", "BINARY_FRAME"),
    ("+IM_READY", "READY"),
    ("+OK", "OK"),
    ("+ERROR", "ERROR"),
    ("+IM_VERSION:", "VERSION"),
    ("+IM_BT_EMITTER", "BT_EMITTER"),
    ("+IM_NAME_EQUALLY", "NAME_EQUALLY"),
    ("+IM_BT_RECEIVER", "BT_RECEIVER"),
    ("+IM_BLE_MASTER", "BLE_MASTER"),
    ("+IM_BLE_SLAVE", "BLE_SLAVE"),
    ("+IM_CONN_STATE:", "CONN_STATE"),
    ("+IM_SCO_CONN", "SCO_CONN"),
    ("+IM_CALL_CONED", "CALL_CONNECTED"),
    ("+IM_CALL_DISCONED", "CALL_DISCONNECTED"),
    ("+IM_SCO_DISCN", "SCO_DISCONNECT"),
    ("+IM_EARDEV:", "EARDEV"),
    ("+IM_BT_SCAN_STOP", "BT_SCAN_STOP"),
    ("+IM_BT_EAR_CONN", "BT_EAR_CONN"),
    ("+IM_BT_DISCN", "BT_DISCONNECT"),
    ("+IM_EAR_PTT_KEYDOWN", "EAR_PTT_DOWN"),
    ("+IM_EAR_PTT_KEYUP", "EAR_PTT_UP"),
    ("+IM_BLE_LOCAL", "BLE_LOCAL"),
    ("+IM_BT_LOCAL", "BT_LOCAL"),
    ("+IM_EAR_SIDE_SINGLE1", "EAR_SIDE_SINGLE"),
    ("+IM_EAR_VOL=", "EAR_VOL"),
]

# Events that move the headset PTT flag in the firmware (app/bt.c).
PTT_EVENTS = {"EAR_PTT_DOWN", "EAR_PTT_UP", "EAR_SIDE_SINGLE"}
PTT_TYPES = {0: "BT", 1: "Local", 2: "Both"}


def classify(line):
    """The event the firmware's parser makes of a module line."""
    if line.startswith("++"):
        line = line[1:]
    for prefix, event in EVENT_PREFIXES:
        if line.startswith(prefix):
            return event
    return "NONE"


class Record:
    """One `BT <ms> <kind> <body>` line from the firmware."""

    def __init__(self, ms, kind, body):
        self.ms, self.kind, self.body = ms, kind, body

    def __repr__(self):
        return "BT %d %s %s" % (self.ms, self.kind, self.body)

    @property
    def fields(self):
        """`key=value` pairs of an S or P line."""
        return parse_fields(self.body)

    @property
    def module_line(self):
        """For a '<' record: (text, event)."""
        m = re.match(r"^(.*) \[([A-Z_?]+)\]$", self.body)
        return (m.group(1), m.group(2)) if m else (self.body, "NONE")


def parse_record(line):
    m = RECORD_RE.match(line.strip("\r\n"))
    if not m:
        return None
    return Record(int(m.group(1)), m.group(2), m.group(3) or "")


def parse_fields(body):
    out = {}
    for tok in body.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = int(v) if v.lstrip("-").isdigit() else v
    return out


def describe_status(st):
    """Human reading of an S line's fields."""
    lines = []
    if not st.get("en"):
        lines.append("Bluetooth is OFF (BT menu: Switch)")
    lines.append("module state %s, %d byte(s) received, version %s"
                 % (st.get("state"), st.get("rx", 0), st.get("ver")))
    lines.append("headset %s, SCO/call %s%s"
                 % ("linked" if st.get("link") else "NOT linked",
                    "up" if st.get("sco") else "down",
                    " (CALL=ON waits for the next RX transition)"
                    if st.get("resume") else ""))
    t = st.get("type", 0)
    lines.append("PTT type %s: headset flag %d, radio key %d -> keys TX: %s; "
                 "TX %s" % (PTT_TYPES.get(t, t), st.get("ptt", 0),
                            st.get("radio_ptt", 0),
                            "yes" if st.get("allow") else "no",
                            "ON" if st.get("tx") else "off"))
    return lines


def diagnose(status, records):
    """Where the headset PTT chain stopped, from a status and the trace
    records captured while the button was pressed.  Returns (ok, [lines])."""
    out = []
    if status is not None:
        if not status.get("en"):
            return False, ["Bluetooth is off -- turn the BT Switch on first."]
        if not status.get("link"):
            return False, ["No headset is linked (link=0) -- pair/connect it "
                           "first; the module reports no button without a "
                           "link."]
    rx = [r for r in records if r.kind == "<"]
    ptt_lines = [r for r in rx if r.module_line[1] in PTT_EVENTS]
    unknown = [r for r in rx if r.module_line[1] == "NONE"]
    ps = [r.fields for r in records if r.kind == "P"]

    if not rx:
        out.append("The module sent NOTHING while the button was pressed.")
        if status is not None and not status.get("sco"):
            out.append("SCO is down (sco=0).  The stock opens the call on a "
                       "PTT press (AT+SPKGAIN, then AT+BT_CALL=ON when SCO "
                       "is down: 0x0801A75C/0x0801A774); try "
                       "`send AT+BT_CALL=ON` and press again.")
        else:
            out.append("Check the headset is the linked one and that its "
                       "button is a PTT button for this module.")
        return False, out

    for r in unknown:
        out.append("module line not recognised by the parser: %r"
                   % r.module_line[0])
    if not ptt_lines:
        out.append("The module talked, but sent no PTT event the firmware "
                   "acts on (%s)." % ", ".join(sorted(PTT_EVENTS)))
        if unknown:
            out.append("One of the unrecognised lines above is probably the "
                       "button; the parser needs a prefix for it.")
        return False, out

    out.append("PTT event(s) from the module: %s"
               % ", ".join("%s [%s]" % r.module_line for r in ptt_lines))
    if not any(p.get("headset") for p in ps):
        out.append("...but the headset PTT flag never went to 1 -- the "
                   "service did not act on the event (app/bt.c).")
        return False, out
    keyed = [p for p in ps if p.get("headset")]
    if not any(p.get("allow") for p in keyed):
        t = keyed[0].get("type", 0)
        out.append("The headset flag went to 1, but PTT type %s does not let "
                   "the headset key TX (allow=0).  Set BT menu PTT to BT or "
                   "Both." % PTT_TYPES.get(t, t))
        return False, out
    if not any(p.get("tx") for p in ps):
        out.append("The PTT decision allowed TX (allow=1) but the measured "
                   "TX chain never keyed (tx=0): look at tx_poll_ptt().")
        return False, out
    out.append("TX keyed from the headset.")
    if ps and ps[-1].get("headset"):
        out.append("Note: the headset flag was still set at the end -- the "
                   "button toggles (SIDE_SINGLE1) or the release was missed.")
    return True, out


class Console:
    """The radio's console, seen as a stream of BT records."""

    def __init__(self, port, baud=BAUD, echo_other=False):
        need_serial()
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.buf = b""
        self.echo_other = echo_other

    def close(self):
        self.ser.close()

    def write_line(self, text):
        """Send `!<text>` + CR.  Paced: the console queues 64 bytes and drains
        one per main-loop pass, which a panel blit can stretch to ms."""
        for ch in ("!" + text + "\r").encode("ascii", "replace"):
            self.ser.write(bytes([ch]))
            self.ser.flush()
            time.sleep(0.003)

    def lines(self, timeout):
        """Yield console lines for up to `timeout` s (None = forever)."""
        end = None if timeout is None else time.monotonic() + timeout
        while end is None or time.monotonic() < end:
            chunk = self.ser.read(256)
            if chunk:
                self.buf += chunk
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                yield raw.decode("ascii", "replace").rstrip("\r")

    def records(self, timeout, until=None):
        """Collect BT records for up to `timeout` s, or until `until(rec)`."""
        got = []
        for line in self.lines(timeout):
            rec = parse_record(line)
            if rec is None:
                if self.echo_other and line.strip():
                    print("   | " + line)
                continue
            got.append(rec)
            if until and until(rec):
                break
        return got

    def command(self, text, kind, timeout=2.0):
        """Send a '!' line and wait for the first record of `kind`."""
        self.write_line(text)
        got = self.records(timeout, until=lambda r: r.kind == kind)
        hit = [r for r in got if r.kind == kind]
        return (hit[0] if hit else None), got

    def status(self):
        rec, _ = self.command("s", "S")
        if rec is None:
            raise SystemExit("no 'BT ... S' reply -- is this the radio console "
                             "at 115200, running a firmware with the BT debug "
                             "console (`!?` should answer)?")
        return rec.fields


def print_record(rec, t0=None, log=None):
    stamp = "%8.3f" % ((rec.ms - t0) / 1000.0) if t0 is not None else "%8d" % rec.ms
    if rec.kind == "<":
        text, ev = rec.module_line
        line = "%s  module -> %-30s [%s]" % (stamp, text, ev)
    elif rec.kind == ">":
        line = "%s  radio  -> %s" % (stamp, rec.body)
    elif rec.kind == "P":
        f = rec.fields
        line = ("%s  PTT    headset=%s radio=%s type=%s allow=%s tx=%s"
                % (stamp, f.get("headset"), f.get("radio"),
                   PTT_TYPES.get(f.get("type"), f.get("type")),
                   f.get("allow"), f.get("tx")))
    else:
        line = "%s  %s %s" % (stamp, rec.kind, rec.body)
    print(line)
    if log:
        log.write(repr(rec) + "\n")
        log.flush()


# ------------------------------------------------------------------ commands

def cmd_status(con, _a):
    st = con.status()
    for line in describe_status(st):
        print(line)
    return 0


def cmd_trace(con, a):
    log = open(a.log, "a") if a.log else None
    con.write_line("t1")
    t0 = None
    print("tracing%s -- Ctrl-C to stop" % (" for %gs" % a.seconds if a.seconds else ""))
    try:
        for line in con.lines(a.seconds or None):
            rec = parse_record(line)
            if rec is None:
                continue
            if t0 is None:
                t0 = rec.ms
            print_record(rec, t0, log)
    except KeyboardInterrupt:
        pass
    finally:
        con.write_line("t0")
        if log:
            log.close()
    return 0


def cmd_send(con, a):
    con.write_line("t1")
    con.records(0.3)
    con.write_line("a" + a.text)
    for rec in con.records(a.wait):
        if rec.kind in "<>A":
            print_record(rec)
    con.write_line("t0")
    return 0


def cmd_inject(con, a):
    con.write_line("t1")
    con.records(0.3)
    con.write_line("i" + a.line)
    for rec in con.records(a.wait):
        print_record(rec)
    con.write_line("t0")
    return 0


def cmd_restart(con, a):
    con.write_line("t1")
    con.records(0.3)
    con.write_line("r")
    for rec in con.records(a.wait):
        print_record(rec)
    con.write_line("t0")
    st = con.status()
    for line in describe_status(st):
        print(line)
    return 0


def cmd_ptt_test(con, a):
    st = con.status()
    print("before:")
    for line in describe_status(st):
        print("  " + line)
    if not st.get("en") or not st.get("link"):
        ok, verdict = diagnose(st, [])
        print("\n".join(verdict))
        return 1

    con.write_line("t1")
    con.records(0.3)
    print("\nPress and HOLD the headset PTT for ~2 s, then release.  "
          "Listening for %gs..." % a.seconds)
    recs = [r for r in con.records(a.seconds) if r.kind in "<>P"]
    con.write_line("t0")
    t0 = recs[0].ms if recs else None
    for rec in recs:
        print_record(rec, t0)
    after = con.status()
    ok, verdict = diagnose(st, recs)
    print("\nverdict:")
    for line in verdict:
        print("  " + line)
    if after.get("tx"):
        print("  WARNING: the radio is still transmitting (tx=1) -- press the "
              "headset button again or use the radio's PTT to release it.")
    return 0 if ok else 1


SHELL_HELP = """\
  s            status            t1 / t0   trace on / off
  AT...        send to module    +IM...    inject as if from the module
  r            restart module    l         last raw bytes
  !<line>      any debug line    q         quit"""


def cmd_shell(con, _a):
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            for rec in con.records(0.2):
                print_record(rec)

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    print(SHELL_HELP)
    try:
        while True:
            try:
                text = input("bt> ").strip()
            except EOFError:
                break
            if not text:
                continue
            if text in ("q", "quit", "exit"):
                break
            if text.startswith("!"):
                con.write_line(text[1:])
            elif text.upper().startswith("AT"):
                con.write_line("a" + text)
            elif text.startswith("+"):
                con.write_line("i" + text)
            elif text in ("s", "t1", "t0", "r", "l", "?"):
                con.write_line(text)
            else:
                print(SHELL_HELP)
    except KeyboardInterrupt:
        pass
    finally:
        con.write_line("t0")
        stop.set()
        th.join(1.0)
    return 0


def cmd_sniff(a):
    need_serial()
    ser = serial.Serial(a.port, a.baud, timeout=0.1)
    buf = b""
    t0 = time.monotonic()
    print("sniffing %s at %d -- Ctrl-C to stop" % (a.port, a.baud))
    try:
        while True:
            buf += ser.read(256)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("ascii", "replace").rstrip("\r")
                if text:
                    print("%8.3f  %-34s [%s]" % (time.monotonic() - t0, text,
                                                 classify(text)))
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    return 0


def cmd_ports(_a):
    need_serial()
    for p in list_ports.comports():
        print("%-20s %s" % (p.device, p.description))
    return 0


# ------------------------------------------------------------------ selftest

def selftest():
    """Drive the console test double through a PTY and check every command's
    parse and the ptt-test verdicts.  No radio needed."""
    here = os.path.dirname(os.path.abspath(__file__))
    sim = os.path.join(here, "ra89r_bt_console_sim.py")
    fails = 0

    def check(ok, what):
        nonlocal fails
        print("[selftest] %s %s" % ("ok  " if ok else "FAIL", what))
        fails += 0 if ok else 1

    # parser table and diagnosis, pure
    check(classify("+IM_EAR_SIDE_SINGLE1") == "EAR_SIDE_SINGLE", "classify SIDE_SINGLE1")
    check(classify("+IM_EAR_SIDE_SINGLE0") == "NONE", "classify SIDE_SINGLE0 as NONE")
    check(classify("++OK") == "OK", "classify strips one '+' of '++'")
    r = parse_record("BT 120 < +IM_SCO_CONN [SCO_CONN]")
    check(r and r.module_line == ("+IM_SCO_CONN", "SCO_CONN"), "parse a '<' record")

    scenarios = [
        # (sim args, expect ok, text the verdict must contain)
        (["--button", "ptt"], True, "TX keyed"),
        (["--button", "side"], True, "TX keyed"),
        (["--button", "none"], False, "NOTHING"),
        (["--button", "unknown"], False, "not recognised"),
        (["--button", "ptt", "--ptt-type", "1"], False, "does not let"),
        (["--button", "ptt", "--unlinked"], False, "No headset"),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        for args, want_ok, want_text in scenarios:
            ptyfile = os.path.join(tmp, "pty")
            proc = subprocess.Popen([sys.executable, "-I", sim, "--pty", ptyfile]
                                    + args)
            try:
                for _ in range(50):
                    if os.path.exists(ptyfile) and open(ptyfile).read().strip():
                        break
                    time.sleep(0.05)
                port = open(ptyfile).read().strip()
                con = Console(port)
                st = con.status()
                if "--unlinked" not in args:
                    check(st.get("en") == 1 and st.get("link") == 1,
                          "%s: status parses (en=1 link=1)" % " ".join(args))
                st0 = st
                if st0.get("link"):
                    con.write_line("t1")
                    con.records(0.3)
                    recs = [r for r in con.records(2.5) if r.kind in "<>P"]
                    con.write_line("t0")
                else:
                    recs = []
                ok, verdict = diagnose(st0, recs)
                text = " ".join(verdict)
                check(ok == want_ok and want_text in text,
                      "%s: verdict %s (%s)" % (" ".join(args),
                                               "ok" if ok else "fail",
                                               verdict[-1] if verdict else ""))
                if args == ["--button", "ptt"]:
                    rec, got = con.command("aAT+GMR?", "A")
                    check(rec is not None, "send is acknowledged")
                    con.write_line("t1")
                    con.records(0.3)
                    con.write_line("i+IM_EAR_PTT_KEYUP")
                    got = con.records(1.0)
                    check(any(g.kind == "<" and g.module_line[1] == "EAR_PTT_UP"
                              for g in got), "inject is traced with its event")
                    con.write_line("t0")
                con.close()
            finally:
                proc.terminate()
                proc.wait(2)
                if os.path.exists(ptyfile):
                    os.unlink(ptyfile)
    print("%d failed" % fails)
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", help="the radio console's serial port")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--selftest", action="store_true",
                    help="run against tools/ra89r_bt_console_sim.py")
    ap.add_argument("--show-console", action="store_true",
                    help="also print console lines that are not BT records")
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("ports", help="list serial ports")
    sub.add_parser("status", help="one status read-out")
    p = sub.add_parser("trace", help="stream the module traffic")
    p.add_argument("--seconds", type=float, default=0, help="stop after N s")
    p.add_argument("--log", help="append the raw records to this file")
    p = sub.add_parser("send", help="send a raw AT command to the module")
    p.add_argument("text")
    p.add_argument("--wait", type=float, default=2.0)
    p = sub.add_parser("inject", help="feed a line as if the module sent it")
    p.add_argument("line")
    p.add_argument("--wait", type=float, default=1.0)
    p = sub.add_parser("restart", help="re-run the module bring-up")
    p.add_argument("--wait", type=float, default=4.0)
    p = sub.add_parser("ptt-test", help="guided headset-PTT diagnosis")
    p.add_argument("--seconds", type=float, default=8.0)
    sub.add_parser("shell", help="interactive")
    sub.add_parser("sniff", help="read the module's TX line directly "
                                 "(USB-UART adapter on --port)")
    a = ap.parse_args(argv)

    if a.selftest:
        return selftest()
    if a.cmd is None:
        ap.print_help()
        return 2
    if a.cmd == "ports":
        return cmd_ports(a)
    if not a.port:
        ap.error("--port is required")
    if a.cmd == "sniff":
        return cmd_sniff(a)

    con = Console(a.port, a.baud, echo_other=a.show_console)
    try:
        return {
            "status": cmd_status, "trace": cmd_trace, "send": cmd_send,
            "inject": cmd_inject, "restart": cmd_restart,
            "ptt-test": cmd_ptt_test, "shell": cmd_shell,
        }[a.cmd](con, a)
    finally:
        con.close()


if __name__ == "__main__":
    sys.exit(main())
