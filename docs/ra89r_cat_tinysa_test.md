# CAT + tinySA-MCP receive test

**Status: the tool, its offline checks (`--sim`, `--mcp-selftest`) and a first
bench run pass.**  On 2026-10-23 the radio (CH340, `/dev/ttyUSB0`) and a
classic tinySA (CDC, `/dev/ttyACM1`) were connected through the tinySA's **low
output**; 145.5000 MHz FM and 118.0000 MHz AM both showed a clean carrier
off/on/off S-meter triplet.  The UHF case needs the **high output** connector
and is still to be run.  The bench table is below.

`tools/cat_rf_test.py` is the test; `tools/tinysa_mcp_sim.py` is the stdio MCP
test double that lets the MCP client half run without hardware.

## What it proves

For each band it does a **carrier off / carrier on / carrier off** triplet:

| step | interface | what is asserted |
|---|---|---|
| tune | CAT `SET_FREQ` + `SET_MODE` | `READ` reports the frequency that was set, and the mode byte (0x08 FM / 0x04 AM) |
| baseline | CAT `SM` | the S-meter is sampled (median of N) |
| carrier on | tinySA MCP `set_generator` | `get_generator_state` confirms output on, the CW frequency and the stage's range; the CAT `SM` rises by at least `--min-delta` |
| carrier off | tinySA MCP `set_generator` | the CAT `SM` returns within `--max-recovery` of the baseline |

The `STATUS` extension (`0xE1`) is printed at each band: squelch, audio lane
(BK4829/BK4815), scan mode, 9/7-bit RSSI and the squelch level.

## Bench

```
radio USB-C (CH340)  ->  /dev/ttyUSB*    CAT, 115200 8N1 (ENABLE_CAT is on by default)
tinySA USB (CDC)     ->  /dev/ttyACM*    driven through the tinySA MCP server
tinySA output  ->  ATTENUATOR  ->  radio antenna jack
```

The radio must be running a build with `ENABLE_CAT` (`App/k1_features.h`,
default on).  CAT and the console share USART1: the test sends 5-byte bursts
with >= 150 ms gaps, which is the auto-detect's requirement, and it never sends
a console character.  It restores the radio's original CAT frequency and mode
and switches the generator off at the end (also on failure).

The classic tinySA has **two output connectors**: the low output covers VHF and
the airband (up to ~350 MHz), the high output starts at ~240 MHz and is what
446 MHz needs.  The test picks the stage from the frequency and validates the
level against it; the cable has to match.

### Safety

* **The test never keys the radio** -- no `PTT` frame is sent, only tune/mode/
  read/S-meter.
* The tinySA's SMA is also its analyser input, rated **+10 dBm** at 0 dB
  attenuation; a handheld transmitter is ~+37 dBm.  Put an external power
  attenuator in line whenever the radio can ever transmit and pass it as
  `--pad-db`; the log then reports both the generator and the antenna level.
* Defaults are deliberately quiet: **-30 dBm** for VHF/air (low stage),
  **-38 dBm** for UHF (high stage).  A level outside the selected stage's range
  is refused before anything is transmitted.

## Interfaces used

CAT (FT-817 frames, `firmware/App/cat.h`, `docs/ra89r_carserial.md`):

```
set freq   BCD(Hz/10) 4 B + 0x01     -> ACK 0x00
set mode   mode 0 0 0   + 0x07       -> ACK 0x00     (0x08 FM, 0x04 AM)
read       00 00 00 00  + 0x03       -> 4 BCD + mode
S-meter    00 00 00 00  + 0x02       -> 00 00 00 rssi 0x02
STATUS     00 00 00 00  + 0xE1       -> status rssi_lo rssi_hi sql 0xE1
```

tinySA MCP tools (`~/Repos/tinySA_mcp`, the same server opencode uses):

```
connect(port)                 open the instrument once
set_generator(port, output, frequency_hz, level_dbm, high_output)
get_generator_state(port)     output / CW frequency / level range read-back
disconnect()
```

The client is plain JSON-RPC over the server's stdio (`initialize` ->
`notifications/initialized` -> `tools/call`), so the test needs no MCP SDK.

## Running

