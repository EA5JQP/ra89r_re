# RA89R Bluetooth debug channel (`!` console lines, `tools/ra89r_bt.py`)

A way to watch and drive the Bluetooth module (YBT100 on USART3, see
`ra89r_bluetooth.md`) from a PC while the radio runs the normal K1 GUI.  It
was added on `driver/bluetooth-merge` (2026-10-29) to chase the headset-PTT
failure, and it replaces the BT console commands the develop merge dropped
(the raw `U` capture and the `A`/`N` status lines: develop reuses `U` for the
airband automation).

## Firmware side — `App/app/bt_debug.c`

Every single-letter console command is taken, so the BT debug commands are
**whole lines** that start with `!` and end with CR or LF (ESC drops a line; an
unterminated line is dropped after 5 s so it cannot eat the console).  Replies
are always `BT <ms> <kind> <body>`:

| line | effect | reply kind |
|---|---|---|
| `!s` | status | `S en= state= link= sco= resume= ptt= radio_ptt= type= allow= tx= bt_tx= found= rx= ver=` |
| `!t1` / `!t0` | trace on / off (on prints a status as the baseline) | `T`, then `<`, `>`, `P` lines |
| `!a<text>` | send `<text>\r\n` to the module | `A`, plus `>` while tracing |
| `!i<text>` | feed `<text>` to the parser as if the module sent it | `I`, plus `<` while tracing |
| `!r` | restart the module (`bt_set_enabled(false)` then `true`) | `R` |
| `!l` | hex of the last 64 bytes received from the module | `L` |
| `!?` | help | `H` |

Trace lines:

- `BT <ms> < <line> [<EVENT>]`: a line from the module, with the event the
  firmware parser (`bt_parse_line`) made of it.  **`[NONE]` means no prefix
  matched**, so the service never saw it.
- `BT <ms> > <line>`: a command the firmware sent to the module.
- `BT <ms> P headset= radio= type= allow= tx=`: a transition of the headset
  PTT flag, the radio's own PTT key or the TX chain.  `allow` is
  `bt_ptt_source_active()`, the PTT-type decision `tx_poll_ptt()` uses.

The transport reports every line through `bluetooth_set_trace_cb()`
(`driver/bluetooth.c`); the PTT/TX picture is sampled by `bt_debug_tick()` on
the 10 ms BT slice.  The module's view of the radio (key, TX, PTT type,
restart) comes in through `bt_debug_io_t`, so the core is host-tested by
`firmware/tools/test_bt_debug.c` (in `check_all.sh`).

The console's input passes through the CAT frame detector first.  Printable
ASCII cannot form a valid FT-817 frame (each needs zero bytes or BCD plus a
control-byte opcode), so a `!` line never keys CAT PTT.  The console queues 64
bytes and drains one per main-loop pass, so the host tool writes one
character every 3 ms rather than bursting a line.

## Host side — `tools/ra89r_bt.py`

```sh
python3 tools/ra89r_bt.py ports
python3 tools/ra89r_bt.py --port /dev/ttyUSB0 status
python3 tools/ra89r_bt.py --port /dev/ttyUSB0 trace --log /tmp/bt.log
python3 tools/ra89r_bt.py --port /dev/ttyUSB0 send 'AT+CONN_STATE?'
python3 tools/ra89r_bt.py --port /dev/ttyUSB0 inject '+IM_EAR_PTT_KEYDOWN'
python3 tools/ra89r_bt.py --port /dev/ttyUSB0 restart
python3 tools/ra89r_bt.py --port /dev/ttyUSB0 ptt-test
python3 tools/ra89r_bt.py --port /dev/ttyUSB0 shell
python3 tools/ra89r_bt.py --port /dev/ttyUSB1 sniff     # USB-UART on the module's TX line
python3 tools/ra89r_bt.py --selftest                    # against the test double
```

`ptt-test` reads the status, turns the trace on, asks for a headset PTT press
and gives a verdict on where the chain *module → parser → headset flag → PTT
type → TX* stops:

