#!/usr/bin/env python3
"""RF harness: correlate the RA89R's UART RSSI with a tinySA signal generator.

The radio's USART1 console (PB6/PB7, 115200 8N1 -- the USB-C/CH340 port) already
knows how to talk about this: 'S' samples BK4829 reg 0x67 every 200 ms for 4 s,
'F' cycles the chip-side receive front end, 'B' cycles the MCU band pin PA0, and
'Q' runs a tinySA-aware squelch autodetect on 145.5000 / 446.00625 MHz.  What was
missing is a host that moves the tinySA in step with the radio, so a "carrier on /
carrier off" pair becomes a number and a pin experiment becomes a table.

Everything here is driven over the two serial ports; no firmware change is needed
and the console text is parsed as-is.

    # what is on the bench?
    python3 tools/rf_harness.py ports

    # one band, carrier off then on, with the radio tuned by CAT
    python3 tools/rf_harness.py sample --band vhf

    # which front-end mode actually receives in each band?
    python3 tools/rf_harness.py matrix --csv /tmp/rf_matrix.csv

    # drive the firmware's own 'Q' squelch autodetect end to end
    python3 tools/rf_harness.py squelch

Only pyserial is needed (the same dependency as tools/ra89r_flash.py).

Two hardware facts this script works around, both measured on a classic tinySA:

  * low output covers up to 350 MHz and high output starts at 240 MHz, so VHF
    (145.5) and UHF (446.00625) need *different* output stages -- prepare() picks
    one per frequency and reports the frequency the device actually tuned, which
    it clamps;
  * the stages have different level ranges (low -76..-6 dBm, high -38..+13 dBm),
    so the injected level cannot be held constant across bands.  The defaults are
    deliberately the quietest sensible value per band; use an attenuator and read
    deltas, not absolute levels, when comparing bands.

And one safety fact that is not negotiable: the tinySA's SMA is also its analyser
input, rated +10 dBm absolute maximum, while a 5 W handheld is +37 dBm.  This
script only ever generates and never sends PTT, but it cannot see a press on the
radio's own PTT, so an external power attenuator is what actually protects the
instrument.  Pass it with --pad-db and every reported level is given at both ends.
"""

import argparse
import re
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: python3 -m pip install pyserial")

PROMPT = b"ch> "

# Bench frequencies, in Hz (the same pair the firmware's 'Q' uses).
BANDS = {
    "vhf": 145_500_000,
    "uhf": 446_006_250,
}

# Classic tinySA output coverage: low output tops out around 350 MHz, high output
# starts around 240 MHz (both measured; see rf_harness README notes).
LOW_OUTPUT_MAX_HZ = 350_000_000

# Default injected levels: the quietest value each stage can do, to keep the
# first contact with a receiver gentle.  Override with --vhf-level/--uhf-level.
DEFAULT_LEVELS = {"vhf": -30.0, "uhf": -38.0}

# Stock squelch marks for reg 0x67 (docs/ra89r_bk4829.md): >=0xCF open, <0xB4 quiet.
SQUELCH_OPEN = 0xCF
SQUELCH_CLOSE = 0xB4

# The tinySA Basic's input rating (tinySA wiki, "Specification"): absolute maximum
# input is +10 dBm with 0 dB internal attenuation, +20 dBm short-term peak with
# 30 dB of internal attenuation.  A 5 W handheld is +37 dBm, so an external power
# attenuator is mandatory whenever the radio can key up -- the generator's own
# output path is on the same SMA and is not built for reverse power either.
TINYSA_MAX_INPUT_DBM = 10.0
HANDHELD_TX_DBM = 37.0  # 5 W; override with --radio-tx-dbm

CAT_CMD_SET_FREQ = 0x01
CAT_FRAME_LEN = 5
# The firmware flushes a lone console character once the burst gap has passed;
# wait at least this long between console characters and CAT frames.
CONSOLE_BURST_GAP_S = 0.25


def rssi_to_dbm(value):
    """BK4829 reg 0x67 (9-bit) -> dBm, per bk4819.c: BK4819_GetRSSI_dBm()."""
    return (value / 2.0) - 160.0


