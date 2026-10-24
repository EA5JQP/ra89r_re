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
This port uses the level result `REG_0C<1>` directly (see below); the SQL
interrupt bits are not armed.

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
  5/3 delays (the setting at the time; the final is 5/0) produced one clean
  transition per carrier edge.
* **The chip does not mute the AF on this board.**  With the SQL status closed,
  the microphone still measured 0.6–14 M in the 1.5–3 kHz band; the software
  gate restored at the end of the run measured 25–35 k.  The tone bursts were
  10^10–10^11 at 1 kHz, so the audio path and tone were fine.
* **The K1's `0x50` word is not the gate.**  Phase 4 used the K1's
  `bk4829.c` `ExitTxMute()` value `0x3B18` instead of the stock/port `0x3B20`
  (bit 5).  The no-carrier hiss stayed (0.78 M pre-tone, 1.10 M post-tone),
  while the software gate again measured 25 k.  `0x50` stays `0x3B20`.
* **Zeroing both delays is not a free speed-up.**  The earlier run with `0x4E`
  open/close delays 0/0 produced extra transitions.  The final setting is
  **5/0**: the open delay (5) and the glitch/Ex-noise filters carry the
  debounce, and close 0 stayed at one transition per carrier edge from −30 to
  −50 dBm in the tail trace (2026-10-24).

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
   (280 MHz), not the K1's 174 MHz calibration split.  The chip's `0x4E` 5/0
   delays (open/close) provide the time hysteresis: the close delay is 0
   because it only added ~10 ms per step of chip tail and 0 stayed at one
   transition from −30 to −50 dBm (see the tail measurement below).

2. **Decision** — in `driver/rx.c`'s BK4829 branch, `REG_0C<1>` sets
   `s_squelch_open` every 10 ms.  The RSSI floor/marks are kept for the scanner
   and S-meter but no longer decide the gate.  SQL 0 forces open.

3. **Audio** — `rx_apply_audio_gate()` still writes `0x47`/`PC13` (and the
   BK4815 `0x49` on its lane) from `s_squelch_open`.  This is the stock's job
   and is required on this board.

4. **One owner** — `rx_service()` publishes `g_SquelchLost`, the green LED and
   the app's display flags on the 10 ms transition, from `REG_0C<1>` on the
   BK4829 lane and from the software gate on the BK4815 lane and during
   scan/LISTEN.  The chip's SQL interrupt bits are **not armed** in `REG_3F`:
   the result is level-based and the AF gate is MCU-side.  `CheckRadioInterrupts()`
   still clears `REG_02` and services CTCSS/CDCSS/DTMF/VOX.

## Validation so far

* UHF FM, 446.00625 MHz, BK4829 audio source, tone-modulated carrier:
  no carrier → the microphone's 1.5–3 kHz band sits at the software-gate floor;
  tone → 1 kHz at 10^10–10^11.  The software gate is driven by the chip's
  `REG_0C<1>` (no RSSI counter involved).
* UHF SQL 1, 5 and 9: no carrier closed/quiet; tone opens (`H`: `0x47=0x6142`,
  `PC13`/speaker on, `squelch open`, function 4).  SQL 0 with no carrier:
  `squelch open`, function 4, audio open — the earlier missing-open-edge is
  fixed by `rx_service()` owning the flag.
* FAST BOTH scan with no carrier: 672 probes/s (the pre-hybrid measurement was
  671/s), so the extra `REG_0C` read is not a scan regression; the live scan
  runs and stops.
* BK4815 lane (VFO B, 446.28125, SQL 5): carrier at -38 dBm raises `0x44` to
  50, `squelch open`, the tone is heard; the software `0x44` gate is unchanged.
* Scan hit: FAST BOTH armed ±500 kHz stops on the 446.00625 carrier
  (`audio source BK4815`, `squelch open`, `0x44` 30).  LISTEN (`SetScn=3`)
  pauses on the carrier with `audio source BK4829`, RSSI 205 and the AF
  unmuted (`0x47=0x6142`).
