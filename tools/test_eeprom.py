#!/usr/bin/env python3
"""
test_eeprom.py -- end-to-end test for tools/ra89r_eeprom.py against the
ra89r_eeprom_sim.py test double.

    python3 tools/test_eeprom.py

Starts the simulator on a PTY, then drives the real host tool as a subprocess
and checks the whole backup/restore cycle, the size guard and the checksum
failure path.  Exits non-zero on the first failure.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TOOL = os.path.join(HERE, "ra89r_eeprom.py")
SIM = os.path.join(HERE, "ra89r_eeprom_sim.py")
SIZE = 64 * 1024

_failures = 0


def check(name, cond, detail=""):
    global _failures
    if cond:
        print("  PASS  %s" % name)
    else:
        _failures += 1
        print("  FAIL  %s %s" % (name, detail))


def run(args, timeout=60):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def tool(port, *rest):
    return [sys.executable, TOOL, "--port", port, "--timeout", "1"] + list(rest)


def start_sim(tmp, **opts):
    pty_file = os.path.join(tmp, "pty")
    if os.path.exists(pty_file):
        os.remove(pty_file)
    args = [sys.executable, SIM, "--pty", pty_file, "--size", hex(SIZE)]
    for k, v in opts.items():
        args += ["--" + k.replace("_", "-"), str(v)]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(pty_file) and os.path.getsize(pty_file) > 0:
            with open(pty_file) as f:
                return proc, f.read().strip()
        time.sleep(0.05)
    proc.terminate()
    raise RuntimeError("simulator did not publish a PTY path")


def expected_chip(size):
    return bytes(((i * 7 + 0x11) & 0xFF) for i in range(size))


def main():
    tmp = tempfile.mkdtemp(prefix="ra89r-eeprom-test-")
    try:
        print("test: backup writes the chip exactly")
        sim, port = start_sim(tmp)
        try:
            out = os.path.join(tmp, "backup.bin")
            r = run(tool(port, "backup", out))
            check("backup exits 0", r.returncode == 0, r.stderr.strip())
            got = open(out, "rb").read() if os.path.exists(out) else b""
            check("backup size", len(got) == SIZE, "got %d" % len(got))
            check("backup content", got == expected_chip(SIZE),
                  "content differs")
        finally:
            sim.terminate()

        print("test: restore writes a new image back")
        sim, port = start_sim(tmp)
        try:
            img = os.path.join(tmp, "new.bin")
            new = bytes(((i * 3 + 0x42) & 0xFF) for i in range(SIZE))
            open(img, "wb").write(new)
            r = run(tool(port, "restore", img, "--yes"))
            check("restore exits 0", r.returncode == 0, r.stderr.strip())
            back = os.path.join(tmp, "back.bin")
            r2 = run(tool(port, "backup", back))
            check("readback exits 0", r2.returncode == 0, r2.stderr.strip())
            got = open(back, "rb").read() if os.path.exists(back) else b""
            check("restored content", got == new, "content differs")
        finally:
            sim.terminate()

        print("test: restore refuses a wrong-size image")
        sim, port = start_sim(tmp)
        try:
            small = os.path.join(tmp, "small.bin")
            open(small, "wb").write(b"\x00" * 128)
            r = run(tool(port, "restore", small, "--yes"))
            check("wrong size refused", r.returncode != 0,
                  "returned %d" % r.returncode)
            check("wrong size message",
                  "refusing" in (r.stderr + r.stdout).lower(),
                  r.stderr.strip())
        finally:
            sim.terminate()

        print("test: restore without --yes does not touch the chip")
        sim, port = start_sim(tmp)
        try:
            img = os.path.join(tmp, "noyes.bin")
            open(img, "wb").write(b"\x55" * SIZE)
            r = run(tool(port, "restore", img))
            check("no --yes refused", r.returncode != 0,
                  "returned %d" % r.returncode)
        finally:
            sim.terminate()

        print("test: a corrupted restore is reported")
        sim, port = start_sim(tmp, corrupt=1000)
        try:
            img = os.path.join(tmp, "corrupt.bin")
            open(img, "wb").write(b"\xA5" * SIZE)
            r = run(tool(port, "restore", img, "--yes"))
            check("corrupt restore fails", r.returncode != 0,
                  "returned %d" % r.returncode)
        finally:
            sim.terminate()

        print("test: --verify reads the image back")
        sim, port = start_sim(tmp)
        try:
            img = os.path.join(tmp, "verify.bin")
            open(img, "wb").write(bytes(range(256)) * (SIZE // 256))
            r = run(tool(port, "restore", img, "--yes", "--verify"))
            check("verify restore exits 0", r.returncode == 0, r.stderr.strip())
            check("verify reports a match", "verify" in r.stdout.lower(),
                  r.stdout.strip())
        finally:
            sim.terminate()

        print("test: --dry-run writes nothing")
        sim, port = start_sim(tmp)
        try:
            img = os.path.join(tmp, "dry.bin")
            open(img, "wb").write(b"\x00" * SIZE)
            r = run(tool(port, "restore", img, "--yes", "--dry-run"))
            check("dry run exits 0", r.returncode == 0, r.stderr.strip())
            back = os.path.join(tmp, "dryback.bin")
            run(tool(port, "backup", back))
            got = open(back, "rb").read() if os.path.exists(back) else b""
            check("dry run left the chip alone", got == expected_chip(SIZE),
                  "content differs")
        finally:
            sim.terminate()

    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if _failures:
        print("%d check(s) failed" % _failures)
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
