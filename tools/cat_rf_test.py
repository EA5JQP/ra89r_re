#!/usr/bin/env python3
"""CAT + tinySA-MCP receive test for the RA89R.

The radio is tuned and measured **over CAT** (FT-817 frames on USART1, the
USB-C/CH340 port), and the carrier comes from a tinySA driven through the
**tinySA MCP server** (the same server opencode uses; see
`~/Repos/tinySA_mcp`).  For each band it does a carrier-off / carrier-on /
carrier-off triplet and asserts that the CAT S-meter read rises and recovers.

Why this pair:

  * CAT is the only interface that both sets the frequency and reads the
    receive state without the console (`SM` is the S-meter, `STATUS` is the
    port's extra: squelch, lane, scan mode -- `firmware/App/cat.h`).
  * the tinySA MCP is the bench's generator interface; `set_generator` wraps
    the mode/output/level commands and `get_generator_state` reads the CW
    frequency and the stage's level range back.

Safety (read before the first run):
  * the test **never** keys the radio: no PTT frame is ever sent.
  * the tinySA's SMA is also its analyser input (+10 dBm absolute at 0 dB
    attenuation).  Put a **power attenuator** between the radio's antenna jack
    and the tinySA when the radio can ever transmit; `--pad-db` records it so
    the log reports both the generator and the radio-antenna level.
  * the classic tinySA has two output stages: low (up to ~350 MHz) and high
    (from ~240 MHz).  The level ranges differ, so the test picks the stage per
    frequency and validates `--level` against it.

Usage:

    # what is on the bench?
    python3 tools/cat_rf_test.py --ports

    # the default VHF + UHF pair, generator on the tinySA MCP
    python3 tools/cat_rf_test.py --cat-port /dev/ttyUSB0 --tinysa-port /dev/ttyACM1

    # one band, a quieter carrier, 10 dB of external pad
    python3 tools/cat_rf_test.py --cat-port /dev/ttyUSB0 \
        --tinysa-port /dev/ttyACM1 --band vhf --level -40 --pad-db 10

    # no hardware: the CAT side is simulated and the MCP side is the bundled
    # stdio test double (tools/tinysa_mcp_sim.py), yet the real MCP client path
    # (JSON-RPC over stdio) is exercised
    python3 tools/cat_rf_test.py --sim

    # just the MCP client handshake/tools against the test double
    python3 tools/cat_rf_test.py --mcp-selftest

CAT frames used (firmware/App/cat.h, docs/ra89r_carserial.md):

    set freq   BCD(Hz/10) 4 B + 0x01        -> ACK 0x00
    set mode   mode,0,0,0 + 0x07            -> ACK 0x00   (0x08 FM, 0x04 AM)
    read       00 00 00 00 + 0x03           -> BCD + mode
    S-meter    00 00 00 00 + 0x02           -> 00 00 00 rssi 0x02
    STATUS     00 00 00 00 + 0xE1           -> status rssi_lo rssi_hi sql 0xE1
    VFO A/B    vfo 0 0 0 + 0x81             -> ACK 0x00

Exit status: 0 = every case passed, 1 = a check failed, 2 = setup/import error.
Only pyserial is needed for a real run; `--sim` and `--mcp-selftest` are stdlib.
"""
import argparse
import json
import os
import select
import shlex
import statistics
import subprocess
import sys
import time

# --------------------------------------------------------------------------
# test cases
# --------------------------------------------------------------------------

# 145.5 and 446.00625 are the same pair the firmware's 'Q' autodetect and
# tools/rf_harness.py use; 118.0 is an airband (AM) receive check.
CASES = {
    "vhf": {"freq_hz": 145_500_000, "mode": "FM", "mode_byte": 0x08,
            "label": "145.5000 MHz FM"},
    "uhf": {"freq_hz": 446_006_250, "mode": "FM", "mode_byte": 0x08,
            "label": "446.00625 MHz FM"},
    "air": {"freq_hz": 118_000_000, "mode": "AM", "mode_byte": 0x04,
            "label": "118.0000 MHz AM (airband)"},
}

