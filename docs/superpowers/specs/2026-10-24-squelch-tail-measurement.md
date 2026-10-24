# Squelch tail-duration measurement and acceptance (2026-10-24)

**Status: proposal; awaiting review. No implementation is authorized by this
document yet.**

## Why

The hybrid squelch (`driver/squelch-k1`) is now validated on UHF FM, VHF FM,
AM airband, both BK4829/BK4815 lanes and the scan/LISTEN paths.  The one open
item is the **tail**: how long the speaker stays audible after the carrier
disappears.

The microphone test resolved the *state* (quiet/tone/hiss) but not the *time*:
at 48 kHz and 5 ms analysis windows, a CW carrier-off shows one short
high-band burst and then the floor.  The tail is at or below the microphone's
resolution, so it is currently described only by a design bound: the chip's
`0x4E` close delay (3) plus one 10 ms `rx_service` tick.

That is not enough to answer the original goal ("little to no audible tail") or
to decide whether the chip delay or the MCU path needs tuning.  This proposal
defines how to measure it objectively and what would count as acceptable.

## What the tail is made of

Define the reference at the **RF carrier loss** (or, for a tone carrier, the
moment the 1 kHz tone stops).  The audible silence comes after:

1. **t_sense** — the BK4829 detects the loss: `0x4F`/`0x78`/`0x4D` thresholds
   plus the `0x4E` close delay (currently 3).
2. **t_service** — `rx_service()` polls at 10 ms and reads `REG_0C<1>`; worst
   case one tick plus the bit-banged RF-bus read (~0.2–0.6 ms).
3. **t_mute** — `rx_apply_audio_gate()` writes `0x47` (chip AF mute) and drops
   `PC13` (amplifier enable); the AF DAC/amplifier mute/disable has its own
   ramp/click behaviour.
4. **t_acoustic** — the speaker and enclosure decay after the amplifier is
   disabled.

`t_sense` is the chip's debounce and cannot be removed without risking
chatter; `t_service` is bounded by the 10 ms service tick; `t_mute` and
`t_acoustic` are the output stage.  The proposal separates these rather than
reporting one number.

Known/measured so far:

- the chip comparator is clean at `0x4E` 5/3; 0/0 was not clean enough to keep
  (the earlier 0/0 run had extra transitions, although that run's generator
  timing was poor);
- the chip does **not** gate the AF on this board; `rx_apply_audio_gate()` is
  the only mute;
- the old software gate was a 0..10 counter: close at 3 counts = 30 ms plus the
  RSSI fall to the close mark;
- SQL 0/1/5/9, VHF/AM, both lanes and scan/LISTEN all pass the state checks;
- the `0x4E` delay units are **not documented** on this part; the 5/3 value is
  inherited from the stock/K1.

## Methods considered

| method | resolution | verdict |
|---|---|---|
| CAT `STATUS` polling | 150 ms frame gap | rejected — far too coarse |
| current microphone, 48 kHz, 5 ms windows | ~5 ms | insufficient; good for quiet/tone/hiss only |
| high-rate microphone/USB audio, 192 kHz, 1 ms windows | ~0.5–1 ms | viable fallback; includes room noise and speaker acoustics |
| direct audio tap + `PC13` on a scope/logic analyser | ~0.1–1 ms | **preferred with a scope**; measures the electrical gate and the audio envelope |
| firmware trace at 2 ms (`systick_micros`, `REG_0C`/`0x67`) | ~2 ms | **preferred without a scope**; electrical tail only, no acoustic |
| firmware timestamps over UART (`systick_micros`) | tens of µs | good for `t_service` only; cannot see the chip or the speaker |

## Proposed measurement

### Test points

1. **Audio** — AC-coupled, high-impedance probe at the amplifier input or the
   speaker terminals (attenuated; never connect a scope ground to a bridged
   output without checking).  This gives the tone/hiss envelope and the mute
   edge.
2. **`PC13`** — the existing audio-path/amplifier enable (`audio_path_drive`);
   the falling edge is `t_mute`'s start.
3. **A spare MCU GPIO as a firmware marker** — one free pin from
   `docs/ra89r_pins.md` (e.g. `PC0`), toggled in `rx_service()` at the
   `REG_0C<1>` close decision and again after `rx_apply_audio_gate()`.  This
   separates `t_service` from `t_mute` and gives a clean scope trigger even if
   `PC13` moves for other reasons.
