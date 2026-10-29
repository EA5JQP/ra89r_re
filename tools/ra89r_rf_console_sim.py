#!/usr/bin/env python3
"""Test double for the RA89R's UART console, for `tools/rf_harness.py`.

Emits the exact strings `firmware/App/main.c` prints for the console commands the
harness uses -- 'S' (`rf_watch`), 'F' (chip front end), 'B' (MCU band pin) and
'Q' (`rf_squelch_autodetect`) -- and accepts FT-817 CAT frames, so the harness can
be exercised end to end with no radio attached.  Same idea as
`tools/ra89r_bootloader_sim.py`: a protocol-level stand-in so the host side is
testable on its own.

Pair it with the harness over a socat PTY:

    socat -d -d pty,raw,echo=0,link=/tmp/ra89r0 \\
                  pty,raw,echo=0,link=/tmp/ra89r1 &
    python3 tools/ra89r_rf_console_sim.py /tmp/ra89r1 &
    python3 tools/rf_harness.py --radio-port /tmp/ra89r0 --dry-run sample --band uhf
    python3 tools/rf_harness.py --radio-port /tmp/ra89r0 --dry-run squelch

The timings are accelerated (50 ms per RSSI sample instead of the firmware's
200 ms) so a run takes seconds; the text is what matters, not the spacing.
`--tx` makes 'T' report "transmitting", which is how the harness's pre-flight
abort is checked.

Keep the format strings in step with `main.c`: the harness parses this text with
regexes aimed at those `printf`s, so a sim that drifts from them stops proving
anything.  The RSSI figures here are made up -- the point is the parse, not the
radio.
"""
import argparse
import sys
import time

import serial

# The pair the firmware's 'Q' uses, as (frequency in MHz, display string).
BAND_INFO = {"VHF": (145.50000, "145.50000"), "UHF": (446.00625, "446.00625")}
# (quiet floor, level with the carrier on), in reg 0x67 counts.
BAND_RSSI = {"VHF": (0x0EE, 0x120), "UHF": (0x0E2, 0x115)}

# Command bytes the harness (and tools/look4sat_cat.py) can send; only 0x01 is
# acted on, the rest are accepted and ACKed so the frame parser is exercised.
CAT_CMDS = (0x01, 0x03, 0x07, 0x08, 0x88, 0x0A, 0x0B)
CAT_CMD_SET_FREQ = 0x01

STATE = {"f_mode": 4, "b_mode": 0, "tuned": 145.50000, "tx": False}
SER = None
LOG = sys.stdout


def p(text):
    """Console output goes over the serial link (and is mirrored to the log)."""
    SER.write(text.encode())
    SER.flush()
    LOG.write(text)
    LOG.flush()


def echo(cmd):
    """The firmware echoes the typed character."""
    p(cmd)


def rf_watch():
    band = "VHF" if STATE["tuned"] < 280 else "UHF"
    floor, _carrier = BAND_RSSI[band]
    p("\nRF: BK4829 reg 0x67 every 200 ms for 4 s "
      "(0xB4 / 0xCF are the stock's squelch marks)\n")
    p(f"  RX {STATE['tuned']:.5f} MHz, {band}, "
      f"{'main' if STATE['tuned'] > 134 else 'sub'} (>134 MHz split)\n")
    values = [floor - 3, floor, floor + 2, floor + 1] * 5
    for i, v in enumerate(values[:20]):
        klass = "open" if v >= 0xCF else ("quiet" if v < 0xB4 else "between")
        p(f"  {i:02d}: 0x{v:03X}{v:4d} {klass}\n")
        time.sleep(0.05)  # accelerated (real firmware: 200 ms)
    p(f"  min 0x{min(values):03X}  max 0x{max(values):03X}  "
      f"last 0x{values[-1]:03X}\n")


FRONT_END_NAMES = [
    "leave the register as BK4819_Init() left it",
    "VHF bit: 0x33 |= 0x04, 0x08 clear",
    "UHF bit: 0x33 |= 0x08, 0x04 clear",
    "both cleared",
    "the K1/app rule: VHF pin 4 below 280 MHz, UHF pin 3 (default)",
]
BAND_PIN_NAMES = [
    "PA0 low whatever the band (the stock's value, default)",
    "PA0 high whatever the band",
    "auto: PA0 high for UHF, low for VHF (an inference)",
]


def front_end():
    STATE["f_mode"] = (STATE["f_mode"] + 1) % len(FRONT_END_NAMES)
    mode = STATE["f_mode"]
    reg33 = {0: 0x0044, 1: 0x0044, 2: 0x0048, 3: 0x0040, 4: 0x0044}[mode]
    p(f"\nfront end: mode {mode} -- {FRONT_END_NAMES[mode]}\n")
    p(f"  PA0 {'high' if STATE['b_mode'] == 1 else 'low'}, reg 0x33 = 0x{reg33:04X}\n")
    p("  listen (or 'S' with a carrier) and press 'F' for the next;"
      " note which mode receives\n")


