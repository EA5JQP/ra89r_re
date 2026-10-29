#!/usr/bin/env python3
"""Pretend to be a Bluetooth earpiece for the RA89R, using this PC's adapter.

The radio's YBT100 module is the audio gateway; an earpiece links to it, opens a
SCO audio channel, and keys the radio with its button.  This script plays that
role with BlueZ, so the transmit audio is a known signal you can judge on a
second radio:

  * an auto-accepting pairing agent, and the adapter discoverable/pairable;
  * a Headset (HSP HS) profile -- and Handsfree (HFP HF) with `--hfp`, which
    PipeWire normally owns, see below -- whose RFCOMM channel it answers;
  * the SCO link: everything the radio sends (its receive audio) is saved to a
    WAV, and the "mic" carries an audio file, spoken text, or tones;
  * PTT, three ways (`--ptt`):
      inject   the radio console's `!i+IM_EAR_PTT_KEYDOWN/UP` -- the module is
               not involved, so SCO is not disturbed (default)
      button   the earpiece's own button over RFCOMM (`--button-cmd`, default
               the HSP button `AT+CKPD=200`); the firmware toggles on the
               module's `+IM_EAR_SIDE_SINGLE1`, so one press keys, one unkeys
      none     never key (stream only)

With `--radio-port` (the radio console, firmware with the `!` BT debug lines)
the script also makes the module scan for this PC and connect to it, and
prints the module's traffic (`BT <ms> < ...`) next to its own.

    # link only: no transmission.  Check the pairing and the SCO audio.
    python3 tools/ra89r_bt_headset.py --radio-port /dev/ttyUSB0

    # key the radio three times with a spoken phrase (listen on a 2nd radio)
    python3 tools/ra89r_bt_headset.py --radio-port /dev/ttyUSB0 --talk --repeat 3

    # the same with the earpiece button instead of the console
    python3 tools/ra89r_bt_headset.py --radio-port /dev/ttyUSB0 --talk --ptt button

    # your own audio (anything ffmpeg reads; it is resampled to 8 kHz mono)
    python3 tools/ra89r_bt_headset.py --radio-port /dev/ttyUSB0 --talk --audio voice.wav

**The script transmits only with `--talk`**, and it always releases PTT on the
way out (Ctrl-C included).

PipeWire registers HFP HF on this machine, so by default only HSP HS is offered.
If the module only speaks HFP, stop PipeWire's HF role first, or pass `--hfp`
and accept that the two registrations compete.  Needs dbus-python and
PyGObject (BlueZ D-Bus), pyserial for the console, and ffmpeg/espeak-ng for
audio.  See docs/ra89r_bt_debug.md.
"""

import argparse
import math
import os
import queue
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import wave

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ra89r_bt  # noqa: E402  (the console client and record parser)

BLUEZ = "org.bluez"
AGENT_PATH = "/ra89r/agent"
HSP_HS_UUID = "00001108-0000-1000-8000-00805f9b34fb"
HFP_HF_UUID = "0000111e-0000-1000-8000-00805f9b34fb"
HSP_AG_UUID = "00001112-0000-1000-8000-00805f9b34fb"
HFP_AG_UUID = "0000111f-0000-1000-8000-00805f9b34fb"
# The SCO link carries one of two codecs, whichever the earpiece side
# negotiated with the module (PipeWire's HF negotiates wideband):
#   cvsd   8 kHz s16le PCM at the HCI, chunked to the socket MTU
#   msbc   16 kHz mSBC: 60-byte packets = H2 header (0x01, seq) + one 57-byte
#          SBC frame (sync 0xAD) + one pad byte, one packet per 7.5 ms
RATES = {"cvsd": 8000, "msbc": 16000}
MSBC_PACKET, MSBC_FRAME = 60, 57
MSBC_SEQ = (0x08, 0x38, 0xC8, 0xF8)
SOL_SCO, SCO_OPTIONS = 17, 1
DEFAULT_TEXT = ("R A eighty nine R, Bluetooth headset test.  "
                "One, two, three, four, five.  Testing, testing.")