4. **Reference** — the audio tone's end (the 1 kHz burst stops when the carrier
   is removed) or a diode detector on the bench coupler.  The tone end is
   sufficient and avoids loading the RF path.

A 2-channel scope (audio + `PC13`/marker) at ≥1 MS/s, 100 ms pre/post trigger,
is enough; a 3-channel scope or a logic analyser plus an audio channel is
better.  If no scope is available, the 192 kHz audio fallback records the
same events acoustically (1 ms windows) but cannot separate `t_service` from
`t_mute`.

### Controls

- **Carrier off** — the gate must close; the tail is measured from the tone
  end to the audio envelope −20 dB.
- **Modulation off with the carrier still on** — the 1 kHz tone stops but the
  carrier remains.  The squelch must **not** close; this separates the RF/AGC
  quieting from the squelch.  If the gate closes here, the thresholds are
  wrong, not the tail.
- **`0x4E` close delay** — 3 (current) vs 2 and 1; 0/0 is excluded by the
  earlier chatter.  The difference gives the delay unit and shows whether the
  chip is the dominant term.
- **Mute path** — `0x47` + `PC13` (current), `0x47` only, `PC13` only.  The
  goal is to know whether the chip AF mute or the amplifier disable produces
  the audible edge/click.
- **Old baseline** — the `develop`/pre-hybrid 0..10 counter build on the same
  bench, so the hybrid is compared with the gate it replaced.

### Firmware-only path (no scope, no case opening)

The console UART plus SysTick are enough for the **electrical** tail.  Add a
blocking `TAIL_TRACE` command in `driver/rx.c` (the same shape as the `!`
probe: the app loop is blocked and the trace owns the audio for its duration):

1. Save the app's gate/AF state, set `0x47` AF FM and `PC13` high, mute the
   BK4815, and leave the BK4829 thresholds as the app has programmed them.
2. Loop every ~2 ms for ~500 ms: read `0x67` and `REG_0C<1>` with
   `systick_micros()` timestamps.
3. Record the first sample where the RSSI falls below the current open mark
   (`t_fall`), the first `REG_0C<1>` 1→0 (`t_close`), and, on that close, set
   `s_squelch_open = false`, call `rx_apply_audio_gate()` and record `t_gate`.
4. Print `t_fall`, `t_close`, `t_gate`, `t_close - t_fall` and
   `t_gate - t_close`, then restore the app state.

The RSSI fall is the RF/AGC-quieting proxy; the `REG_0C<1>` close is the chip's
squelch result; the `rx_apply_audio_gate()` call is the MCU mute.  The 2 ms
poll bounds `t_service`; the chip's `0x4E` delay and the RSSI/AGC are included
in `t_close - t_fall`.  The acoustic decay after `PC13` is not in this trace;
it is the same speaker/amplifier as the stock and is accepted by ear (or by the
existing 48 kHz microphone, which already shows one short burst and no
sustained tail).

What the K1/stock firmware contributes here:

* the K1's `BK4819_SetupSquelch()` programs the same `0x4E` 5/3 delay and the
  same `0x4D`/`0x4F`/`0x78` comparator structure on the same BK4829; its
  `CheckRadioInterrupts()`/`g_SquelchLost` is the same chip result.  The K1
  has no timestamped tail measurement of its own;
* the stock's counter (`FUN_0801D3F0`, close below 3 of 0..10) is three 10 ms
  ticks = **30 ms** plus the RSSI fall — the old baseline this hybrid replaced;
* the stock's AF mute (chip AF registers + PA14/PC13) is the mute path the port
  still uses; the speaker/amplifier are the same hardware.

So the no-scope acceptance can be a **bound**: the chip result must reach
`REG_0C<1>` within ~20 ms of the RSSI fall, the gate within ~2 ms of that, the
total must not exceed the stock's 30 ms + RSSI fall, and the ear/microphone must
show no sustained tail.  Changing `0x4E` close 3→2/1 is the lever if the bound
is missed; 0/0 is excluded by the earlier chatter.

### Matrix