def encode_freq_bcd(freq_hz):
    """145.5e6 Hz -> b'\\x14\\x55\\x00\\x00' (Hz/10, two decimal digits per byte).

    Matches cat_freq_10hz_to_bcd() in firmware/App/cat.c and
    encode_freq_bcd() in tools/look4sat_cat.py.
    """
    freq_10hz = freq_hz // 10
    digits = "%08d" % freq_10hz
    return bytes(((int(digits[i * 2]) << 4) | int(digits[i * 2 + 1])) for i in range(4))


# --------------------------------------------------------------------------
# console text -> numbers (regexes mirror the uart_printf() formats in main.c)
# --------------------------------------------------------------------------

RE_RF_HEADER = re.compile(r"RX\s+([\d.]+)\s+MHz,\s*(VHF|UHF),\s*(main|sub)")
RE_SAMPLE = re.compile(r"^\s*(\d+):\s+0x([0-9A-Fa-f]{3})\s+(\d+)\s+(\w+)\s*$", re.M)
RE_S_MINMAX = re.compile(r"min\s+0x([0-9A-Fa-f]+)\s+max\s+0x([0-9A-Fa-f]+)\s+last\s+0x([0-9A-Fa-f]+)")
RE_F_MODE = re.compile(r"front end: mode\s+(\d+)")
RE_F_REG33 = re.compile(r"PA0\s+(high|low),\s*reg 0x33 = 0x([0-9A-Fa-f]+)")
RE_B_MODE = re.compile(r"MCU band pin: mode\s+(\d+)")
RE_B_PA0 = re.compile(r"PA1 high, PA0\s+(high|low)\s*\((VHF|UHF)\)")
RE_Q_BAND = re.compile(r"---\s+(VHF|UHF)\s+([\d.]+)\s+MHz\s+---")
RE_Q_FLOOR = re.compile(r"floor\s*:\s*min\s+0x([0-9A-Fa-f]+)\s+max\s+0x([0-9A-Fa-f]+)\s+mean\s+0x([0-9A-Fa-f]+)")
RE_Q_CARRIER = re.compile(r"carrier:\s*min\s+0x([0-9A-Fa-f]+)\s+max\s+0x([0-9A-Fa-f]+)\s+mean\s+0x([0-9A-Fa-f]+)")
RE_Q_DETECTED = re.compile(r"detected:\s+open\s+0x([0-9A-Fa-f]+),\s*close\s+0x([0-9A-Fa-f]+)")
RE_Q_NOCARRIER = re.compile(r"detected:\s+floor and carrier overlap")
RE_TX_ACTIVE = re.compile(r"transmitting\s+\((\w+)\)\s+([\d.]+)\s+MHz\s+band\s+(VHF|UHF)")
RE_TX_IDLE = re.compile(r"idle -- hold PTT to transmit")


def parse_samples(text):
    """'  00: 0x0EC 236 open' lines -> [(index, hex, dec, class)]."""
    return [(int(i), int(h, 16), int(d), c) for i, h, d, c in RE_SAMPLE.findall(text)]


def parse_rf_header(text):
    """'  RX 145.50000 MHz, VHF, main (>134 MHz split)' -> dict."""
    m = RE_RF_HEADER.search(text)
    if not m:
        return None
    return {"freq_mhz": float(m.group(1)), "band": m.group(2), "branch": m.group(3)}


def parse_s_minmax(text):
    m = RE_S_MINMAX.search(text)
    if not m:
        return None
    return {"min": int(m.group(1), 16), "max": int(m.group(2), 16), "last": int(m.group(3), 16)}


# --------------------------------------------------------------------------
# tinySA generator (the timing/prompt rules that make the device behave)
# --------------------------------------------------------------------------

