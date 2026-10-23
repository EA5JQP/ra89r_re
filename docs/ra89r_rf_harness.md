# RF harness — the radio's UART RSSI against a tinySA generator

`tools/rf_harness.py` turns the firmware's console diagnostics into a measurement:
it drives a tinySA signal generator in step with the radio's `S`/`F`/`B`/`Q`
console commands and reports "carrier off" vs "carrier on" as a number, so a
question like *"is the right front-end pin being used?"* becomes a table instead
of a judgement call.  No firmware change is needed: the console text is parsed
as-is.

Two serial links are involved and they are independent:

| link | port | settings | role |
|---|---|---|---|
| radio console | USB-C programming port, USART1 `PB6`/`PB7` (CH340) | 115200 8N1 | sends console characters, receives RSSI text |
| tinySA | tinySA USB | 9600 | generator level/frequency/output |

## What the console already provides

All of this predates the harness; the harness only times the tinySA against it.

* `S` — `rf_watch()`, `firmware/App/main.c:1191`.  Samples BK4829 reg `0x67`
  every 200 ms for 4 s (20 readings) and prints them plus `min`/`max`/`last`.
  One reading cannot tell a carrier from a noise floor, which is exactly why the
  harness always takes an off/on *pair*.
* `F` — cycles the chip-side receive front end (`pa.c`'s `pa_chip_path_mode`),
  i.e. the VHF/UHF receive LNA pin selection in reg `0x33`.  Prints
  `front end: mode N` and `PA0 <high|low>, reg 0x33 = 0xNNNN`, so the mode is
  **observable** rather than assumed.  This is the "filter pin" experiment: the
  band whose carrier the mode responds to is the band that mode is wired for.
* `B` — cycles the MCU band pin `PA0` (`pa.c`'s `pa_band_pin_mode`).  Prints
  `MCU band pin: mode N` and `PA1 high, PA0 <high|low> (<VHF|UHF>)`.
* `Q` — `rf_squelch_autodetect()`, `firmware/App/main.c:1225`.  Tunes the radio
  itself to 145.5000 then 446.00625 MHz, measures the floor and then the carrier,
  and reports the squelch marks it would program.  It blocks on
  `uart_getc_timeout(15000)` between phases, asking the operator to switch the
  tinySA output — a gate the harness satisfies, which makes the whole sequence
  automatic.

RSSI units: reg `0x67` is 9 bits (`bk4819.c:1484`), so it prints as exactly three
hex digits; `BK4819_GetRSSI_dBm()` is `rssi / 2 - 160` (`bk4819.c:365`).  The
stock's squelch marks are `0xCF` open / `0xB4` close, which is why `0xB4`/`0xCF`
appear in the console text.  Deltas are reported in raw counts and in dB (2
counts = 1 dB).

## Tuning the radio from the harness

`S` samples whatever the radio is tuned to, so the harness tunes it over the
FT-817 CAT interface (`cat_radio.c`, `ENABLE_CAT 1`): a 5-byte frame, BCD of
`Hz/10` then command `0x01`.  `tools/look4sat_cat.py` and `cat.c:90` agree on the
byte order, and the harness reproduces it.

Console characters and CAT frames share the byte stream.  The firmware
(`main.c:88` `cat_input_service`) accumulates up to 5 bytes and pushes them to
the console if they do not form a valid frame, flushing a lone character once
`CAT_BURST_GAP_MS` (20 ms, `main.c:54`) has passed.  Sending a 1-byte console
command immediately before a 5-byte frame is therefore ambiguous, so the harness
inserts a 250 ms gap before a CAT frame — well clear of the 20 ms window — and
drains the frame's `0x00` ACK.

## The generator constraint that shapes the experiment

Measured on a classic tinySA (`tinySA_v1.4-175-g1419a93`), requesting −30 dBm and
reading back what the device actually tuned:

| output stage | 145.5 MHz | 446.00625 MHz |
|---|---|---|
| `mode low output` | 145 500 000 ✅ | **350 000 000** (clamped) |
| `mode high output` | **240 000 000** (clamped) | 446 006 250 ✅ |

So the two bands need **different output stages**, and the stages have different
level ranges (`low` −76..−6 dBm, `high` −38..+13 dBm).  Consequences:

* the injected level cannot be held constant across a VHF/UHF A-B comparison, so
  a level difference must not be read as a filter-path difference — read the
  *deltas*, and normalise with an attenuator if the comparison has to be tight;
* `prepare()` re-reads the tuned frequency and the harness reports a clamp
  loudly, because a clamped carrier at the wrong frequency looks exactly like a
  filter that does not pass.

## RF safety: generator-only is not the same as TX-proof

The harness only ever *transmits*: it sets a CW level and switches the output on
and off, and never takes an input measurement.  It also never sends PTT -- the
firmware has CAT PTT (`cat_radio.c:116`) but the harness only sends the
set-frequency frame.  That is still not the same as being safe from the radio's
own transmitter: the tinySA Basic's SMA is both the generator output and the
analyser input, and its input is rated (tinySA wiki, "Specification")

* **absolute maximum input +10 dBm** with 0 dB internal attenuation,
* +20 dBm only as a short-term peak *with* 30 dB of internal attenuation,
* **below −25 dBm** for a clean measurement.

A 5 W handheld is **+37 dBm** — 27 dB past the destructive rating — and it is
27 dB past it whether the device is in output mode (reverse power into the
generator's output chain) or input mode (into the input attenuator).  Note too
that the harness *parks the device in `mode low input` when it exits*, i.e. as an
analyser with its input on the SMA, so the pad has to stay in place between runs,
not just during them.

So the protection is an external pad, not the script:

| radio TX | pad | at the tinySA SMA | |
|---|---|---|---|
| 5 W (+37 dBm) | 0 dB | +37 dBm | destroys it |
| 5 W (+37 dBm) | 30 dB | +7 dBm | survivable; still above the −25 dBm "clean" level |
| 5 W (+37 dBm) | 40 dB | −3 dBm | comfortable; a 10 W-rated 40 dB pad is cheap |

The pad costs the experiment nothing: the quietest generator setting is −38 dBm,
so even 40 dB of it leaves **−78 dBm at the radio's antenna** — about 50 dB above
the radio's sensitivity, and far above the level the front-end modes are being
compared at.  Pass the pad with `--pad-db` and every level is reported at both
ends (`-30 dBm at the SMA = -70 dBm at the radio`); `--radio-tx-dbm` (default
5 W) fixes the arithmetic for a different radio.

Two software courtesies, neither of which replaces the pad:

* before the generator port is even opened, the harness polls `T` — that is
  `tx_diag()` (`main.c:1068`), a read-only TX report that does **not** key the
  radio — and aborts if the radio says it is transmitting;
* it never sends CAT PTT, and of the bench TX commands it uses only the
  read-only `T`: `C` (PC13), `Y` (PA duty), `x` (TX path pin) and `w` (TX
  variant) are never sent.

A press on the radio's own PTT is invisible to the script, which is why the pad
matters more than the check.  Wanting the tinySA to *measure* the radio's
transmit rather than inject into its receive is a different experiment (bigger
pad, input mode, overload warning) and not this tool.

## Running it

```sh
# what is on the bench (the CH340 is '1a86:7523', the tinySA '0483:5740')
python3 tools/rf_harness.py ports

# one band: carrier off vs on, radio tuned by CAT (40 dB pad in the RF path)
python3 tools/rf_harness.py --radio-port /dev/ttyUSB0 --pad-db 40 sample --band vhf

# every front-end mode x band-pin mode, both bands, dumped to CSV
python3 tools/rf_harness.py --radio-port /dev/ttyUSB0 --pad-db 40 matrix --csv /tmp/rf.csv

# drive the firmware's own 'Q' squelch autodetect end to end
python3 tools/rf_harness.py --radio-port /dev/ttyUSB0 --pad-db 40 squelch
```

`--no-tune` leaves the radio on its current tuning (the `S` header still reports
what it is tuned to).  `--dry-run` never touches the generator at all, which
exercises the whole radio-side path without RF.  Defaults are the quietest
sensible level per band (`vhf` −30 dBm, `uhf` −38 dBm); levels outside a stage's
range are refused rather than clamped.  On exit the generator output is switched
off and the device is parked back in `mode low input`.

### Without the radio

`tools/ra89r_rf_console_sim.py` is a console test double (the same idea as
`ra89r_bootloader_sim.py`), so the harness can be run before the CH340 is
plugged in:

```sh
socat -d -d pty,raw,echo=0,link=/tmp/ra89r0 \
              pty,raw,echo=0,link=/tmp/ra89r1 &
python3 tools/ra89r_rf_console_sim.py /tmp/ra89r1 &
python3 tools/rf_harness.py --radio-port /tmp/ra89r0 --dry-run sample --band uhf
python3 tools/rf_harness.py --radio-port /tmp/ra89r0 --dry-run matrix \
        --bands vhf --f-modes 5 --b-modes 3
python3 tools/rf_harness.py --radio-port /tmp/ra89r0 --dry-run squelch
```

Its RSSI numbers are invented and its timings are accelerated (50 ms per sample);
it exists to prove the parsing and the sequencing, not the radio.  Restart it
with `--tx` to make `T` report "transmitting" and watch the safety check abort.

## Status

Verified on hardware (classic tinySA):

* `TinySA.prepare()` picks `low output` for 145.5 MHz and `high output` for
  446.00625 MHz, sets the level, reads the tuned frequency back, and reports no
  clamp; a −70 dBm request against the high stage (min −38) is refused;
  `restore_input()` leaves `sweep` reporting `0 350000000 290`, i.e. spectrum
  mode.

Verified against a test double (`tools/ra89r_rf_console_sim.py`, a fake console
that emits the exact `printf` strings from `main.c`, over a socat PTY pair):

* `S` parsing (samples, `min`/`max`/`last`, the header's RX frequency/band),
  `F`/`B` state parsing, CAT tuning (a UHF frame moves the header to UHF), and
  the full `Q` gate sequence including both the `detected:` and the
  `floor and carrier overlap` branches;
* the RF-safety pre-flight: `T` parses to `{'transmitting': False}`, a sim run
  with `--tx` aborts before the generator is opened, and the pad arithmetic
  reads "safe" at 40 dB, "STILL DESTRUCTIVE" at 20 dB and warns at 0.

One real run was done with the harness driving the **real tinySA** and the sim as
the radio (`--pad-db 40`): the banner, the `T` idle check, `prepare()` at
−30 dBm and the park back to `mode low input` all behaved.

Not verified / open:

* the harness has **never run against a real RA89R** — the radio's CH340 port was
  not attached when it was written, so every radio-side claim above rests on the
  test double, not the radio;
* whether `F`'s modes actually differ in received carrier on this board, or
  whether `B` (`PA0`) matters at all — that is the experiment, not a known result;
* the absolute levels a real front end tolerates before compression, and the
  attenuation actually fitted on the bench.