T0 = time.monotonic()


def log(who, text):
    print("%8.3f  %-6s %s" % (time.monotonic() - T0, who, text), flush=True)


# ------------------------------------------------------------------- audio

def tones(rate):
    """1 kHz, 700 Hz, 1.5 kHz, half a second each with gaps."""
    out = bytearray()
    for f in (1000, 700, 1500):
        for i in range(rate // 2):
            out += struct.pack("<h", int(0.6 * 32767 * math.sin(2 * math.pi * f * i / rate)))
        out += bytes(rate // 5 * 2)
    return bytes(out)


def ffmpeg(args, data=None):
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg is needed for the audio")
    return subprocess.run(["ffmpeg", "-v", "error"] + args, input=data,
                          check=True, stdout=subprocess.PIPE).stdout


def load_audio(rate, path=None, text=None):
    """Mono s16le PCM at `rate` from a file, espeak-ng text, or tones."""
    if path is None and text is None:
        return tones(rate), "tones 1000/700/1500 Hz"
    src = path
    if path is None:
        if not shutil.which("espeak-ng"):
            log("audio", "espeak-ng missing; using tones")
            return tones(rate), "tones"
        src = "/tmp/ra89r_bt_headset_say.wav"
        subprocess.run(["espeak-ng", "-s", "140", "-w", src, text], check=True)
    pcm = ffmpeg(["-i", src, "-ac", "1", "-ar", str(rate),
                  "-af", "loudnorm=I=-16:TP=-2", "-f", "s16le", "-"])
    return pcm, (os.path.basename(path) if path else "espeak: %r" % text)


def msbc_encode(pcm16k):
    """16 kHz PCM -> list of 57-byte mSBC frames."""
    raw = ffmpeg(["-f", "s16le", "-ar", "16000", "-ac", "1", "-i", "-",
                  "-c:a", "sbc", "-msbc", "1", "-f", "sbc", "-"], pcm16k)
    return [raw[i:i + MSBC_FRAME] for i in range(0, len(raw) - MSBC_FRAME + 1, MSBC_FRAME)]


def msbc_decode(packets):
    """Received SCO bytes (H2-framed mSBC) -> 16 kHz PCM; frames are found by
    their sync, so a lost byte costs one frame, not the rest."""
    frames = bytearray()
    i = 0
    while i + 2 + MSBC_FRAME <= len(packets):
        if packets[i] == 0x01 and packets[i + 2] == 0xAD:
            frames += packets[i + 2:i + 2 + MSBC_FRAME]
            i += MSBC_PACKET
        else:
            i += 1
    if not frames:
        return b""
    return ffmpeg(["-f", "sbc", "-i", "-", "-ac", "1", "-ar", "16000",
                   "-f", "s16le", "-"], bytes(frames))


def save_wav(path, pcm, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def level_dbfs(pcm):
    if len(pcm) < 2:
        return -120.0
    n = len(pcm) // 2
    s = struct.unpack("<%dh" % n, pcm[:n * 2])
    rms = math.sqrt(sum(x * x for x in s) / n)
    return 20 * math.log10(rms / 32768) if rms > 0 else -120.0


class Sco:
    """The SCO channel: listen, accept the module's connection, tell the codec
    from the first packet, then keep the mic flowing at the link's own rate
    (one packet of ours per packet received, or by the clock when the module
    is quiet) and keep everything received."""

    def __init__(self, adapter_addr):
        self.addr = adapter_addr
        self.mtu = 48
        self.codec = None
        self.codec_known = threading.Event()
        self.rx = bytearray()
        self.lock = threading.Lock()
        self.up = threading.Event()
        self.tx_done = threading.Event()
        self.stop = False
        self.packets_in = 0
        self.disconnects = 0
        self.tx = []               # packets (msbc) or PCM bytes (cvsd)
        self.tx_pos = 0
        self.seq = 0
        self.silence = None        # one encoded mSBC frame of silence

    @property
    def rate(self):
        return RATES[self.codec or "cvsd"]

    def start(self):
        s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET,
                          socket.BTPROTO_SCO)
        s.bind(self.addr.encode())
        s.listen(1)
        s.settimeout(0.5)
        self.listener = s
        threading.Thread(target=self._run, daemon=True).start()

    def play(self, pcm):
        """Queue PCM at `self.rate` for the mic."""
        tx = msbc_encode(pcm) if self.codec == "msbc" else pcm
        with self.lock:
            self.tx, self.tx_pos = tx, 0
            self.tx_done.clear()

    def duration(self, pcm):
        return len(pcm) / 2.0 / self.rate

    # -- one unit of mic data, codec by codec

    def _next_msbc(self):
        with self.lock:
            if self.tx_pos < len(self.tx):
                frame = self.tx[self.tx_pos]
                self.tx_pos += 1
            else:
                if self.tx:
                    self.tx = []
                    self.tx_done.set()
                frame = self.silence
        h2 = bytes((0x01, MSBC_SEQ[self.seq % 4]))
        self.seq += 1
        return h2 + frame + b"\x00"

    def _next_cvsd(self, n):
        with self.lock:
            if self.tx_pos >= len(self.tx):
                if self.tx:
                    self.tx = b""
                    self.tx_done.set()
                return bytes(n)
            chunk = self.tx[self.tx_pos:self.tx_pos + n]
            self.tx_pos += n
        return chunk + bytes(n - len(chunk))

    def _run(self):
        while not self.stop:
            try:
                conn, peer = self.listener.accept()
            except socket.timeout:
                continue
            except OSError as e:
                log("sco", "accept failed: %s" % e)
                return
            try:
                self.mtu = struct.unpack("<H", conn.getsockopt(SOL_SCO, SCO_OPTIONS, 2))[0]
            except OSError:
                pass
            log("sco", "audio link UP from %s (mtu %d)" % (peer, self.mtu))
            self.up.set()
            self._pump(conn)
            self.up.clear()
            self.disconnects += 1
            log("sco", "audio link DOWN")

    def _detect(self, data):
        if self.codec is None:
            self.codec = "msbc" if (len(data) >= 3 and data[0] == 0x01
                                    and data[2] == 0xAD) else "cvsd"
            if self.codec == "msbc" and self.silence is None:
                self.silence = msbc_encode(bytes(2 * 240))[0]
            log("sco", "codec %s (%d Hz)" % (self.codec, self.rate))
            self.codec_known.set()

    def _pump(self, conn):
        conn.settimeout(0.02)
        last = time.monotonic()
        credit = 0.0
        while not self.stop:
            try:
                data = conn.recv(1024)
                if not data:
                    return
                self._detect(data)
                self.rx += data
                self.packets_in += 1
                credit += len(data)
            except socket.timeout:
                if self.codec is None:
                    continue           # nothing to pace by yet
                credit += (time.monotonic() - last) * (8000 if self.codec == "msbc"
                                                        else 16000)
            except OSError:
                return
            last = time.monotonic()
            try:
                if self.codec == "msbc":
                    while credit >= MSBC_PACKET:
                        conn.send(self._next_msbc())
                        credit -= MSBC_PACKET
                else:
                    while credit >= self.mtu:
                        conn.send(self._next_cvsd(self.mtu))
                        credit -= self.mtu
            except OSError:
                return

    def received_pcm(self):
        """What the radio sent, as PCM at `self.rate`."""
        if self.codec == "msbc":
            return msbc_decode(bytes(self.rx))
        return bytes(self.rx)


# ------------------------------------------------------------- BlueZ glue

class Agent(dbus.service.Object):
    """Accept every pairing and service request without asking."""

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Release(self):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="s")
    def RequestPinCode(self, device):
        log("agent", "PIN requested by %s -> 0000" % device)
        return "0000"

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="u")
    def RequestPasskey(self, device):
        log("agent", "passkey requested by %s -> 0" % device)
        return dbus.UInt32(0)

    @dbus.service.method("org.bluez.Agent1", in_signature="ouq", out_signature="")
    def DisplayPasskey(self, device, passkey, entered):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def DisplayPinCode(self, device, pincode):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="ou", out_signature="")
    def RequestConfirmation(self, device, passkey):
        log("agent", "confirm %06d for %s: yes" % (passkey, device))

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="")
    def RequestAuthorization(self, device):
        log("agent", "authorize %s: yes" % device)

    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def AuthorizeService(self, device, uuid):
        log("agent", "service %s for %s: yes" % (uuid[4:8], device))

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Cancel(self):
        log("agent", "cancelled")


