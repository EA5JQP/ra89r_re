# The receive squelch

**Status: hybrid implemented on `driver/squelch-k1` and validated on the radio
for UHF FM (446.00625 MHz) with a tone carrier.**  The BK4829 decides; the MCU
still performs the AF mute.  The other bands, the BK4815 lane, SQL 0/1/9 and the
scan/LISTEN paths are not re-measured yet.

The port previously used a software RSSI floor + 0..10 counter (`rx.h`,
`rx_floor_step`/`rx_squelch_step`).  That worked, but it was a substitute for
the stock's real thresholds.  This feature replaces the BK4829 lane's decision
with the chip's own squelch comparator and leaves the audio mute in software.

## The three shapes

| | stock RA89R | K1 | this port (hybrid) |
|---|---|---|---|
| decision | MCU: `FUN_080052B8` + 0..10 counter | BK4829 comparator + SQL IRQ | BK4829 comparator, read as `REG_0C<1>` |
| metrics | `0x63`/`0x65`/`0x67` | chip-internal | chip-internal |
| thresholds | `0x3880` block + per-VFO RSSI mark | K1 EEPROM table at `0x010000` | local table (below) |
| AF mute | MCU (chip AF registers + `PA14` Rx.Light) | chip (K1's board) | MCU (`rx_apply_audio_gate`) |
| second chip | `FUN_08005218` on `0x43`/`0x44` | none | existing software gate |

The K1's calibration table is **blank (`0xFF`) on this radio**, so the K1 import
programmed `255/127/255` and its SQL interrupt never fired.  The stock's
`0x3880` block is the local factory data; the values used here are the measured
ones from `ra89r_calibration.md` plus the bench measurements below.

## What the BK4829 squelch registers actually do

The BK4819 application note §13 gives the register semantics:

* `0x4F<6:0>` = Ex-noise threshold for squelch open, `0x4F<14:8>` = close;
* `0x78<15:8>` = RSSI open, `0x78<7:0>` = RSSI close (0.5 dB/step);
* `0x4D<7:0>` = glitch close, `0x4E<7:0>` = glitch open;
* `0x4E<13:11>` = open delay, `0x4E<10:9>` = close delay;
* `REG_0C<1>` = squelch result, **1 = link/open, 0 = loss/closed**;
* `REG_02<2>`/`<3>` = squelch lost/found interrupts.

The K1's `CheckRadioInterrupts()` treats `sqlLost` as `g_SquelchLost = true`
(open) and `sqlFound` as false (closed) — the same polarity as `REG_0C<1>`.

## The experiment (`!` console command)

`App/main.c`'s `squelch_k1_probe()` deliberately blocks the app loop (so
`rx_poll()` cannot touch the audio), programs the BK4829 thresholds, leaves the
AF in FM and the amplifier on, and prints the SQL status and meters.  The bench
carrier is the tinySA through the MCP, NFM-modulated with its internal tone.

The microphone is the detector: a 1 kHz Goertzel for the tone and the 1.5–3 kHz
band for the demodulated hiss.  The same recording contains a known quiet
reference (the software gate restored at the end) and a known tone.

### Results

* **The chip's comparator works.**  With the carrier present `REG_02` reports
  `0x0004` (`sqlLost`/open); with it removed `0x0008` (`sqlFound`/closed).  The
  5/3 delays produced one clean transition per carrier edge.
* **The chip does not mute the AF on this board.**  With the SQL status closed,
  the microphone still measured 0.6–14 M in the 1.5–3 kHz band; the software
  gate restored at the end of the run measured 25–35 k.  The tone bursts were
  10^10–10^11 at 1 kHz, so the audio path and tone were fine.
* **The K1's `0x50` word is not the gate.**  Phase 4 used the K1's
  `bk4829.c` `ExitTxMute()` value `0x3B18` instead of the stock/port `0x3B20`
  (bit 5).  The no-carrier hiss stayed (0.78 M pre-tone, 1.10 M post-tone),
  while the software gate again measured 25 k.  `0x50` stays `0x3B20`.
* **0/0 delays are not a free speed-up.**  The earlier run with `0x4E` open/
  close delays 0/0 produced extra transitions; 5/3 is the debounced setting.
  (The 0/0 run was not clean enough to call it chatter by itself, but there is
  no reason to remove the debounce.)

The same chip is used on the K1, so this is **not** a BK4829 limitation: the
RA89R's board takes the AF before the chip's squelch gate (which is also why
the stock runs a software squelch), while the K1's board uses the gated output.
Firmware cannot change that routing.  The portable half is the **decision**.

## The hybrid

1. **Thresholds** — `RADIO_ConfigureSquelchAndOutputPower()` in `radio.c` no
   longer reads the blank K1 EEPROM.  For SQL level 1..9:

   | field | value |
   |---|---|
   | RSSI open | `floor + 6 + 6*level`; floor 55 UHF, 100 VHF, 63 AM |
   | RSSI close | open − 12 |
   | ExNoise open | `42 − 3*(level−1)` |
   | ExNoise close | 46 |
   | glitch open / close | 45 / 55 |

   SQL 0 keeps the K1's all-open values.  The split is this board's RF path
   (280 MHz), not the K1's 174 MHz calibration split.  The chip's `0x4E` 5/3
   delays provide the time hysteresis.

2. **Decision** — in `driver/rx.c`'s BK4829 branch, `REG_0C<1>` sets
   `s_squelch_open` every 10 ms.  The RSSI floor/marks are kept for the scanner
   and S-meter but no longer decide the gate.  SQL 0 forces open.

3. **Audio** — `rx_apply_audio_gate()` still writes `0x47`/`PC13` (and the
   BK4815 `0x49` on its lane) from `s_squelch_open`.  This is the stock's job
   and is required on this board.

4. **One owner** — `rx_squelch_chip_owner()` is true only when the BK4829 is
   the audio lane and no scan is retuning it.  `CheckRadioInterrupts()` uses
   the K1 SQL path (`g_SquelchLost`, green LED) only then; on the BK4815 lane
   and during scan/LISTEN `rx_service()` keeps publishing the software gate.
   The BK4815 lane and scan/LISTEN still use the existing `0x44`/RSSI software
   gate.

## Validation so far

* UHF FM, 446.00625 MHz, SQL 5, BK4829 audio source, tone-modulated carrier:
  no carrier → the microphone's 1.5–3 kHz band sits at the software-gate floor;
  tone → 1 kHz at 10^10–10^11.  The software gate is driven by the chip's
  `REG_0C<1>` (no RSSI counter involved).
* Chip status/SQL polarity and the 5/3 delay behaviour measured.
* `0x50=0x3B20` kept; the K1 `0x3B18` value does not gate the AF here.

Open: VHF FM / AM airband, SQL 1 and 9, the BK4815 lane, scan/LISTEN and the
LISTEN hand-back, the audible tail against the old 0..10 counter, and the
`g_SquelchLost`/LED/INCOMING path on the K1 app.  The threshold table's VHF/AM
ExNoise/glitch values are assumed from the UHF measurement until measured.

The `!` bench probe and its fourth phase are development tooling; the shipped
path is the hybrid above.