def band_pin():
    STATE["b_mode"] = (STATE["b_mode"] + 1) % len(BAND_PIN_NAMES)
    mode = STATE["b_mode"]
    pa0 = "high" if mode == 1 else "low"
    p(f"\nMCU band pin: mode {mode} -- {BAND_PIN_NAMES[mode]}\n")
    p(f"  PA1 high, PA0 {pa0} ({'UHF' if STATE['tuned'] >= 280 else 'VHF'})\n")
    p("  retune UHF, press 'S' with a carrier and listen;"
      " note the level that works\n")


def wait_key(seconds):
    end = time.time() + seconds
    while time.time() < end:
        if SER.in_waiting:
            SER.read(SER.in_waiting)
            return True
        time.sleep(0.05)
    return False


def squelch():
    p("\nsquelch auto-detect (BK4829 reg 0x67) -- "
      "VHF (145.5000) first, then UHF (446.00625)\n")
    for name in ("VHF", "UHF"):
        disp = BAND_INFO[name][1]
        p(f"\n--- {name} {disp} MHz ---\n"
          "  set the tinySA to this frequency with its output OFF,\n"
          "  then press any key (or wait 15 s)\n")
        if not wait_key(15.0):
            return
        STATE["tuned"] = BAND_INFO[name][0]
        time.sleep(0.1)
        floor, carrier = BAND_RSSI[name]
        p(f"  floor  : min 0x{floor - 4:03X} max 0x{floor:03X} mean 0x{floor:03X}\n")
        p("  now turn the tinySA output ON, then press any key (or wait 15 s)\n")
        if not wait_key(15.0):
            return
        p(f"  carrier: min 0x{carrier - 6:03X} max 0x{carrier:03X} "
          f"mean 0x{carrier:03X}\n")
        # The real test is sig_lo - floor_hi >= 0x10 (main.c); the shape of the
        # two branches is what the harness parses, so either is fine here.
        if carrier - floor >= 0x10:
            open_mark = floor + (carrier - floor) // 2
            p(f"  detected: open 0x{open_mark:03X}, close 0x{open_mark - 0x1B:03X} "
              f"(between floor and carrier; stock 0x0CF / 0x0B4)\n")
        else:
            p("  detected: floor and carrier overlap -- no carrier seen\n")
    p(f"\nback on {STATE['tuned']:.5f} MHz\n")


def tx_diag():
    """Console 'T': the firmware's read-only TX path read-out (main.c:1068)."""
    p("\nTX path\n")
    if STATE["tx"]:
        p(f"  transmitting (tone)  {STATE['tuned']:.5f} MHz  "
          f"band {'UHF' if STATE['tuned'] >= 280 else 'VHF'}  TX_VFO 0\n")
    else:
        p("  idle -- hold PTT to transmit\n")
    p("  PA duty (PB14/TIM1_CH2 compare): 0 of 719   (pa_last_compare 0)\n")
    p("  power: OUTPUT_POWER=2  TXP_CalculatedSetting=102 -> compare 287\n")
    p("  TX path pin: mode 0 -- auto -> band bits 0x0040\n")
    p("  BK4829: 0x33=0x0040 0x36=0x0000 0x7D=0x0000 0x30=0x0000 0x50=0x0000\n")
    p("  TX filter 0x43=0x0000   BK4815 band 0x75=0x0011  T/R 0x0C=0x0000\n")
    p("  TX variant ('w'): 0 -- default\n")
    p("  'x' cycles the TX path pin, 'Y' steps the PA duty, 'w' the TX variant\n")


def handle_cat(frame):
    if frame[4] == CAT_CMD_SET_FREQ:
        freq_10hz = 0
        for b in frame[:4]:
            freq_10hz = freq_10hz * 100 + (b >> 4) * 10 + (b & 0x0F)
        STATE["tuned"] = freq_10hz / 100000.0
        p(f"[sim] CAT set freq {STATE['tuned']:.5f} MHz\n")
    SER.write(bytes([0x00]))  # ACK


def main():
    global SER
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port", help="PTY end to serve, e.g. /tmp/ra89r1")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--tx", action="store_true",
                        help="report 'transmitting' from 'T' (to test the harness's "
                             "pre-flight abort)")
    args = parser.parse_args()
    STATE["tx"] = args.tx

    SER = serial.Serial(args.port, args.baud, timeout=0.2)
    p(f"[RA89R console sim on {args.port}]\n")
    while True:
        first = SER.read(1)
        if not first:
            continue
        pending = first
        # A 5-byte CAT frame may arrive split; give it a moment before deciding
        # (the firmware does the same with its CAT_BURST_GAP_MS window).
        gap = time.time() + 0.15
        while len(pending) < 5 and time.time() < gap:
            more = SER.read(5 - len(pending))
            if more:
                pending += more
        if len(pending) >= 5 and pending[4] in CAT_CMDS:
            handle_cat(pending[:5])
            pending = pending[5:]
        while pending:
            ch, pending = chr(pending[0]), pending[1:]
            echo(ch)
            if ch == "S":
                rf_watch()
            elif ch == "F":
                front_end()
            elif ch == "B":
                band_pin()
            elif ch == "Q":
                squelch()
            elif ch == "T":
                tx_diag()


if __name__ == "__main__":
    main()