class Rfcomm:
    """The earpiece's control channel: answer the gateway's AT traffic, run the
    HFP service-level setup when asked, send the button."""

    def __init__(self, fd, kind, device):
        self.sock = socket.socket(fileno=fd)
        self.kind = kind
        self.device = device
        self.lines = queue.Queue()
        self.alive = True
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b""
        while self.alive:
            try:
                data = self.sock.recv(256)
            except OSError:
                break
            if not data:
                break
            buf += data
            while b"\r" in buf or b"\n" in buf:
                i = min(x for x in (buf.find(b"\r"), buf.find(b"\n")) if x >= 0)
                line, buf = buf[:i].decode("ascii", "replace"), buf[i + 1:]
                if line:
                    log(self.kind, "< " + line)
                    self.lines.put(line)
                    self._unsolicited(line)
        self.alive = False
        log(self.kind, "control channel closed")

    def _unsolicited(self, line):
        # The gateway asking the earpiece something it must answer.
        if self.kind == "hsp" and line.startswith("AT"):
            self.send_raw("\r\nOK\r\n")

    def send_raw(self, text):
        try:
            self.sock.send(text.encode())
        except OSError as e:
            log(self.kind, "send failed: %s" % e)

    def at(self, cmd, timeout=3.0):
        """Send an AT command and wait for OK/ERROR."""
        log(self.kind, "> " + cmd)
        self.send_raw(cmd + "\r")
        end = time.monotonic() + timeout
        got = []
        while time.monotonic() < end:
            try:
                line = self.lines.get(timeout=0.1)
            except queue.Empty:
                continue
            got.append(line)
            if line in ("OK", "ERROR") or line.startswith("+CME ERROR"):
                break
        return got

    def hfp_setup(self):
        """HFP service-level connection, HF side (minimal features)."""
        self.at("AT+BRSF=16")          # remote volume only
        self.at("AT+CIND=?")
        self.at("AT+CIND?")
        self.at("AT+CMER=3,0,0,1")