| band / lane | SQL | level | notes |
|---|---|---|---|
| UHF FM 446.00625, BK4829 | 1, 5, 9 | −38 dBm and the weakest level that still opens | strong vs threshold |
| VHF FM 145.500, BK4829 | 5 | −30 dBm | |
| VHF FM 145.500, BK4815 (`TrVfoA=4815`) | 5 | −30 dBm | stock > 134 MHz path |
| AM 118.000, BK4829 | 5 | −30 dBm, `am` | |
| scan LISTEN hit | 5 | −38 dBm | pause/resume tail |
| old baseline | 5 | −38 dBm | comparison |

Each case: carrier off, 10 captures; report median and worst case for
`t_sense`, `t_service`, `t_mute`, `t_acoustic` and total.

## Acceptance criteria (proposed)

- **`t_service` ≤ 10 ms** — one 10 ms service tick plus the bus read; the
  current design should meet this.
- **Electrical total (`t_sense + t_service + t_mute`) ≤ 30 ms** for a strong
  carrier (−38/−30 dBm), and ≤ 40 ms at the weakest level that still opens.
- **No worse than the old 0..10 counter** on the same carrier: the hybrid's
  total must not exceed the old gate's 30 ms + RSSI-fall baseline.
- **No new click/pop** — the audio envelope after the mute must not have a
  larger transient than the old gate.
- **Modulation-off control:** the gate must stay open, i.e. the tail is a
  carrier-loss effect, not an audio-level artifact.
- **No chatter** at SQL 1 on a quiet channel after any `0x4E` change.
- **Firmware trace bound (no scope):** `t_close − t_fall` ≤ 20 ms and
  `t_gate − t_close` ≤ 2 ms; the RSSI fall is the RF/AGC-quieting proxy.
- **Acoustic:** no sustained tail by ear (the microphone already shows one
  short burst and no sustained tail); the speaker/amplifier are the stock
  hardware.

If the electrical total exceeds 30 ms, the first lever is the `0x4E` close
delay (3 → 2 → 1), then a faster service path (a dedicated faster poll) only if
the chip is not the dominant term.  The chip AF mute is not a candidate on this
board; it is already known not to gate the AF.

## Plan

1. **Firmware trace** — add the blocking `TAIL_TRACE` console command in
   `driver/rx.c` (the `!`-probe shape) with `systick_micros`, 2 ms `0x67` /
   `REG_0C<1>` reads, the `t_fall`/`t_close`/`t_gate` timestamps and the
   on-close `rx_apply_audio_gate()`.  No production path; default off.  No
   spare pin or audio tap is needed for this path.
2. **Bench harness** — a host script (or manual console runs) that arms the
   trace and drives the tinySA tone/carrier off; the trace prints the bound.
   The MCP command latency does not matter: the trace timestamps the radio's
   own RSSI/`REG_0C` events.
3. **Measure the matrix** — current hybrid, then `0x4E` 3/2/1, then the old
   `develop` 0..10-counter baseline.  The K1's `0x4E` 5/3 and the stock's
   30 ms counter are the references.
4. **Decide** — keep the current config if the bound is met; otherwise change
   the close delay and re-measure.  Record the result in
   `docs/ra89r_squelch.md`; keep any change on `driver/squelch-k1` until the
   bench accepts it.
5. **Optional scope path** — if a scope becomes available later, the
   audio/`PC13` capture in this proposal gives the exact electrical/acoustic
   split; it is not required to close the item.

## Files touched by the proposal (when approved)

- `firmware/App/driver/rx.c` — the `TAIL_TRACE` command; no production path.
- `firmware/App/main.c` — the console key that calls it (if kept outside
  `rx.c`).
- `docs/ra89r_squelch.md` — the measured table and the final delay choice.
- `docs/superpowers/specs/2026-10-24-squelch-tail-measurement.md` — this file.

The optional scope path would additionally touch `board_pins.h` /
`docs/ra89r_pins.md` for a marker pin and the audio/`PC13` test points, but
neither is needed for the no-scope route.

## Open questions for review

1. Approve the **firmware-only `TAIL_TRACE`** as the primary method (no scope,
   no case opening), with the ear/microphone as the acoustic acceptance?
2. For the old baseline, rebuild `develop`'s 0..10-counter firmware, or is the
   documented 30 ms + RSSI-fall figure sufficient?
3. Is the 2 ms trace resolution enough, or should the trace poll at 1 ms
   (more RF-bus time, same 10 ms app service in production)?
4. Which `0x4E` close-delay variants should be included in the first pass
   (3 and 2, or 3/2/1)?
