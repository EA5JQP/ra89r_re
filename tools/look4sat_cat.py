#!/usr/bin/env python3
"""Look4Sat FT-817 CAT stand-in for the RA89R.

Look4Sat's phone-side radio control is a `Ft817Controller` speaking Yaesu FT-817
CAT.  It cannot open a USB serial port today, so this script sends exactly the
bytes that controller would send, over the USB-C (CH340) serial port, to the
radio's CAT server.  When Look4Sat gains a USB-serial transport, this is the
traffic it will produce.

Protocol (from Look4Sat `Ft817CatProtocol.kt`): 5-byte frames = 4 payload bytes
then a command byte; writes are ACKed with 0x00; READ returns 4 BCD + mode.

  set freq    BCD(Hz/10) 4 bytes + 0x01
  read       00 00 00 00 + 0x03  -> 4 BCD + mode
  set mode   mode 00 00 00 + 0x07   (0x04 AM, 0x08 FM, ...)
  PTT on     00 00 00 00 + 0x08
  PTT off    00 00 00 00 + 0x88
  CTCSS mode sub 00 00 00 + 0x0A    (0x2A on, 0x8A off)
  CTCSS tone BCD(tone*10) 2 bytes 00 00 + 0x0B

Examples:
    python3 tools/look4sat_cat.py --port /dev/ttyUSB1 freq 145.500
    python3 tools/look4sat_cat.py --port /dev/ttyUSB1 freq 435.100 --mode FM
    python3 tools/look4sat_cat.py --port /dev/ttyUSB1 read
    python3 tools/look4sat_cat.py --port /dev/ttyUSB1 ptt on
    python3 tools/look4sat_cat.py --port /dev/ttyUSB1 ctcss 88.5
    python3 tools/look4sat_cat.py --port /dev/ttyUSB1 sweep 435.600 435.700 5

Only pyserial is needed (the same dependency as tools/ra89r_flash.py).
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:                     # pragma: no cover
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    sys.exit(2)

CMD_SET_FREQ = 0x01
CMD_READ = 0x03
CMD_SET_MODE = 0x07
CMD_PTT_ON = 0x08
CMD_PTT_OFF = 0x88
CMD_CTCSS_MODE = 0x0A
CMD_CTCSS_TONE = 0x0B

CTCSS_ENC_ON = 0x2A
CTCSS_OFF = 0x8A

MODE_TO_BYTE = {
    "LSB": 0x00, "USB": 0x01, "CW": 0x02, "CW-R": 0x03,
    "AM": 0x04, "FM": 0x08, "DIG": 0x0A, "PKT": 0x0C,
}
BYTE_TO_MODE = {v: k for k, v in MODE_TO_BYTE.items()}

# Look4Sat waits this long after a write before reading the ACK.
COMMAND_DELAY_S = 0.2


def encode_freq_bcd(freq_hz):
    """145.5e6 Hz -> b'\x14\x55\x00\x00' (Hz/10, two decimal digits per byte)."""
    freq_10hz = freq_hz // 10
    digits = "%08d" % freq_10hz
    return bytes(((int(digits[i * 2]) << 4) | int(digits[i * 2 + 1])) for i in range(4))


def decode_freq_bcd(bcd):
    freq_10hz = 0
    for b in bcd[:4]:
        freq_10hz = freq_10hz * 100 + ((b >> 4) & 0x0F) * 10 + (b & 0x0F)
    return freq_10hz * 10


def encode_ctcss_bcd(tone_hz):
    tone_01hz = int(round(tone_hz * 10))
    digits = "%04d" % tone_01hz
    return bytes(((int(digits[i * 2]) << 4) | int(digits[i * 2 + 1])) for i in range(2))


def parse_hz(text):
    """'145.500' or '145500000' -> Hz."""
    if "." in text:
        return int(round(float(text) * 1_000_000))
    return int(text)


class Rig:
    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.3)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def write(self, payload, ack=True):
        """Send 4 payload bytes + command, then read the 1-byte ACK (0x00)."""
        frame = bytes(payload[:4]).ljust(4, b"\x00")
        cmd = payload[4]
        self.ser.write(frame + bytes([cmd]))
        self.ser.flush()
        time.sleep(COMMAND_DELAY_S)
        if not ack:
            return None
        ack_byte = self.ser.read(1)
        return ack_byte[0] if ack_byte else None

    def set_freq(self, freq_hz):
        return self.write(encode_freq_bcd(freq_hz) + bytes([CMD_SET_FREQ]))

    def set_mode(self, mode):
        return self.write(bytes([MODE_TO_BYTE[mode], 0, 0, 0, CMD_SET_MODE]))

    def read_freq_mode(self):
        self.ser.write(bytes([0, 0, 0, 0, CMD_READ]))
        self.ser.flush()
        time.sleep(COMMAND_DELAY_S)
        resp = self.ser.read(5)
        if len(resp) < 5:
            return None
        return decode_freq_bcd(resp), BYTE_TO_MODE.get(resp[4], "?")

    def ptt(self, on):
        return self.write(bytes([0, 0, 0, 0, CMD_PTT_ON if on else CMD_PTT_OFF]))

    def ctcss_mode(self, on):
        return self.write(bytes([CTCSS_ENC_ON if on else CTCSS_OFF, 0, 0, 0, CMD_CTCSS_MODE]))

    def ctcss_tone(self, tone_hz):
        return self.write(encode_ctcss_bcd(tone_hz) + bytes([0, 0, CMD_CTCSS_TONE]))


def report(label, ack):
    ok = ack == 0x00
    print("%-14s ack=%s %s" % (label, "0x%02X" % ack if ack is not None else "--",
                               "ok" if ok else "NO ACK"))


def main():
    ap = argparse.ArgumentParser(description="Look4Sat FT-817 CAT stand-in")
    ap.add_argument("--port", required=True, help="serial port, e.g. /dev/ttyUSB1")
    ap.add_argument("--baud", type=int, default=115200, help="CAT baud (default 115200, the port's USART1 rate)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("freq"); p.add_argument("hz")
    p.add_argument("--mode", choices=sorted(MODE_TO_BYTE))
    sub.add_parser("read")
    p = sub.add_parser("ptt"); p.add_argument("state", choices=["on", "off"])
    p = sub.add_parser("ctcss"); p.add_argument("tone", type=float, nargs="?")
    p = sub.add_parser("sweep"); p.add_argument("start"); p.add_argument("stop")
    p.add_argument("count", type=int); p.add_argument("--mode", default="FM")

    args = ap.parse_args()
    rig = Rig(args.port, args.baud)
    try:
        if args.cmd == "freq":
            report("set-freq", rig.set_freq(parse_hz(args.hz)))
            if args.mode:
                report("set-mode", rig.set_mode(args.mode))
        elif args.cmd == "read":
            got = rig.read_freq_mode()
            print("read: %s" % ("--" if got is None else
                                "%.6f MHz %s" % (got[0] / 1e6, got[1])))
        elif args.cmd == "ptt":
            report("ptt-%s" % args.state, rig.ptt(args.state == "on"))
        elif args.cmd == "ctcss":
            if args.tone is None:
                report("ctcss-off", rig.ctcss_mode(False))
            else:
                report("ctcss-mode", rig.ctcss_mode(True))
                report("ctcss-tone", rig.ctcss_tone(args.tone))
        elif args.cmd == "sweep":
            start, stop = parse_hz(args.start), parse_hz(args.stop)
            report("set-mode", rig.set_mode(args.mode))
            for i in range(args.count):
                f = start + (stop - start) * i // max(args.count - 1, 1)
                ack = rig.set_freq(f)
                print("  %10.6f MHz -> %s" % (f / 1e6,
                      "ok" if ack == 0x00 else "NO ACK"))
    finally:
        rig.close()


if __name__ == "__main__":
    main()