class TinySA:
    """Minimal generator control for the tinySA console.

    The device echoes commands and terminates every reply with 'ch> ', so reads
    are synchronized on that prompt.  Commands that switch the output stage leave
    the device unresponsive for a few hundred ms; sending the next command too
    early wedges its USB endpoint (and needs an unplug), hence the settle times.
    """

    def __init__(self, port, timeout=3.0, log=print):
        self.port = port
        self.timeout = timeout
        self.log = log
        self.ser = None
        self.stage = None

    # -- plumbing ---------------------------------------------------------
    def open(self):
        self.ser = serial.Serial(self.port, 9600, timeout=self.timeout,
                                 write_timeout=self.timeout)
        self._read_until_prompt(timeout=1.5, idle=0.3)  # startup banner
        time.sleep(0.2)
        if self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)
        return self

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def _read_until_prompt(self, timeout=3.0, idle=1.0):
        start = time.time()
        deadline = start + timeout
        data = bytearray()
        while time.time() < deadline:
            waiting = self.ser.in_waiting
            if waiting:
                data += self.ser.read(waiting)
                if bytes(data).endswith(PROMPT):
                    break
                deadline = min(start + timeout, time.time() + idle)
            else:
                time.sleep(0.02)
        return bytes(data)

    def send(self, command, wait=0.1, settle=0.0, idle=1.0):
        stale = self.ser.in_waiting
        if stale:
            self.log(f"  [tinysa] discarding stale data: "
                     f"{self.ser.read(stale).decode('utf-8', 'replace').strip()!r}")
        self.ser.write((command.strip() + "\r\n").encode())
        time.sleep(wait)
        raw = self._read_until_prompt(timeout=max(self.timeout, wait + 1.0), idle=idle)
        if raw and not raw.endswith(PROMPT):
            raw += self._read_until_prompt(timeout=2.0, idle=idle)
        text = raw.decode("utf-8", "replace")
        lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        while lines and lines[0].strip() == command.strip():
            lines.pop(0)
        while lines and lines[-1].strip() in ("ch>", ""):
            lines.pop()
        if settle:
            time.sleep(settle)
        return "\n".join(lines).strip()

    # -- queries ----------------------------------------------------------
    def level_range(self):
        """Output level range for the current stage, from `level`'s usage line."""
        m = re.search(r"level\s+(-?\d+(?:\.\d+)?)\.\.(-?\d+(?:\.\d+)?)", self.send("level"))
        return (float(m.group(1)), float(m.group(2))) if m else None

    def actual_frequency(self):
        """`sweep` reports what the device really tuned (it clamps)."""
        m = re.match(r"(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)\s+(\d+)", self.send("sweep"))
        return float(m.group(2)) if m else None

    # -- generator --------------------------------------------------------
    def prepare(self, freq_hz, level_dbm, modulation=None):
        """Select the stage that covers freq_hz and set up a CW carrier.

        Returns the frequency the device actually tuned; a mismatch means the
        device clamped the request and the measurement would be meaningless.
        """
        stage = "low" if freq_hz <= LOW_OUTPUT_MAX_HZ else "high"
        if stage != self.stage:
            self.send(f"mode {stage} output", settle=0.5)
            self.stage = stage
        allowed = self.level_range()
        if allowed and not (allowed[0] <= level_dbm <= allowed[1]):
            raise SystemExit(f"level {level_dbm} dBm is outside the {stage} output range "
                             f"{allowed[0]}..{allowed[1]} dBm")
        self.send(f"level {level_dbm:g}", settle=0.3)
        self.send(f"sweep cw {int(round(freq_hz))}", settle=0.3)
        if modulation:
            self.send(f"modulation {modulation}", settle=0.3)
        actual = self.actual_frequency()
        return actual

    def output(self, on):
        self.send("output on" if on else "output off", settle=1.5)

    def restore_input(self):
        """Park the device back in spectrum-analyzer mode."""
        if self.ser is None:
            return
        try:
            self.output(False)
            self.send("mode low input", settle=0.5)
            self.stage = None
        except Exception as exc:  # keep teardown best-effort
            self.log(f"  [tinysa] restore failed: {exc}")


# --------------------------------------------------------------------------
# radio console
# --------------------------------------------------------------------------