# Classic tinySA output stages (measured; see tools/rf_harness.py).
LOW_OUTPUT_MAX_HZ = 350_000_000
HIGH_OUTPUT_MIN_HZ = 240_000_000
LOW_LEVEL_RANGE = (-76.0, -6.0)
HIGH_LEVEL_RANGE = (-38.0, 13.0)

DEFAULT_LEVELS = {"vhf": -30.0, "uhf": -38.0, "air": -30.0}

CAT_SET_FREQ = 0x01
CAT_SM = 0x02
CAT_READ = 0x03
CAT_SET_MODE = 0x07
CAT_VS = 0x81
CAT_STATUS = 0xE1

CAT_ACK = 0x00
CAT_FRAME_GAP_S = 0.15          # the parser needs a burst gap (console shares the port)
CAT_TIMEOUT_S = 0.6

DEFAULT_MCP_CMD = ["uv", "--directory", os.path.expanduser("~/Repos/tinySA_mcp"),
                   "run", "tinySA_Operator.py"]
FAKE_MCP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "tinysa_mcp_sim.py")


class TestError(Exception):
    pass


# --------------------------------------------------------------------------
# CAT side
# --------------------------------------------------------------------------

def encode_freq_bcd(freq_hz):
    """145.5e6 -> b'\\x14\\x55\\x00\\x00' (Hz/10, two decimal digits per byte)."""
    freq_10hz = freq_hz // 10
    digits = "%08d" % freq_10hz
    return bytes((int(digits[i * 2]) << 4) | int(digits[i * 2 + 1])
                 for i in range(4))


def decode_freq_bcd(bcd):
    freq_10hz = 0
    for b in bcd[:4]:
        freq_10hz = freq_10hz * 100 + ((b >> 4) & 0x0F) * 10 + (b & 0x0F)
    return freq_10hz * 10


class CatLink:
    """FT-817 CAT over the radio's serial port."""

    def __init__(self, port, baud=115200, log=print):
        self.port = port
        self.baud = baud
        self.log = log
        self.ser = None

    def open(self):
        try:
            import serial
        except ImportError:
            raise TestError("pyserial is required for a real run "
                            "(python3 -m pip install pyserial)")
        self.ser = serial.Serial(self.port, self.baud, timeout=CAT_TIMEOUT_S)
        # Opening the CH340 can leave a byte or two of console/earlier traffic
        # in flight; let the line settle and start from a clean buffer.
        time.sleep(0.4)
        self.ser.reset_input_buffer()

    def close(self):
        if self.ser is not None:
            self.ser.close()
            self.ser = None

    def _xfer(self, payload4, cmd, reply_len, expect_ack=False,
              expect_tail=True):
        frame = bytes(payload4[:4]).ljust(4, b"\x00") + bytes([cmd])
        self.ser.reset_input_buffer()
        self.ser.write(frame)
        self.ser.flush()
        time.sleep(CAT_FRAME_GAP_S)
        if expect_ack:
            # The console shares this port, so skip any console bytes that were
            # still in flight and take the ACK (0x00) when it arrives.
            got = b""
            deadline = time.time() + CAT_TIMEOUT_S
            while time.time() < deadline and len(got) < 16:
                b = self.ser.read(1)
                if not b:
                    continue
                got += b
                if b == bytes([CAT_ACK]):
                    return b
            raise TestError(f"CAT 0x{cmd:02X}: expected ACK, got "
                            f"{got.hex() or '(nothing)'}")
        reply = self.ser.read(reply_len)
        if len(reply) != reply_len:
            raise TestError(f"CAT 0x{cmd:02X}: expected {reply_len} bytes, got "
                            f"{reply.hex() or '(nothing)'}")
        if expect_tail and reply[-1] != cmd:
            raise TestError(f"CAT 0x{cmd:02X}: reply ends with 0x{reply[-1]:02X}"
                            f" ({reply.hex()})")
        return reply

    def set_freq(self, freq_hz):
        self._xfer(encode_freq_bcd(freq_hz), CAT_SET_FREQ, 1, expect_ack=True)

    def set_mode(self, mode_byte):
        self._xfer(bytes([mode_byte, 0, 0, 0]), CAT_SET_MODE, 1, expect_ack=True)

    def select_vfo(self, vfo):
        self._xfer(bytes([vfo & 1, 0, 0, 0]), CAT_VS, 1, expect_ack=True)

    def read_freq_mode(self):
        # READ replies with 4 BCD bytes plus the *mode* byte, not the command.
        reply = self._xfer(bytes(4), CAT_READ, 5, expect_tail=False)
        return decode_freq_bcd(reply[:4]), reply[4]

    def sm(self):
        """The CAT S-meter byte (the lane's raw RSSI clamped to 255)."""
        reply = self._xfer(bytes(4), CAT_SM, 5)
        return reply[3]

    def status(self):
        reply = self._xfer(bytes(4), CAT_STATUS, 5)
        status, lo, hi, sql = reply[0], reply[1], reply[2], reply[3]
        return {"squelch_open": bool(status & 0x01),
                "lane_4815": bool(status & 0x02),
                "scan_mode": (status >> 2) & 0x3F,
                "rssi": lo | (hi << 8),
                "sql_level": sql}

    def sm_median(self, samples, gap=0.20):
        values = []
        for _ in range(samples):
            values.append(self.sm())
            time.sleep(gap)
        return statistics.median(values)