| verdict | meaning / next step |
|---|---|
| BT off / no headset linked | switch it on / pair first |
| module sent NOTHING | the button produced no UART line; if `sco=0`, try `send AT+BT_CALL=ON` and press again (the stock opens the call on a PTT press, see below) |
| line not recognised | the module reports the button with a prefix the parser lacks: add it to `bt_events[]` |
| flag never set | the event parsed but `app/bt.c` did not act on it |
| PTT type does not let the headset key | `allow=0`: BT menu PTT is `Local` |
| allowed but `tx=0` | the TX chain did not key: `tx_poll_ptt()` |
| TX keyed | works; a "flag still set" note means the toggle (`SIDE_SINGLE1`) or a missed release |

`sniff` uses no firmware at all.  It reads a USB-UART adapter tapped onto the
module's TX line (115200 8N1) and classifies each line with the same prefix
table.  That table is copied from `driver/bluetooth.c`: keep the two in step.

`tools/ra89r_bt_console_sim.py` is the test double: the same protocol on a PTY,
with a fake linked headset that presses 0.6 s after the first `!t1`
(`--button ptt|side|unknown|none`, `--ptt-type`, `--unlinked`).

## Headset emulator — `tools/ra89r_bt_headset.py`

This script makes the PC (BlueZ, D-Bus) act as the earpiece, so the transmit
audio is a known signal you can judge on a second radio.  It pairs with the
module's classic identity (`RETEVIS RA89R(BT)`, `5A:DF:CD:EF:C3:11` on this
radio, device class "phone").  It opens HFP to the module's gateway, gets the
SCO link, streams spoken text (espeak-ng), tones or a file as the "mic", and
saves the radio's receive audio to a WAV.  PTT goes through the console's `!i`
inject by default.

```sh
# no transmission: link, codec, mic stream, receive recording
python3 tools/ra89r_bt_headset.py --radio-port /dev/ttyUSB0 --connect-to 5A:DF:CD:EF:C3:11 --hfp
# transmit: key, speak, unkey, three times
python3 tools/ra89r_bt_headset.py --radio-port /dev/ttyUSB0 --connect-to 5A:DF:CD:EF:C3:11 --hfp --talk --repeat 3
```

Observed on the radio (2026-10-29, no TX), using the PC side:

- The module's scan (`AT+BT_SCAN=ON`) does **not** list the PC, whose device
  class is "computer"; this is probably a class filter.  `AT+EAR_CONN=<PC
  address>` did not reach the PC either.  The PC connecting to the module
  works: Secure Simple Pairing (numeric comparison), then HFP.
- The module offers `1101 110a 110b 110c 110d 110e 111f`: SPP, A2DP, AVRCP
  and the **HFP gateway**, with no HSP.
- PipeWire owns HFP HF on this PC, so it takes the RFCOMM control channel.
  The SCO socket still comes to the script (a listener bound to the adapter
  address beats PipeWire's wildcard).  PipeWire negotiates **mSBC**, so SCO
  carries 60-byte H2-framed mSBC packets (16 kHz).  The script detects the
  codec from the first packet and encodes and decodes it with ffmpeg's SBC.
- The firmware's connect flow ran as designed: `+IM_BT_EAR_CONN`, then
  `AT+BT_CALL=ON`, `+IM_SCO_CONN`.  A run left connected (PipeWire keeps the
  link) leaves the radio linked with SCO down, so the script drops a stale
  link before connecting.
- `--ptt button` (the earpiece's own button) needs the control channel, so
  PipeWire's HF role must be off for it.

## Stock reference for the headset PTT

Located in stock V49 (static only, not verified on the radio):

- The parser `FUN_08022974` sets `0x200094EA` (state `+6`) on
  `+IM_EAR_PTT_KEYDOWN` and clears it on `+IM_EAR_PTT_KEYUP`.  It has **no**
  `+IM_EAR_SIDE_SINGLE` prefix; the port's `SIDE_SINGLE1` toggle is the port's
  own, from an on-radio capture.
- `0x0800F370` returns that flag after calling `FUN_08016A14`, which on a
  press runs `0x0801A75C`.  The first time, that queues `AT+SPKGAIN` and, when
  SCO is down (state `+0xA` = 0), `AT+BT_CALL=ON` (`0x0801A774` → `FUN_0802213C`).
  On release, `0x0801A7E4` clears state `+7` and marks the call up again.  The
  main loop (`0x080244D4`) and the T/R path (`0x0801A2C0`) call `0x0800F370`.
- The port does not do the CALL=ON-on-press yet.

## Open

- Run `ptt-test` on the radio with the headset that fails, and record which
  verdict it gives and the raw lines.