class RadioConsole:
    """The RA89R console on USART1: single characters in, text out.

    The same port also carries FT-817 CAT frames; the firmware only accepts a
    frame when its command byte and payload are well formed, and flushes a lone
    character once CONSOLE_BURST_GAP_S has passed, so console characters and CAT
    frames are sent with a gap between the two kinds.
    """

    def __init__(self, port, baud=115200, log=print):
        self.port = port
        self.baud = baud
        self.log = log
        self.ser = None
        self.buf = ""

    def open(self):
        self.ser = serial.Serial(self.port, self.baud, timeout=0.2, write_timeout=1.0)
        self.buf = ""
        self.read_for(0.3)  # drop anything left over from a previous session
        return self

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    # -- raw io -----------------------------------------------------------
    def read_for(self, seconds):
        """Collect whatever arrives within the window (never blocks forever)."""
        end = time.time() + seconds
        out = ""
        while time.time() < end:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                out += chunk.decode("utf-8", "replace")
        self.buf += out
        return out

    def read_until(self, marker, timeout):
        """Consume from the buffer until marker appears; return the text consumed.

        Raises SystemExit on timeout, with whatever was received for context.
        """
        deadline = time.time() + timeout
        while marker not in self.buf:
            if time.time() > deadline:
                raise SystemExit(f"timed out waiting for {marker!r}; got:\n{self.buf[-600:]}")
            self.read_for(0.2)
        idx = self.buf.index(marker) + len(marker)
        taken, self.buf = self.buf[:idx], self.buf[idx:]
        return taken

    def send_char(self, ch):
        self.ser.write(ch.encode())
        self.ser.flush()

    def cat_set_frequency(self, freq_hz):
        """Tune via FT-817 CAT (cat_tune() in cat_radio.c sets the active VFO)."""
        time.sleep(CONSOLE_BURST_GAP_S)  # let the console's burst window close
        self.ser.write(encode_freq_bcd(freq_hz) + bytes([CAT_CMD_SET_FREQ]))
        self.ser.flush()
        self.read_for(0.4)  # ACK (0x00) + echo

    def tx_state(self):
        """Console 'T': the TX path read-out, which does **not** key the radio.

        `tx_diag()` (`main.c:1068`) only reads registers and the PA compare, so it
        is safe to poll as an interlock before the generator is switched on.
        """
        self.buf = ""
        self.send_char("T")
        text = self.read_until("cycles the TX path pin", timeout=4.0)
        active = RE_TX_ACTIVE.search(text)
        if active:
            return {"transmitting": True, "source": active.group(1),
                    "freq_mhz": float(active.group(2)), "band": active.group(3)}
        if RE_TX_IDLE.search(text):
            return {"transmitting": False}
        raise SystemExit(f"'T' gave no TX state:\\n{text[-400:]}")

    # -- console commands -------------------------------------------------
    def sample_rssi(self, label=""):
        """Console 'S': 20 samples of reg 0x67 over 4 s, plus header and summary."""
        self.buf = ""
        self.send_char("S")
        text = self.read_until("last 0x", timeout=8.0)
        text += self.read_until("\n", timeout=2.0)
        samples = parse_samples(text)
        header = parse_rf_header(text)
        summary = parse_s_minmax(text)
        if not samples or not summary:
            raise SystemExit(f"'S' did not look like a sample run:\n{text[-600:]}")
        values = [s[1] for s in samples]
        result = {
            "label": label,
            "radio": header,
            "samples": values,
            "min": summary["min"],
            "max": summary["max"],
            "last": summary["last"],
            "mean": sum(values) / len(values),
            "mean_dbm": rssi_to_dbm(sum(values) / len(values)),
            "open": sum(1 for s in samples if s[3] == "open"),
            "quiet": sum(1 for s in samples if s[3] == "quiet"),
        }
        return result

    def cycle_front_end(self):
        """Console 'F': step the chip-side receive front end, read the new mode."""
        self.buf = ""
        self.send_char("F")
        text = self.read_until("press 'F' for the next", timeout=4.0)
        mode = RE_F_MODE.search(text)
        reg = RE_F_REG33.search(text)
        if not mode:
            raise SystemExit(f"'F' gave no mode line:\n{text[-400:]}")
        return {"mode": int(mode.group(1)),
                "pa0": reg.group(1) if reg else None,
                "reg33": int(reg.group(2), 16) if reg else None}

    def cycle_band_pin(self):
        """Console 'B': step the MCU band pin PA0, read the new mode."""
        self.buf = ""
        self.send_char("B")
        text = self.read_until("note the level that works", timeout=4.0)
        mode = RE_B_MODE.search(text)
        pa0 = RE_B_PA0.search(text)
        if not mode:
            raise SystemExit(f"'B' gave no mode line:\n{text[-400:]}")
        return {"mode": int(mode.group(1)),
                "pa0": pa0.group(1) if pa0 else None,
                "band": pa0.group(2) if pa0 else None}

    def set_front_end_mode(self, target, max_steps=6):
        """'F' only cycles, so step until the reported mode matches."""
        seen = []
        for _ in range(max_steps):
            state = self.cycle_front_end()
            seen.append(state["mode"])
            if state["mode"] == target:
                return state
        raise SystemExit(f"could not reach front-end mode {target}; cycled {seen}")

    def set_band_pin_mode(self, target, max_steps=5):
        seen = []
        for _ in range(max_steps):
            state = self.cycle_band_pin()
            seen.append(state["mode"])
            if state["mode"] == target:
                return state
        raise SystemExit(f"could not reach band-pin mode {target}; cycled {seen}")