class FakeCat:
    """The same interface, no serial: proves the test's logic offline.

    The carrier flag is set by the test right after it commands the generator
    (the two sides are independent on the bench too)."""

    def __init__(self, log=print):
        self.log = log
        self.freq = 145_500_000
        self.mode = 0x08
        self.carrier = False
        self.vfo = 0

    def open(self):
        pass

    def close(self):
        pass

    def set_freq(self, freq_hz):
        self.freq = freq_hz

    def set_mode(self, mode_byte):
        self.mode = mode_byte

    def select_vfo(self, vfo):
        self.vfo = vfo & 1

    def read_freq_mode(self):
        return self.freq, self.mode

    def sm(self):
        # ~40 counts of noise, ~120 with the carrier: the same order of delta a
        # real bench shows (reg 0x67 is ~200 quiet..232 with a carrier).
        level = 120 if self.carrier else 40
        return level

    def status(self):
        return {"squelch_open": self.carrier, "lane_4815": False,
                "scan_mode": 0, "rssi": self.sm(), "sql_level": 3}

    def sm_median(self, samples, gap=0.20):
        values = [self.sm() for _ in range(samples)]
        return statistics.median(values)


# --------------------------------------------------------------------------
# tinySA MCP client (JSON-RPC over stdio, stdlib only)
# --------------------------------------------------------------------------

class McpError(TestError):
    pass