class Profile(dbus.service.Object):
    def __init__(self, bus, path, kind, owner):
        super().__init__(bus, path)
        self.kind = kind
        self.owner = owner

    @dbus.service.method("org.bluez.Profile1", in_signature="", out_signature="")
    def Release(self):
        pass

    @dbus.service.method("org.bluez.Profile1", in_signature="oha{sv}", out_signature="")
    def NewConnection(self, device, fd, props):
        fd = fd.take()
        log(self.kind, "control channel from %s" % device)
        self.owner.on_rfcomm(Rfcomm(fd, self.kind, device))

    @dbus.service.method("org.bluez.Profile1", in_signature="o", out_signature="")
    def RequestDisconnection(self, device):
        log(self.kind, "disconnect requested by %s" % device)


# --------------------------------------------------------------- the radio

class Radio:
    """The radio console in the background: print its BT records, let the
    sequence wait for one."""

    def __init__(self, port):
        self.con = ra89r_bt.Console(port)
        self.q = queue.Queue()
        self.stop = False
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while not self.stop:
            for rec in self.con.records(0.2):
                if rec.kind in "<>P":
                    text = (rec.module_line[0] + "  [%s]" % rec.module_line[1]
                            if rec.kind == "<" else rec.body)
                    log("radio" + rec.kind, text)
                self.q.put(rec)

    def cmd(self, line):
        self.con.write_line(line)

    def wait(self, pred, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                rec = self.q.get(timeout=0.1)
            except queue.Empty:
                continue
            if pred(rec):
                return rec
        return None

    def status(self):
        while not self.q.empty():
            self.q.get_nowait()
        self.cmd("s")
        rec = self.wait(lambda r: r.kind == "S", 3.0)
        return rec.fields if rec else None


# ------------------------------------------------------------- the sequence

class Headset:
    def __init__(self, a):
        self.a = a
        self.rfcomm = None
        self.rf_up = threading.Event()
        self.radio = Radio(a.radio_port) if a.radio_port else None
        self.keyed = False
        self.loop = GLib.MainLoop()

    def on_rfcomm(self, rf):
        self.rfcomm = rf
        if rf.kind == "hfp":
            threading.Thread(target=rf.hfp_setup, daemon=True).start()
        self.rf_up.set()

    def setup_bluez(self):
        dbus.mainloop.glib.threads_init()
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        bus = dbus.SystemBus()
        self.bus = bus
        mgr = dbus.Interface(bus.get_object(BLUEZ, "/"),
                             "org.freedesktop.DBus.ObjectManager")
        path = None
        for p, ifs in mgr.GetManagedObjects().items():
            if "org.bluez.Adapter1" in ifs and (not self.a.adapter or p.endswith(self.a.adapter)):
                path = p
                break
        if not path:
            sys.exit("no Bluetooth adapter")
        props = dbus.Interface(bus.get_object(BLUEZ, path), "org.freedesktop.DBus.Properties")
        self.adapter_props = props
        self.adapter_path = path
        self.addr = str(props.Get("org.bluez.Adapter1", "Address"))
        self.saved = {k: props.Get("org.bluez.Adapter1", k)
                      for k in ("Alias", "Discoverable", "Pairable")}
        props.Set("org.bluez.Adapter1", "Powered", dbus.Boolean(True))
        props.Set("org.bluez.Adapter1", "Alias", dbus.String(self.a.name))
        props.Set("org.bluez.Adapter1", "DiscoverableTimeout", dbus.UInt32(0))
        props.Set("org.bluez.Adapter1", "Discoverable", dbus.Boolean(True))
        props.Set("org.bluez.Adapter1", "Pairable", dbus.Boolean(True))
        log("bt", "adapter %s %s as %r, discoverable, pairable"
            % (path, self.addr, self.a.name))

        self.agent = Agent(bus, AGENT_PATH)
        am = dbus.Interface(bus.get_object(BLUEZ, "/org/bluez"), "org.bluez.AgentManager1")
        am.RegisterAgent(AGENT_PATH, "NoInputNoOutput")
        am.RequestDefaultAgent(AGENT_PATH)

        pm = dbus.Interface(bus.get_object(BLUEZ, "/org/bluez"), "org.bluez.ProfileManager1")
        self.profiles = []
        wanted = [("hsp", HSP_HS_UUID)] + ([("hfp", HFP_HF_UUID)] if self.a.hfp else [])
        for kind, uuid in wanted:
            ppath = "/ra89r/" + kind
            Profile(bus, ppath, kind, self)
            try:
                # No "Role": both, so the PC can also open the channel
                # (Device1.ConnectProfile) the way an earpiece reconnects.
                pm.RegisterProfile(ppath, uuid, {
                    "Name": "RA89R test " + kind.upper(),
                    "RequireAuthentication": dbus.Boolean(False),
                    "RequireAuthorization": dbus.Boolean(False),
                })
                self.profiles.append(ppath)
                log("bt", "registered %s (%s)" % (kind.upper(), uuid[4:8]))
            except dbus.DBusException as e:
                log("bt", "could not register %s: %s" % (kind.upper(), e.get_dbus_message()))
        self.pm = pm
        self.am = am

    def teardown_bluez(self):
        for p in self.profiles:
            try:
                self.pm.UnregisterProfile(p)
            except dbus.DBusException:
                pass
        try:
            self.am.UnregisterAgent(AGENT_PATH)
        except dbus.DBusException:
            pass
        for k, v in self.saved.items():
            try:
                self.adapter_props.Set("org.bluez.Adapter1", k, v)
            except dbus.DBusException:
                pass

    # -- linking

    def link_via_console(self):
        """Have the module scan, find this PC by name, connect to it."""
        r = self.radio
        st = r.status()
        if st is None:
            log("radio", "no reply from the console -- link it from the radio's "
                         "BT menu instead (Pair, pick %r)" % self.a.name)
            return
        if not st.get("en"):
            log("radio", "Bluetooth is off on the radio; turn the BT Switch on")
            return
        if st.get("link"):
            log("radio", "the module already has a headset linked; disconnecting it")
            r.cmd("aAT+BT_DISCN")
            time.sleep(1.5)
        r.cmd("t1")
        r.cmd("aAT+BT_SCAN=ON")
        log("radio", "module scanning for %r ..." % self.a.name)
        rec = r.wait(lambda x: x.kind == "<" and x.module_line[1] == "EARDEV"
                     and self.a.name in x.module_line[0], self.a.scan_timeout)
        if rec is None:
            log("radio", "the module did not report this PC; it may filter by "
                         "device class.  Try the BT menu's Pair list, or "
                         "`--connect-addr`.")
            r.cmd("aAT+BT_SCAN=OFF")
            return
        payload = rec.module_line[0].split(":", 1)[1]
        r.cmd("aAT+BT_SCAN=OFF")
        time.sleep(0.3)
        r.cmd("aAT+EAR_CONN=" + payload.split(",")[0])

    def connect_to(self, addr):
        """The PC side opens the link: pair with the module (its classic
        identity, class "phone", `RETEVIS RA89R(BT)`), then open the headset
        profile to its gateway -- what an earpiece does when it reconnects."""
        path = "%s/dev_%s" % (self.adapter_path, addr.upper().replace(":", "_"))
        mgr = dbus.Interface(self.bus.get_object(BLUEZ, "/"),
                             "org.freedesktop.DBus.ObjectManager")
        if path not in mgr.GetManagedObjects():
            log("bt", "searching for %s ..." % addr)
            ad = dbus.Interface(self.bus.get_object(BLUEZ, self.adapter_path),
                                "org.bluez.Adapter1")
            ad.SetDiscoveryFilter({"Transport": "bredr"})
            ad.StartDiscovery()
            end = time.monotonic() + 20
            while path not in mgr.GetManagedObjects() and time.monotonic() < end:
                time.sleep(0.5)
            ad.StopDiscovery()
            if path not in mgr.GetManagedObjects():
                log("bt", "%s not found (is the radio's BT Switch on?)" % addr)
                return
        dev = dbus.Interface(self.bus.get_object(BLUEZ, path), "org.bluez.Device1")
        props = dbus.Interface(self.bus.get_object(BLUEZ, path),
                               "org.freedesktop.DBus.Properties")
        if not props.Get("org.bluez.Device1", "Paired"):
            log("bt", "pairing with %s" % addr)
            try:
                dev.Pair(timeout=40)
            except dbus.DBusException as e:
                log("bt", "pairing failed: %s" % e.get_dbus_message())
                return
            props.Set("org.bluez.Device1", "Trusted", dbus.Boolean(True))
        if props.Get("org.bluez.Device1", "Connected"):
            # A link left over from an earlier run (PipeWire keeps HFP up):
            # drop it, so the module sees a fresh earpiece connect and the
            # firmware opens the call (CALL=ON on +IM_BT_EAR_CONN).
            log("bt", "dropping the existing link first")
            try:
                dev.Disconnect(timeout=10)
            except dbus.DBusException as e:
                log("bt", "disconnect: %s" % e.get_dbus_message())
            time.sleep(2.0)
        uuids = [str(u) for u in props.Get("org.bluez.Device1", "UUIDs")]
        log("bt", "module services: %s" % ", ".join(u[4:8] for u in uuids))
        for kind, uuid in (("hfp", HFP_AG_UUID), ("hsp", HSP_AG_UUID)):
            if kind == "hfp" and not self.a.hfp:
                continue
            if uuid not in uuids:
                continue
            log("bt", "opening %s to the module" % kind.upper())
            try:
                dev.ConnectProfile(uuid, timeout=30)
                return
            except dbus.DBusException as e:
                log("bt", "%s connect failed: %s" % (kind.upper(), e.get_dbus_message()))
        log("bt", "no headset profile could be opened")

    def connect_addr(self, addr):
        self.radio.cmd("t1")
        self.radio.cmd("aAT+EAR_CONN=" + addr)

    # -- PTT

    def ptt(self, down):
        mode = self.a.ptt
        if mode == "none":
            return
        if mode == "inject":
            self.radio.cmd("i+IM_EAR_PTT_KEYDOWN" if down else "i+IM_EAR_PTT_KEYUP")
        elif mode == "button":
            if self.rfcomm is None or not self.rfcomm.alive:
                log("ptt", "no control channel for the button")
                return
            log("ptt", "button %s" % ("press (key)" if down else "press (unkey)"))
            self.rfcomm.at(self.a.button_cmd, timeout=1.0)
        self.keyed = down
        if self.radio:
            want = 1 if down else 0
            rec = self.radio.wait(lambda r: r.kind == "P" and r.fields.get("tx") == want, 2.0)
            log("ptt", ("radio TX %s" % ("ON" if down else "off")) if rec else
                "no TX %s seen on the console within 2 s" % ("ON" if down else "off"))

    def release(self):
        """Never leave the radio transmitting."""
        if not self.radio:
            return
        self.radio.cmd("i+IM_EAR_PTT_KEYUP")
        st = self.radio.status()
        if st and st.get("tx"):
            log("ptt", "radio STILL transmitting -- press its PTT or switch it off")

    # -- run

    def sequence(self):
        try:
            self._sequence()
        except Exception as e:  # pragma: no cover
            log("error", repr(e))
        finally:
            self.release()
            GLib.idle_add(self.loop.quit)

    def _sequence(self):
        a = self.a
        if a.connect_to:
            if self.radio:
                self.radio.cmd("t1")
            self.connect_to(a.connect_to)
        elif self.radio and a.connect_addr:
            self.connect_addr(a.connect_addr)
        elif self.radio and not a.no_link:
            self.link_via_console()
        else:
            log("bt", "waiting for the radio to connect: BT menu -> Pair -> %r" % a.name)

        end = time.monotonic() + a.link_timeout
        asked_call = False
        while time.monotonic() < end and not (self.rf_up.is_set() or self.sco.up.is_set()):
            time.sleep(0.5)
            if self.radio and not asked_call and time.monotonic() > end - a.link_timeout + 8:
                st = self.radio.status()
                if st and st.get("link") and not st.get("sco"):
                    log("radio", "linked but no audio link: asking for AT+BT_CALL=ON")
                    self.radio.cmd("aAT+BT_CALL=ON")
                    asked_call = True
        if self.rf_up.is_set():
            log("bt", "linked; control channel %s is ours" % self.rfcomm.kind.upper())
        elif self.sco.up.is_set():
            log("bt", "linked; the control channel went to another HF (PipeWire), "
                      "so `--ptt button` is not available")
        else:
            log("bt", "no link within %gs" % a.link_timeout)
            return
        if not self.sco.up.wait(15):
            log("sco", "no SCO audio link within 15 s; the radio sends "
                       "AT+BT_CALL=ON on connect -- see the radio trace")
            if self.radio:
                self.radio.cmd("aAT+BT_CALL=ON")
            if not self.sco.up.wait(10):
                return

        if not self.sco.codec_known.wait(5):
            log("sco", "the module sends no audio; assuming CVSD")
            self.sco.codec = "cvsd"
        time.sleep(1.0)
        log("audio", "radio receive audio over SCO so far: %d packets" % self.sco.packets_in)

        pcm, what = load_audio(self.sco.rate, a.audio,
                               None if a.audio or a.tones else a.say)
        log("audio", "mic signal: %s, %.1f s at %d Hz"
            % (what, self.sco.duration(pcm), self.sco.rate))

        for i in range(a.repeat):
            if a.talk:
                log("ptt", "--- transmission %d/%d ---" % (i + 1, a.repeat))
                self.ptt(True)
                time.sleep(a.lead)
            else:
                log("audio", "--- mic stream %d/%d (no PTT: --talk not given) ---"
                    % (i + 1, a.repeat))
            sco_before = self.sco.disconnects
            self.sco.play(pcm)
            self.sco.tx_done.wait(self.sco.duration(pcm) + 3)
            if self.sco.disconnects != sco_before or not self.sco.up.is_set():
                log("sco", "the audio link DROPPED during this transmission")
            time.sleep(a.tail)
            if a.talk:
                self.ptt(False)
            time.sleep(a.gap)
            if not self.sco.up.is_set():
                log("sco", "waiting for the audio link to come back")
                if not self.sco.up.wait(10):
                    break

        if self.a.listen:
            log("audio", "recording radio receive audio for %gs" % self.a.listen)
            time.sleep(self.a.listen)

    def run(self):
        self.setup_bluez()
        self.sco = Sco(self.addr)
        self.sco.start()
        threading.Thread(target=self.sequence, daemon=True).start()
        try:
            self.loop.run()
        except KeyboardInterrupt:
            log("bt", "interrupted")
            self.release()
        finally:
            self.sco.stop = True
            if self.radio:
                self.radio.cmd("t0")
                self.radio.stop = True
            self.teardown_bluez()
            if self.sco.rx:
                pcm = self.sco.received_pcm()
                save_wav(self.a.record, pcm, self.sco.rate)
                log("audio", "radio receive audio: %s (%s, %.1f s, %.1f dBFS)"
                    % (self.a.record, self.sco.codec, len(pcm) / 2 / self.sco.rate,
                       level_dbfs(pcm)))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--radio-port", help="radio console (for linking, trace, inject PTT)")
    ap.add_argument("--adapter", help="hciN (default: the first)")
    ap.add_argument("--name", default="RA89R-TestHS", help="name the module will see")
    ap.add_argument("--hfp", action="store_true", help="also offer HFP HF (PipeWire may own it)")
    ap.add_argument("--no-link", action="store_true",
                    help="don't drive the module; link from the radio's BT menu")
    ap.add_argument("--connect-to", metavar="ADDR",
                    help="the PC pairs with and connects to the module itself "
                         "(its classic address, `RETEVIS RA89R(BT)`)")
    ap.add_argument("--connect-addr", help="AT+EAR_CONN address to use instead of a scan")
    ap.add_argument("--scan-timeout", type=float, default=25.0)
    ap.add_argument("--link-timeout", type=float, default=60.0)
    ap.add_argument("--talk", action="store_true", help="KEY THE TRANSMITTER")
    ap.add_argument("--ptt", choices=["inject", "button", "none"], default="inject")
    ap.add_argument("--button-cmd", default="AT+CKPD=200",
                    help="what the earpiece button sends (HSP: AT+CKPD=200)")
    ap.add_argument("--audio", help="audio file for the mic (any ffmpeg input)")
    ap.add_argument("--say", default=DEFAULT_TEXT, help="text for espeak-ng")
    ap.add_argument("--tones", action="store_true", help="tones instead of speech")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--lead", type=float, default=0.6, help="s keyed before the audio")
    ap.add_argument("--tail", type=float, default=0.4, help="s keyed after the audio")
    ap.add_argument("--gap", type=float, default=2.0, help="s between transmissions")
    ap.add_argument("--listen", type=float, default=0.0,
                    help="record the radio's receive audio this long at the end")
    ap.add_argument("--record", default="/tmp/ra89r_bt_rx.wav",
                    help="where the radio's SCO audio is saved")
    a = ap.parse_args(argv)
    if a.ptt == "inject" and a.talk and not a.radio_port:
        ap.error("--ptt inject needs --radio-port")
    Headset(a).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