# --------------------------------------------------------------------------
# experiments
# --------------------------------------------------------------------------

def at_radio_text(level_dbm, pad_db):
    """The generator level is quoted at the SMA; a pad may sit before the radio."""
    if pad_db:
        return (f"{level_dbm:g} dBm at the SMA "
                f"({level_dbm - pad_db:g} dBm at the radio, {pad_db:g} dB pad)")
    return f"{level_dbm:g} dBm at the SMA (no pad)"


def rf_safety_check(args, radio):
    """Poll 'T' (read-only) and spell out the attenuator arithmetic before RF.

    The probe never transmits and never sends PTT, but it cannot see a press on
    the radio's own PTT, so the pad -- not the script -- is the protection.
    """
    tx = radio.tx_state()
    pad = args.pad_db
    print(f"\nRF safety: the tinySA input is rated +{TINYSA_MAX_INPUT_DBM:g} dBm "
          f"(absolute max at 0 dB internal atten; +20 dBm only as a short peak "
          f"with 30 dB internal atten).")
    print(f"  a keyed {args.radio_tx_dbm:g} dBm handheld is "
          f"{args.radio_tx_dbm - TINYSA_MAX_INPUT_DBM:g} dB past that limit, and the "
          f"generator's output path shares the same SMA.")
    into_tinysa = args.radio_tx_dbm - pad
    if pad <= 0:
        print("  !! --pad-db 0: nothing stands between the radio's TX and the SMA.")
    else:
        print(f"  TX {args.radio_tx_dbm:g} dBm through the {pad:g} dB pad -> "
              f"{into_tinysa:+g} dBm at the SMA "
              f"({'safe' if into_tinysa <= TINYSA_MAX_INPUT_DBM else 'STILL DESTRUCTIVE'})")
    if into_tinysa > TINYSA_MAX_INPUT_DBM:
        print("  -> want at least 30 dB (10 W-rated) and 40-50 dB in total; the pad "
              "still leaves the injected signal ~50 dB above the radio's sensitivity.")
    if tx["transmitting"]:
        raise SystemExit(
            f"  ABORT: the radio reports transmitting ({tx['source']}, "
            f"{tx['freq_mhz']:.5f} MHz, {tx['band']}).\n"
            f"  Stop the TX and retry -- a pad makes it survivable, but a carrier "
            f"from the radio would swamp the measurement anyway.")
    print("  radio reports: idle -- not transmitting (read with 'T', no keying)")
    print("  a press on the radio's own PTT is invisible here, so keep the pad in "
          "place rather than relying on this check.")


def measure_pair(radio, tinysa, label, dry_run=False):
    """Carrier off / carrier on for the current tuning: the pair is the datum."""
    if dry_run:
        off = radio.sample_rssi(f"{label} (carrier off)")
        on = off
    else:
        tinysa.output(False)
        off = radio.sample_rssi(f"{label} (carrier off)")
        tinysa.output(True)
        on = radio.sample_rssi(f"{label} (carrier on)")
    delta = on["mean"] - off["mean"]
    return {"label": label, "off": off, "on": on, "delta": delta,
            "delta_dbm": delta / 2.0}


def run_sample(args, radio, tinysa):
    freq = BANDS[args.band]
    level = args.level if args.level is not None else DEFAULT_LEVELS[args.band]
    level_txt = "DRY RUN (generator untouched)" if args.dry_run \
        else at_radio_text(level, args.pad_db)
    print(f"\n=== {args.band.upper()} {freq / 1e6:.5f} MHz, generator {level_txt} ===")
    if not args.dry_run:
        actual = tinysa.prepare(freq, level)
        if actual and abs(actual - freq) > 1000:
            print(f"  !! device clamped {freq / 1e6:.5f} MHz to {actual / 1e6:.5f} MHz "
                  f"-- this band needs the other output stage")
    if args.tune:
        radio.cat_set_frequency(freq)
        print(f"  radio tuned via CAT to {freq / 1e6:.5f} MHz")
    pair = measure_pair(radio, tinysa, args.band, args.dry_run)
    report_pair(pair)