class McpClient:
    def __init__(self, cmd, timeout=25.0, log=print):
        self.cmd = cmd
        self.timeout = timeout
        self.log = log
        self.proc = None
        self.next_id = 1

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.close()

    def start(self):
        self.proc = subprocess.Popen(
            self.cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        params = {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "ra89r-cat-rf-test", "version": "1.0"},
        }
        self._request("initialize", params)
        self._notify("notifications/initialized")

    def close(self):
        if self.proc is None:
            return
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=2)
        except Exception:
            self.proc.kill()
        self.proc = None

    def _send(self, obj):
        self.proc.stdin.write(json.dumps(obj, separators=(",", ":")) + "\n")
        self.proc.stdin.flush()

    def _notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self._send(msg)

    def _read_message(self):
        r, _, _ = select.select([self.proc.stdout], [], [], self.timeout)
        if not r:
            err = ""
            try:
                err = self.proc.stderr.read(4000)
            except Exception:
                pass
            raise McpError(f"no MCP reply in {self.timeout:.0f}s; stderr: {err!r}")
        line = self.proc.stdout.readline()
        if not line:
            raise McpError("MCP server closed its stdout")
        return json.loads(line)

    def _request(self, method, params=None):
        msg_id = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": msg_id, "method": method}
        if params is not None:
            msg["params"] = params
        self._send(msg)
        while True:
            reply = self._read_message()
            if reply.get("id") != msg_id:
                continue                    # a notification or an old reply
            if "error" in reply:
                raise McpError(f"{method}: {reply['error']}")
            return reply.get("result", {})

    def call_tool(self, name, **arguments):
        result = self._request("tools/call",
                               {"name": name, "arguments": arguments})
        text = ""
        for item in result.get("content", []):
            if item.get("type") == "text":
                text += item.get("text", "")
        if result.get("isError"):
            raise McpError(f"{name} failed: {text or result}")
        try:
            return json.loads(text) if text else {}
        except json.JSONDecodeError:
            return {"raw": text}

    # the four tools this test needs
    def set_generator(self, port, output, frequency_hz=None, level_dbm=None,
                      high_output=False, modulation=None):
        return self.call_tool("set_generator", port=port, output=output,
                              frequency_hz=frequency_hz, level_dbm=level_dbm,
                              high_output=high_output, modulation=modulation)

    def generator_state(self, port):
        return self.call_tool("get_generator_state", port=port)

    def disconnect(self):
        try:
            return self.call_tool("disconnect")
        except McpError:
            return {}


# --------------------------------------------------------------------------
# the test itself
# --------------------------------------------------------------------------

def stage_for(freq_hz):
    high = freq_hz >= HIGH_OUTPUT_MIN_HZ
    return high, (HIGH_LEVEL_RANGE if high else LOW_LEVEL_RANGE)


def run_case(cat, mcp, tinysa_port, key, case, level, pad_db,
             samples, min_delta, max_recovery, log):
    freq = case["freq_hz"]
    high, level_range = stage_for(freq)
    if not (level_range[0] <= level <= level_range[1]):
        raise TestError(
            f"{key}: level {level} dBm is outside the "
            f"{'high' if high else 'low'} output range {level_range[0]}.."
            f"{level_range[1]} dBm")

    log(f"\n[{key}] {case['label']}: generator {level} dBm "
        f"({'high' if high else 'low'} stage)"
        + (f", {pad_db} dB pad -> {level - pad_db:.1f} dBm at the antenna"
           if pad_db else ""))

    cat.set_freq(freq)
    cat.set_mode(case["mode_byte"])
    time.sleep(0.4)
    tuned, mode = cat.read_freq_mode()
    if abs(tuned - freq) > 100_000:          # 10 Hz units are exact; allow 100 kHz
        raise TestError(f"{key}: radio reports {tuned} Hz, wanted {freq} Hz")
    log(f"  radio: {tuned/1e6:.5f} MHz, mode byte 0x{mode:02X}, "
        f"status {cat.status()}")

    # Baseline: generator off, then carrier on, then off again.
    mcp.set_generator(tinysa_port, False, frequency_hz=freq,
                      level_dbm=level, high_output=high)
    if hasattr(cat, "carrier"):
        cat.carrier = False
    time.sleep(0.3)
    quiet = cat.sm_median(samples)
    log(f"  carrier off -> S-meter {quiet:.0f}")

    mcp.set_generator(tinysa_port, True, frequency_hz=freq, level_dbm=level,
                      high_output=high)
    state = mcp.generator_state(tinysa_port)
    # The classic tinySA has no query for output/mode, so the real server keeps
    # them in a `tracked` block; the test double reports them at the top level.
    tracked = state.get("tracked") or {}
    output_on = state.get("output", tracked.get("output_on"))
    if not output_on:
        raise TestError(f"{key}: generator state says output is off: {state}")
    if hasattr(cat, "carrier"):
        cat.carrier = True
    time.sleep(0.5)
    loud = cat.sm_median(samples)
    log(f"  carrier on  -> S-meter {loud:.0f}")

    mcp.set_generator(tinysa_port, False, frequency_hz=freq, level_dbm=level,
                      high_output=high)
    if hasattr(cat, "carrier"):
        cat.carrier = False
    time.sleep(0.5)
    after = cat.sm_median(samples)
    log(f"  carrier off -> S-meter {after:.0f}")

    delta = loud - quiet
    recovery = after - quiet
    ok_open = delta >= min_delta
    ok_close = recovery <= max_recovery
    verdict = "PASS" if (ok_open and ok_close) else "FAIL"
    log(f"  delta {delta:+.0f} (need >= {min_delta}), "
        f"recovery {recovery:+.0f} (need <= {max_recovery}) -> {verdict}")
    return {"case": key, "freq_hz": freq, "quiet": quiet, "loud": loud,
            "after": after, "delta": delta, "recovery": recovery,
            "ok": ok_open and ok_close}