* Chip status/SQL polarity, the 5/0 delay choice and the tail measured (below).
* `0x50=0x3B20` kept; the K1 `0x3B18` value does not gate the AF here.
* **VHF FM, 145.500 MHz, BK4829, SQL 5:** the tinySA low output at −30 dBm
  raises the RSSI 100→187; `squelch open`; the NFM tone is heard (1 kHz
  10^10–10^11 in the mic).  The VHF table (100 floor, 42/46 ExNoise, 45/55
  glitch) closes on no carrier and opens on the carrier.
* **AM airband, 118.000 MHz, BK4829, SQL 5:** the low output with `am`
  modulation raises the RSSI to 195; `demod 0x31` bit 0 = AM, airband rule on;
  `squelch open`, `0x47=0x6142`, and the AM tone is heard.  `H` shows function
  4 while the carrier is present.
* **BK4815 VHF, 145.500 MHz (TrVfoA=4815):** the low output raises the BK4815
  meter and the audio gate opens; `H` reports `lane 4815`, `squelch open`,
  function 4 and `0x47=0x6142`; the tone is heard.  Set the frequency **after**
  the `9` transceiver switch: the `J` diagnostic's scan benchmark reloads the
  saved channel and reverts an unsaved CAT edit, so this check used `H`.
* Host tests: the `check_all.sh` suite passes in a rootless `podman` `gcc:13`
  container (104 + 16 + 12 + 86 checks, scan/CAT, preview 0 failures); the ARM
  target build passes on the host.  The host itself has no `gcc` installed.

### Tail duration (2026-10-24)

Measured with the blocking console `@` trace (`driver/rx.c`): it pins
145.500 MHz, reprograms the threshold table, opens the audio, then polls `0x67`
and `REG_0C<1>` every ~2 ms with `systick_micros()` and timestamps the RSSI
fall, the chip's close and the `rx_apply_audio_gate()` call.  **Do not drop the
carrier with the tinySA MCP `set_generator(off)`**: it always sends
`mode low output` first, which re-enables the output, so the carrier returns
for ~1.9 s before the final `output off`.  The first trace runs timed that and
reported a bogus 1.88 s tail.  Use a bare `execute_command("output off")`; the
per-200 ms meter timeline added to `@` shows the carrier coming back if this
mistake is repeated.

| condition | `0x4E` close | `t_close − t_fall` | `t_gate − t_close` | transitions |
|---|---|---|---|---|
| −30 dBm, 5 s tone | 3 | 78 ms | 253 µs | 1 |
| −30 dBm, 5 s tone | 0 | 46–48 ms | 253 µs | 1 |
| −50 dBm (RSSI 145 vs mark 136) | 3 | 78 ms | 253 µs | 1 |
| −50 dBm | 0 | 50 ms | 253 µs | 1 |
| SQL 1, −60 dBm (RSSI 127 vs open 112) | 0 | 60 ms | 253 µs | 1 |
| SQL 1, −65 dBm (RSSI 114, 2 counts above open) | 0 | 58 ms | 254 µs | 1 |

`t_fall` is the RSSI crossing the **open** mark (136, ~16 dB above the VHF
quiet floor 103), so the chip's close is driven by the RSSI close threshold,
the Ex-noise and the glitch filters; each close-delay step is ~10 ms.

The **audible** tail is longer than the electrical chip close.  At both delays
the ear hears roughly 300 ms of hiss after the tone stops, dominated by the
audio/AGC settling, and an A/B with 5 s tones found delay 0 vs 3 **not
audibly different**.  That is the acceptance: no sustained tail, and the 28 ms
saved is free.  Close delay **0** was chosen (5/0).

The app path (not the trace) was then checked on the BK4829 at −30 and
−50 dBm with the carrier held and dropped: one transition, no chatter, tail
unchanged.  SQL 1 with close 0 was checked at −60 and −65 dBm (the latter only
2 counts above the SQL 1 open mark): one transition each, so the close delay
did not introduce chatter near the threshold.  The BK4815 lane keeps its
software `0x44` gate.

Still open: the old 0..10-counter build was not reflashed for a side-by-side
audible comparison; its documented 30 ms + RSSI fall is the reference, and the
stock's threshold and the AGC decay are not separable without it.

The `!` bench probe and its fourth phase are development tooling; the shipped
path is the hybrid above.
