#!/usr/bin/env python3
"""End-to-end check: the real CHIRP F4HWN v6.2.0 driver (through the RA89R
subclass in ra89r_f4hwn.py) against the firmware's own programming code
(firmware/App/k5prog.c + k1_view.c over the settings layer), on a PTY.

    python3 tools/webchirp/chirp_e2e.py --chirp ~/src/chirp \\
        --driver ~/src/webchirp/web/python/extra_drivers/quansheng/f4hwn_v6_2_0.py

--chirp is a CHIRP source tree (the directory holding the `chirp` package; the
webchirp submodule pin is 0b4076d9).  The simulator is built with gcc from
firmware/ unless --sim names a built one.  No radio is needed.

What it does, as webchirp would: download; change a channel, delete one, set
a name, change the squelch and a VFO; upload (the radio "reboots"); download
again and compare; then the simulator reports whether the stock codeplug and
the K1 calibration bytes are unchanged.
"""

import argparse
import importlib.util
import os
import subprocess
import sys
import tempfile
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FIRMWARE = os.path.join(REPO, "firmware")


def build_sim(out):
    srcs = []
    with open(os.path.join(FIRMWARE, "tools", "check_all.sh")) as f:
        text = f.read()
    block = text.split('K1_HOST_SRCS="', 1)[1].split('"', 1)[0]
    srcs = block.replace("\\", " ").split()
    cmd = (["gcc", "-std=c11", "-I", "tools/host", "-I", "App", "-I", "App/driver",
            "-DPY32F403xD", "-include", "App/k1_features.h", "-DST7565_HOST_TEST",
            "-DBLUETOOTH_HOST_TEST", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "tools/k5prog_sim.c", "App/k5prog.c"] + srcs + ["-o", out])
    subprocess.run(cmd, cwd=FIRMWARE, check=True, stderr=subprocess.DEVNULL)


def load_driver(chirp_src, driver_path):
    sys.path.insert(0, chirp_src)
    # The F4HWN driver imports wx for its desktop dialogs; webchirp shims it too.
    wx = types.ModuleType("wx")
    wx.MessageBox = lambda *a, **k: 0
    wx.OK = wx.CANCEL = wx.CANCEL_DEFAULT = wx.ICON_WARNING = 0
    sys.modules["wx"] = wx
    import chirp.drivers  # noqa: F401  (the package the modules land in)

    def load(name, path):
        spec = importlib.util.spec_from_file_location("chirp.drivers." + name, path)
        mod = importlib.util.module_from_spec(spec)
        sys.modules["chirp.drivers." + name] = mod
        spec.loader.exec_module(mod)
        return mod

    load("f4hwn_v6_2_0", driver_path)
    return load("ra89r_f4hwn", os.path.join(HERE, "ra89r_f4hwn.py"))