def mcp_selftest(log):
    log("MCP self-test against tools/tinysa_mcp_sim.py")
    cmd = [sys.executable, FAKE_MCP]
    with McpClient(cmd, log=log) as mcp:
        state = mcp.set_generator("/dev/fake", True, frequency_hz=145_500_000,
                                  level_dbm=-30.0, high_output=False)
        log(f"  set_generator: {state}")
        state = mcp.generator_state("/dev/fake")
        log(f"  get_generator_state: {state}")
        mcp.disconnect()
    log("MCP self-test: PASS")


def list_ports(log):
    try:
        from serial.tools import list_ports as lp
    except ImportError:
        raise TestError("pyserial is required for --ports")
    found = False
    for p in sorted(lp.comports(), key=lambda p: p.device):
        if p.device.startswith("/dev/ttyS"):
            continue                    # motherboard UARTs: not a USB device
        found = True
        hint = ""
        text = (p.description or "") + " " + (p.manufacturer or "")
        if "CH340" in text or "1a86" in (p.hwid or "").lower():
            hint = "  <- likely the radio CAT/console port (CH340)"
        elif "tinySA" in text or "0483" in (p.hwid or "").lower():
            hint = "  <- likely the tinySA"
        log(f"  {p.device:20s} {p.description}{hint}")
    if not found:
        log("  (no serial ports found)")
    return found


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="CAT + tinySA-MCP receive test for the RA89R")
    ap.add_argument("--cat-port", help="radio serial port (USB-C/CH340)")
    ap.add_argument("--cat-baud", type=int, default=115200)
    ap.add_argument("--tinysa-port", help="tinySA serial port (e.g. /dev/ttyACM1)")
    ap.add_argument("--mcp-cmd", default=None,
                    help="MCP server command (default: uv --directory "
                         "~/Repos/tinySA_mcp run tinySA_Operator.py)")
    ap.add_argument("--band", choices=("vhf", "uhf", "air", "all"), default="all",
                    help="which band(s); 'all' = vhf + uhf (air is opt-in)")
    ap.add_argument("--level", type=float, default=None,
                    help="generator level in dBm (default: -30 VHF/air, -38 UHF)")
    ap.add_argument("--pad-db", type=float, default=0.0,
                    help="external attenuator between radio and tinySA, dB")
    ap.add_argument("--samples", type=int, default=5,
                    help="S-meter reads per phase (median)")
    ap.add_argument("--min-delta", type=float, default=10.0,
                    help="minimum S-meter rise for a carrier (default 10)")
    ap.add_argument("--max-recovery", type=float, default=8.0,
                    help="maximum S-meter rise left after the carrier is off")
    ap.add_argument("--sim", action="store_true",
                    help="no hardware: fake CAT + the bundled MCP test double")
    ap.add_argument("--mcp-selftest", action="store_true",
                    help="exercise the MCP client against the bundled test double")
    ap.add_argument("--ports", action="store_true", help="list serial ports and exit")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan; touch no hardware")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args(argv)

    def log(*msg):
        if not args.quiet:
            print(*msg)

    if args.ports:
        list_ports(log)
        return 0

    if args.mcp_selftest:
        try:
            mcp_selftest(log)
            return 0
        except TestError as exc:
            print(f"FAIL: {exc}", file=sys.stderr)
            return 1

    # `all` is the two ham bands; 118 MHz (AM) is explicit because it is an
    # airband receive check, not a general one.
    bands = (args.band,) if args.band != "all" else ("vhf", "uhf")

    mcp_cmd = shlex.split(args.mcp_cmd) if args.mcp_cmd else DEFAULT_MCP_CMD
    if args.sim:
        mcp_cmd = [sys.executable, FAKE_MCP]
    elif not os.path.isdir(DEFAULT_MCP_CMD[2]) and args.mcp_cmd is None:
        print("the tinySA MCP repository is not at ~/Repos/tinySA_mcp; "
              "pass --mcp-cmd", file=sys.stderr)
        return 2

    if args.dry_run:
        log("plan (no hardware touched):")
        log(f"  MCP: {' '.join(mcp_cmd)}")
        log(f"  CAT: {'simulated' if args.sim else (args.cat_port or '<missing --cat-port>')}"
            f" @ {args.cat_baud} 8N1")
        for key in bands:
            case = CASES[key]
            high, rng = stage_for(case["freq_hz"])
            level = args.level if args.level is not None else DEFAULT_LEVELS[key]
            log(f"  {key:3s} {case['label']}: {level} dBm "
                f"{'high' if high else 'low'} stage, "
                f"delta >= {args.min_delta:g}, recovery <= {args.max_recovery:g}")
        return 0

    if not args.sim and not args.cat_port:
        print("--cat-port is required for a real run (see --ports)", file=sys.stderr)
        return 2
    if not args.sim and not args.tinysa_port:
        print("--tinysa-port is required for a real run (see --ports)",
              file=sys.stderr)
        return 2

    tinysa_port = args.tinysa_port or ("/dev/fake" if args.sim else None)
    cat = FakeCat(log=log) if args.sim else CatLink(args.cat_port, args.cat_baud, log)
    results = []
    mcp = McpClient(mcp_cmd, log=log)
    try:
        log(f"MCP: {' '.join(mcp_cmd)}")
        mcp.start()
        cat.open()
        original = None
        try:
            original = cat.read_freq_mode()      # put the radio back afterwards
        except TestError:
            original = None
        try:
            for key in bands:
                level = args.level if args.level is not None else DEFAULT_LEVELS[key]
                results.append(run_case(cat, mcp, tinysa_port, key, CASES[key],
                                        level, args.pad_db, args.samples,
                                        args.min_delta, args.max_recovery, log))
        finally:
            try:
                if tinysa_port:
                    mcp.set_generator(tinysa_port, False)
            except TestError:
                pass
            if original is not None:
                try:
                    cat.set_freq(original[0])
                    cat.set_mode(original[1])
                    log(f"\nrestored the radio to {original[0]/1e6:.5f} MHz, "
                        f"mode 0x{original[1]:02X}")
                except TestError:
                    pass
            cat.close()
    except TestError as exc:
        print(f"\nFAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        mcp.close()

    log("\nsummary")
    log(f"  {'case':4s} {'quiet':>6s} {'on':>5s} {'off':>5s} "
        f"{'delta':>6s} {'recovery':>9s}  verdict")
    failed = 0
    for r in results:
        log(f"  {r['case']:4s} {r['quiet']:6.0f} {r['loud']:5.0f} {r['after']:5.0f} "
            f"{r['delta']:+6.0f} {r['recovery']:+9.0f}  "
            f"{'PASS' if r['ok'] else 'FAIL'}")
        if not r["ok"]:
            failed += 1
    log(f"  {len(results) - failed}/{len(results)} cases passed")
    return 0 if failed == 0 and results else 1


if __name__ == "__main__":
    sys.exit(main())