def hex3(value):
    """A mean is fractional; show the reg 0x67 equivalent as a rounded integer."""
    return f"0x{int(round(value)):03X}"


def report_pair(pair):
    off, on = pair["off"], pair["on"]
    if off["radio"]:
        print(f"  radio reports RX {off['radio']['freq_mhz']:.5f} MHz, "
              f"{off['radio']['band']}, {off['radio']['branch']}")
    print(f"  carrier off: mean {off['mean']:6.1f} ({hex3(off['mean'])}, "
          f"{off['mean_dbm']:7.2f} dBm)  min {off['min']:3d} max {off['max']:3d} "
          f"open {off['open']}/{len(off['samples'])}")
    print(f"  carrier on : mean {on['mean']:6.1f} ({hex3(on['mean'])}, "
          f"{on['mean_dbm']:7.2f} dBm)  min {on['min']:3d} max {on['max']:3d} "
          f"open {on['open']}/{len(on['samples'])}")
    print(f"  delta      : {pair['delta']:+.1f} raw ({pair['delta_dbm']:+.2f} dB)")


def run_matrix(args, radio, tinysa):
    rows = []
    for band in args.bands:
        freq = BANDS[band]
        level = args.level if args.level is not None else DEFAULT_LEVELS[band]
        print(f"\n=== {band.upper()} {freq / 1e6:.5f} MHz @ "
              f"{at_radio_text(level, args.pad_db)} "
              f"({args.f_modes} front-end modes x {args.b_modes} band-pin modes) ===")
        if not args.dry_run:
            actual = tinysa.prepare(freq, level)
            if actual and abs(actual - freq) > 1000:
                print(f"  !! device clamped to {actual / 1e6:.5f} MHz -- wrong output stage")
        if args.tune:
            radio.cat_set_frequency(freq)
            print(f"  radio tuned via CAT to {freq / 1e6:.5f} MHz")

        for f_step in range(args.f_modes):
            fe = radio.cycle_front_end()
            for b_step in range(args.b_modes):
                bp = radio.cycle_band_pin()
                reg_txt = f"0x{fe['reg33']:04X}" if fe["reg33"] is not None else "0x????"
                label = (f"{band} F{fe['mode']}({reg_txt}) "
                         f"B{bp['mode']}(PA0 {bp['pa0']})")
                pair = measure_pair(radio, tinysa, label, args.dry_run)
                header_band = pair["off"]["radio"]["band"] if pair["off"]["radio"] else "?"
                rows.append({
                    "band": band, "f_mode": fe["mode"], "reg33": fe["reg33"],
                    "b_mode": bp["mode"], "pa0": bp["pa0"],
                    "reported_band": header_band,
                    "off_mean": pair["off"]["mean"], "on_mean": pair["on"]["mean"],
                    "delta": pair["delta"], "delta_dbm": pair["delta_dbm"],
                })
                print(f"  F{fe['mode']} ({reg_txt}) B{bp['mode']} "
                      f"(PA0 {bp['pa0']}): off {pair['off']['mean']:6.1f} "
                      f"-> on {pair['on']['mean']:6.1f}  "
                      f"delta {pair['delta']:+7.1f} ({pair['delta_dbm']:+.2f} dB)  "
                      f"[radio says {header_band}]")

        # Leave the front end on the application rule (mode 4) for normal use.
        radio.set_front_end_mode(4)

    print_matrix_verdict(rows)
    if args.csv:
        write_csv(args.csv, rows)
        print(f"\nwrote {args.csv}")