def find_setting(group, name):
    for el in group:
        if hasattr(el, "get_name") and el.get_name() == name and hasattr(el, "value"):
            return el
        if hasattr(el, "__iter__") and not hasattr(el, "value"):
            got = find_setting(el, name)
            if got is not None:
                return got
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--chirp", required=True, help="CHIRP source tree (holds chirp/)")
    ap.add_argument("--driver", required=True, help="f4hwn_v6_2_0.py")
    ap.add_argument("--sim", help="a built k5prog_sim (default: build it)")
    args = ap.parse_args()

    import serial  # pyserial

    drv = load_driver(os.path.abspath(args.chirp), os.path.abspath(args.driver))
    from chirp import chirp_common

    tmp = tempfile.mkdtemp(prefix="ra89r_chirp_")
    sim_bin = args.sim or os.path.join(tmp, "k5prog_sim")
    if not args.sim:
        build_sim(sim_bin)
    pty_file = os.path.join(tmp, "pty")
    sim = subprocess.Popen([sim_bin, pty_file], stdin=subprocess.PIPE, text=True)
    for _ in range(100):
        if os.path.exists(pty_file) and open(pty_file).read().strip():
            break
        time.sleep(0.05)
    port = open(pty_file).read().strip()

    failures = []

    def check(ok, what):
        print(("ok    " if ok else "FAIL  ") + what)
        if not ok:
            failures.append(what)

    def radio():
        ser = serial.Serial(port, drv.RA89RF4HWNRadio.BAUD_RATE, timeout=6)
        r = drv.RA89RF4HWNRadio(ser)
        r.status_fn = lambda s: None
        return r, ser

    try:
        # ---- download -----------------------------------------------------
        r, ser = radio()
        t0 = time.time()
        r.sync_in()
        print("download: %.1f s, firmware '%s'" % (time.time() - t0, r.FIRMWARE_VERSION))
        check(r.FIRMWARE_VERSION.startswith("RA89R"), "hello reports the RA89R version")
        m1 = r.get_memory(1)
        check(m1.freq == 144975000 and m1.name.strip() == "CH-01",
              "channel 1 = 144.975 CH-01 (got %d %r)" % (m1.freq, m1.name))
        m3 = r.get_memory(3)
        check(m3.freq == 430375000 and m3.rtone == 88.5,
              "channel 3 = 430.375, CTCSS 88.5 (got %d %s %s)" % (m3.freq, m3.tmode, m3.rtone))
        check(r.get_memory(200).empty, "channel 200 empty")
        settings = r.get_settings()
        sq = find_setting(settings, "squelch")
        check(sq is not None, "settings parse; squelch present (%s)" % (sq.value if sq else None))

        # ---- edit ----------------------------------------------------------
        m = chirp_common.Memory()
        m.number = 301
        m.freq = 446006250
        m.name = "PMR1"
        m.mode = "NFM"
        m.duplex = ""
        m.tmode = "Tone"
        m.rtone = 67.0
        r.set_memory(m)
        m = r.get_memory(301)
        for x in m.extra:
            if x.get_name() == "scanlists":
                x.value = x.value.get_options()[1]      # list 1
        r.set_memory(m)
        r.erase_memory(2)
        m4 = r.get_memory(4)
        m4.name = "RENAMED"
        r.set_memory(m4)
        sq.value = 6
        r.set_settings(settings)
        image1 = bytes(r.get_mmap().get_packed())
        ser.close()

        # ---- upload ------------------------------------------------------------
        r2, ser = radio()
        r2.load_mmap = None
        r2._mmap = r.get_mmap()
        r2.process_mmap()
        t0 = time.time()
        r2.sync_out()
        print("upload: %.1f s" % (time.time() - t0))
        ser.close()
        time.sleep(0.3)

        # ---- download again ------------------------------------------------------
        r3, ser = radio()
        r3.sync_in()
        ser.close()
        g = r3.get_memory(301)
        check(g.freq == 446006250 and g.name.strip() == "PMR1" and g.mode == "NFM" and
              g.tmode == "Tone" and g.rtone == 67.0,
              "channel 301 back (%d %r %s %s %s)" % (g.freq, g.name, g.mode, g.tmode, g.rtone))
        check(r3.get_memory(2).empty, "channel 2 deleted")
        check(r3.get_memory(4).name.strip() == "RENAMED", "channel 4 renamed")
        sq3 = find_setting(r3.get_settings(), "squelch")
        check(sq3 is not None and int(sq3.value) == 6, "squelch 6 after reboot")
        image3 = bytes(r3.get_mmap().get_packed())
        att = image3[0x8000 + 300 * 2:0x8000 + 300 * 2 + 2]
        check(att[0] & 0x80 == 0 and att[1] == 1,
              "channel 301 is scannable: exclude clear, list 1 (attr %s)" % att.hex())
        # The firmware clears each attribute's `exclude` bit (bit 7) on the
        # way in, as upstream does at boot; compare with that applied.
        image1 = bytearray(image1)
        for i in range(0x8000, 0x880E, 2):
            if image1[i:i + 2] != b"\xff\xff":
                image1[i] &= 0x7F
        image1 = bytes(image1)
        same = image1[:0xA178] == image3[:0xA178]
        if not same:
            diffs = [i for i in range(0xA178) if image1[i] != image3[i]]
            print("  %d bytes differ, first at 0x%04X: %02X -> %02X"
                  % (len(diffs), diffs[0], image1[diffs[0]], image3[diffs[0]]))
        check(same, "the second download equals what was uploaded (0x0000-0xA177)")
    finally:
        sim.stdin.close()
        rc = sim.wait(timeout=10)
    check(rc == 0, "simulator: stock codeplug and calibration unchanged")

    print("%s: %d failures" % ("FAIL" if failures else "all CHIRP e2e checks passed", len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