```sh
# what is plugged in?
python3 tools/cat_rf_test.py --ports

# show the plan without touching anything
python3 tools/cat_rf_test.py --dry-run --cat-port /dev/ttyUSB0 --tinysa-port /dev/ttyACM1

# VHF + UHF, default levels
python3 tools/cat_rf_test.py --cat-port /dev/ttyUSB0 --tinysa-port /dev/ttyACM1

# one band, quieter, with 10 dB of external pad
python3 tools/cat_rf_test.py --cat-port /dev/ttyUSB0 --tinysa-port /dev/ttyACM1 \
    --band vhf --level -40 --pad-db 10

# airband AM receive check (opt-in)
python3 tools/cat_rf_test.py --cat-port /dev/ttyUSB0 --tinysa-port /dev/ttyACM1 --band air

# no hardware: fake CAT + the bundled stdio MCP test double
python3 tools/cat_rf_test.py --sim

# only the MCP client path, against the test double
python3 tools/cat_rf_test.py --mcp-selftest
```

`--mcp-cmd` overrides the server command (default:
`uv --directory ~/Repos/tinySA_mcp run tinySA_Operator.py`).  The real server
opens its Tk log window, so it needs `DISPLAY` (opencode's MCP config sets it).

Exit status: `0` every case passed, `1` a check failed, `2` setup/import error.

## Pass criteria

* `delta = on - quiet >= --min-delta` (default 10 counts) for every band.
* `recovery = after - quiet <= --max-recovery` (default 8 counts).
* the `READ` frequency matches what was set (within 100 kHz) and `READ`/`SM`/
  `STATUS` replies are well formed (the CAT client raises on a bad ACK or a
  malformed frame).

The S-meter is the lane's **raw** RSSI clamped to 255 (`SM`), not a calibrated
dBm: assert deltas, not absolute levels.  `STATUS` gives the unclamped 9-bit
value for a log.

## Offline result (this repository, no hardware)

```
$ python3 tools/cat_rf_test.py --sim
[vhf] 145.5000 MHz FM: generator -30.0 dBm (low stage)
  carrier off -> S-meter 40
  carrier on  -> S-meter 120
  carrier off -> S-meter 40
  delta +80 (need >= 10.0), recovery +0 (need <= 8.0) -> PASS
[uhf] 446.00625 MHz FM: generator -38.0 dBm (high stage)
  ... same triple -> PASS
2/2 cases passed
```

`--mcp-selftest` also passes: the JSON-RPC handshake, `set_generator` and
`get_generator_state` are exercised against `tools/tinysa_mcp_sim.py`, which
implements the same tool names and validates the stage/level the way the real
server does.

## On-radio result

**To be recorded.**  Fill this table on the first bench run:

| band | level | pad | quiet | on | after | delta | verdict |
|---|---|---|---|---|---|---|---|
| 145.5000 FM | | | | | | | |
| 446.00625 FM | | | | | | | |
| 118.0000 AM | | | | | | | |

## On-radio result

First bench run, 2026-10-23.  Radio on the USB-C CH340 port, tinySA on the
**low output** (-30 dBm, no external pad, `--samples 7`).  The S-meter column is
the CAT `SM` byte (raw RSSI clamped to 255); `STATUS` reported the unclamped
9-bit RSSI (quiet ~101-103, carrier ~188-190 on VHF).

| band | level | pad | quiet | on | after | delta | verdict |
|---|---|---|---|---|---|---|---|
| 145.5000 FM | -30 dBm | 0 | 103 | 190 | 103 | +87 | **PASS** |
| 446.00625 FM | -38 dBm | 0 | — | — | — | — | not run: needs the high output |
| 118.0000 AM | -30 dBm | 0 | 63 | 85 | 63 | +22 | **PASS** |

`READ` returned the exact set frequency and the expected mode byte (0x08 FM,
0x04 AM) every time.  The first VHF run had a lower post-carrier reading (102
quiet, 188 on, 64 off) and still passed; a repeat gave the table above, so the
delta is stable while the absolute level moves with the radio's AGC/noise
floor -- which is why the test asserts deltas.

## Files

* [`../tools/cat_rf_test.py`](../tools/cat_rf_test.py) -- the test.
* [`../tools/tinysa_mcp_sim.py`](../tools/tinysa_mcp_sim.py) -- MCP stdio test double.
* [`ra89r_carserial.md`](ra89r_carserial.md) -- the CAT wire protocol.
* [`ra89r_rf_harness.md`](ra89r_rf_harness.md) -- the console/RSSI harness this
  complements (that one reads the console and drives the tinySA console; this
  one reads/writes CAT and drives the tinySA MCP).