def print_matrix_verdict(rows):
    print("\n=== observed (deltas are mean reg 0x67 carrier-on minus carrier-off) ===")
    for band in sorted({r["band"] for r in rows}):
        band_rows = [r for r in rows if r["band"] == band]
        best = max(band_rows, key=lambda r: r["delta"])
        floor = min(band_rows, key=lambda r: r["delta"])
        print(f"  {band.upper()}: best F{best['f_mode']} (0x{best['reg33']:04X}) "
              f"B{best['b_mode']} delta {best['delta']:+.1f} raw "
              f"({best['delta_dbm']:+.2f} dB); "
              f"worst F{floor['f_mode']} (0x{floor['reg33']:04X}) "
              f"B{floor['b_mode']} delta {floor['delta']:+.1f}")
        margin = best["delta"] - floor["delta"]
        if margin < 16:  # 8 raw counts = 4 dB between the best and worst mode
            print(f"    note: best-to-worst margin is only {margin:.1f} raw "
                  f"({margin / 2:.1f} dB) -- too small to tell the modes apart. "
                  f"Raise the injected level or check the RF connection.")
        else:
            print(f"    margin best-to-worst: {margin:.1f} raw ({margin / 2:.1f} dB)")


def write_csv(path, rows):
    import csv
    with open(path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def run_squelch(args, radio, tinysa):
    """Drive the firmware's own 'Q' autodetect, satisfying its keystroke gates."""
    print("\n=== driving the firmware's 'Q' squelch autodetect ===")
    if args.tune is False:
        print("  note: 'Q' tunes the radio itself; --no-tune does not apply")

    radio.buf = ""
    radio.send_char("Q")
    results = []
    for _ in range(2):  # VHF then UHF
        band_text = radio.read_until("---", timeout=20.0)
        band_text += radio.read_until("\n", timeout=5.0)
        m = RE_Q_BAND.search(band_text)
        if not m:
            raise SystemExit(f"no band header from 'Q':\n{band_text[-400:]}")
        band_name, freq_mhz = m.group(1), float(m.group(2))
        freq = int(round(freq_mhz * 1e6))
        key = "vhf" if band_name == "VHF" else "uhf"
        level = args.level if args.level is not None else DEFAULT_LEVELS[key]
        print(f"\n  --- {band_name} {freq_mhz:.5f} MHz ---")

        # Gate 1: the radio wants the generator quiet while it measures the floor.
        radio.read_until("press any key", timeout=20.0)
        if not args.dry_run:
            actual = tinysa.prepare(freq, level)
            tinysa.output(False)
            if actual and abs(actual - freq) > 1000:
                print(f"    !! device clamped to {actual / 1e6:.5f} MHz")
            print(f"    generator set to {actual / 1e6 if actual else freq / 1e6:.5f} MHz, "
                  f"{at_radio_text(level, args.pad_db)}, output OFF")
        radio.send_char(" ")

        # Gate 2: carrier on.
        floor_text = radio.read_until("press any key", timeout=20.0)
        floor = RE_Q_FLOOR.search(floor_text)
        if not args.dry_run:
            tinysa.output(True)
            print("    generator output ON")
        radio.send_char(" ")

        carrier_text = radio.read_until("detected:", timeout=20.0)
        carrier_text += radio.read_until("\n", timeout=5.0)
        carrier = RE_Q_CARRIER.search(carrier_text)
        detected = RE_Q_DETECTED.search(carrier_text)
        no_carrier = RE_Q_NOCARRIER.search(carrier_text)

        floor_mean = int(floor.group(3), 16) if floor else None
        carrier_mean = int(carrier.group(3), 16) if carrier else None
        print(f"    floor   mean {floor_mean} ({rssi_to_dbm(floor_mean):.2f} dBm)"
              if floor_mean is not None else "    floor   : not parsed")
        print(f"    carrier mean {carrier_mean} ({rssi_to_dbm(carrier_mean):.2f} dBm)"
              if carrier_mean is not None else "    carrier : not parsed")
        if detected:
            print(f"    detected: open 0x{detected.group(1)} close 0x{detected.group(2)}")
        elif no_carrier:
            print("    detected: floor and carrier overlap -- no carrier seen")
        if floor_mean is not None and carrier_mean is not None:
            print(f"    delta   : {carrier_mean - floor_mean:+d} raw "
                  f"({(carrier_mean - floor_mean) / 2:+.2f} dB)")
        results.append({"band": band_name, "freq_mhz": freq_mhz,
                        "floor_mean": floor_mean, "carrier_mean": carrier_mean,
                        "open": int(detected.group(1), 16) if detected else None,
                        "close": int(detected.group(2), 16) if detected else None})

    radio.read_for(1.0)
    print("\n=== summary ===")
    for r in results:
        print(f"  {r['band']}: floor {r['floor_mean']} carrier {r['carrier_mean']} "
              f"delta {r['carrier_mean'] - r['floor_mean']:+d}"
              if r["floor_mean"] is not None and r["carrier_mean"] is not None
              else f"  {r['band']}: incomplete")


def run_ports(_args):
    print("serial ports:")
    for entry in list_ports.comports():
        print(f"  {entry.device:<28} {entry.description}")
        if entry.hwid:
            print(f"      {entry.hwid}")
    print("\ntypically: the tinySA is 'tinysa.org tinySA' (USB 0483:5740) and the radio's\n"
          "console is a CH340 (USB 1a86:7523) on the USB-C programming port.")


# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Correlate the RA89R's UART RSSI with a tinySA signal generator.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Only pyserial")[0])
    parser.add_argument("--radio-port", default="/dev/ttyUSB0",
                        help="radio console (CH340 USB-C port), default %(default)s")
    parser.add_argument("--tinysa-port", default="/dev/ttyACM1",
                        help="tinySA console, default %(default)s")
    parser.add_argument("--vhf-level", type=float, help="VHF injected level in dBm")
    parser.add_argument("--uhf-level", type=float, help="UHF injected level in dBm")
    parser.add_argument("--pad-db", type=float, default=0.0,
                        help="external attenuation between the tinySA SMA and the "
                             "radio's antenna port (default %(default)s; 40 is a sane "
                             "5 W pad).  Levels are quoted at the SMA either way")
    parser.add_argument("--radio-tx-dbm", type=float, default=HANDHELD_TX_DBM,
                        help="the radio's TX power in dBm, for the pad arithmetic "
                             "(default %(default)s = 5 W)")
    parser.add_argument("--no-tune", dest="tune", action="store_false",
                        help="do not tune the radio via CAT; use the current tuning")
    parser.add_argument("--dry-run", action="store_true",
                        help="never touch the tinySA output (validates the UART side only)")
    parser.add_argument("-v", "--verbose", action="store_true", help="echo serial traffic")

    subs = parser.add_subparsers(dest="command", required=True)

    subs.add_parser("ports", help="list serial ports")

    p_sample = subs.add_parser("sample", help="one band: carrier off vs on")
    p_sample.add_argument("--band", choices=sorted(BANDS), required=True)
    p_sample.add_argument("--level", type=float, help="injected level in dBm")
    p_sample.set_defaults(func=run_sample)

    p_matrix = subs.add_parser("matrix", help="front-end/band-pin modes x band")
    p_matrix.add_argument("--bands", nargs="+", choices=sorted(BANDS),
                          default=["vhf", "uhf"])
    p_matrix.add_argument("--level", type=float, help="injected level (both bands)")
    p_matrix.add_argument("--f-modes", type=int, default=5,
                          help="how many 'F' modes to visit (default %(default)s)")
    p_matrix.add_argument("--b-modes", type=int, default=3,
                          help="how many 'B' modes to visit (default %(default)s)")
    p_matrix.add_argument("--csv", help="write the raw rows to this CSV file")
    p_matrix.set_defaults(func=run_matrix)

    p_sq = subs.add_parser("squelch", help="drive the firmware's 'Q' autodetect")
    p_sq.add_argument("--level", type=float, help="injected level in dBm")
    p_sq.set_defaults(func=run_squelch)

    args = parser.parse_args()

    if args.vhf_level is not None:
        DEFAULT_LEVELS["vhf"] = args.vhf_level
    if args.uhf_level is not None:
        DEFAULT_LEVELS["uhf"] = args.uhf_level

    if args.command == "ports":
        return run_ports(args)

    log = print if args.verbose else (lambda *_a, **_k: None)
    radio = RadioConsole(args.radio_port, log=log)
    tinysa = TinySA(args.tinysa_port, log=log)
    try:
        radio.open()
        print(f"radio console open on {args.radio_port}")
        if not args.dry_run:
            rf_safety_check(args, radio)
            tinysa.open()
            print(f"tinySA open on {args.tinysa_port}")
        args.func(args, radio, tinysa)
    finally:
        if not args.dry_run:
            tinysa.restore_input()
            tinysa.close()
            print("tinySA parked back in input mode")
        radio.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
