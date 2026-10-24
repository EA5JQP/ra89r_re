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
| direct audio tap + `PC13` on a scope/logic analyser | ~0.1–1 ms | **preferred**; measures the electrical gate and the audio envelope |
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

If the electrical total exceeds 30 ms, the first lever is the `0x4E` close
delay (3 → 2 → 1), then a faster service path (the SQL IRQ or a dedicated
5 ms poll) only if the chip is not the dominant term.  The chip AF mute is not
a candidate on this board; it is already known not to gate the AF.

## Plan

1. **Bench preparation** — identify a free GPIO and the audio/`PC13` test
   points; confirm probe grounding and attenuation.
2. **Firmware marker** — add a `TAIL_TEST` compile-time block in
   `driver/rx.c` that toggles the spare pin at the `REG_0C<1>` decision and
   after `rx_apply_audio_gate()`.  No production change; default off.
3. **Capture harness** — a small host script (or manual scope captures) that
   drives the tinySA tone on/off with the MCP and records the audio/`PC13`/
   marker.  The MCP command latency is irrelevant because the scope triggers
   on the tone/marker, not on the host clock.
4. **Measure the matrix** — current hybrid, then the `0x4E` 3/2/1 and mute-path
   variants, then the old baseline.
5. **Decide** — keep the current config if the acceptance is met; otherwise
   change the close delay or the service path and re-measure.  Record the
   result in `docs/ra89r_squelch.md` and, if a code change is made, keep it on
   `driver/squelch-k1` until the same bench accepts it.

## Files touched by the proposal (when approved)

- `firmware/App/driver/rx.c` — `TAIL_TEST` marker only; no production path.
- `firmware/App/board_pins.h` / `docs/ra89r_pins.md` — the chosen free pin.
- `firmware/App/main.c` — an optional bench command for the marker/capture.
- `docs/ra89r_squelch.md` — the measured table and the final delay choice.
- `docs/superpowers/specs/2026-10-24-squelch-tail-measurement.md` — this file.

## Open questions for review

1. Is a scope/logic analyser available on the bench, or should the 192 kHz
   microphone fallback be the primary method?
2. Which free pin is physically accessible on this board for the marker (the
   `docs/ra89r_pins.md` free list vs the QFP pads)?
3. Is the amplifier input or the speaker terminal the preferred audio tap, and
   is the output bridged (grounding caution)?
4. Should the old 0..10-counter build be rebuilt from `develop` for the
   baseline, or is the documented 30 ms + RSSI-fall figure sufficient?
