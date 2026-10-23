# Squelch: from the ad-hoc floor+margin to the stock's shape (2026-10-23)

## Why

The receive gate (`driver/rx.c` / `rx.h`) was fitted to a handful of tinySA
measurements: per-(lane, band, modulation) floor, `floor + margin(level)` open,
`open - margin/2` close, **one RSSI sample decides the state**.  On the radio it
is jumpy:

* at SQL 1 the BK4815 lane's margin is 4 counts and its hysteresis 2, so a
  noise sample at the mark flips it (measured: a false open at RSSI 12 on a
  quiet 446.2 MHz channel);
* the BK4829 lane's margin at SQL 1 is 12 counts against a noise band that
  spans 12 counts (49..61 at 446), so its open mark sits at the top of the
  noise; one sample is enough to open/close;
* the floor is per (lane, band, modulation), not per frequency: a retune inside
  a band reuses the previous channel's floor;
* the K1's own squelch path is dead on this radio: `RADIO_ConfigureSquelchAnd-
  OutputPower()` reads its six thresholds from the K1 EEPROM layout at
  `0x10000`/`0x10060`, which is **all 0xFF** in `stock_eeprom.bin`, so
  `BK4819_SetupSquelch()` programs 255/127/255 and the SQL IRQs never fire.
  The IRQ handler still writes `g_SquelchLost` (a latent second owner);
* the stock's squelch is not a comparator: `FUN_080052B8` compares `0x63`,
  `0x65` and `0x67` against per-channel marks adjusted by the squelch level,
  ramps the AGC step `0x13` (`0x3b0 | (8 - counter)`) and feeds a **0..10
  counter** (`FUN_0801D3F0`, open at >= 7, close below 3).  Its marks come from
  the external-flash calibration page `0x3710` (`Freq + W/N Sql1/Sql9`).

## Goal

Stop the chatter first, then take the thresholds from the stock calibration
instead of bench-fitted margins.

## Phase 1 -- the stock's counter (this branch)

* Add a pure, header-only squelch state machine (`rx.h`): count 0..10,
  increment on "present" (`rssi >= open_mark`), decrement on "absent"
  (`rssi < close_mark`), hold between the marks, open at >= 7 / close below 3
  (the stock's own numbers), force-open for SQL 0 / the BK4815 AF override.
* `rx_poll()` uses it for both lanes; `s_closed_polls` stays as the derived
  "closed for N polls" for `rx_squelch_quiet()` (the scan trigger).
* Raise the BK4815 margin base from 2 to 4 (SQL 1 = 6 counts) so the level
  hysteresis also clears its noise; the BK4829 margin stays as measured.
* Host test `firmware/tools/test_squelch.c`: noise around the mark must not
  chatter, a carrier opens on the 7th poll, removal closes on the 8th, the
  dead band holds, force-open works.  Added to `check_all.sh`.
* Keep the retune hold-off (`driver/rx-retune-squelch`, separate branch) as an
  orthogonal change; do not stack the two in one merge.

**Acceptance (radio):** SQL 1 and 5, quiet channel, 10 min -> no gate
transitions; carrier at the threshold in 1 dB steps -> one clean transition;
fading carrier -> one open/close, no chatter; FAST BOTH / LISTEN still trigger.

## Phase 2 -- the stock calibration marks

* Decode calibration page `0x3710` (336 B, programmed, values 22..37) and
  `0x3860` (16 B).  Follow the stock's reader `FUN_08010550` and the tables at
  `0x080248E2`/`0x080248ED`, and the CPS grid (`AdjWin.cs`, field order
  `Freq + W/N Sql1/Sql9`), to settle rows/columns.
* Pure `squelch_cal` module: for (frequency, bandwidth W/N, SQL level 1..9)
  interpolate the surrounding rows and the Sql1..Sql9 span, returning the
  open/close marks.  Host-test against the dump.
* Use those marks for the BK4829 lane; keep the floor+margin only when the page
  is blank (`0xFF`) or the frequency is outside the calibrated rows.
* Match the stock's AGC state before using its marks: the stock ramps `0x13`
  with the squelch level; the port leaves it at `BK4819_InitAGC`'s `0x3DF`.
  Either reproduce the ramp for the level or characterise the offset.

**Acceptance:** calibrated marks land a few counts above the measured floor at
145.5 / 446.00625 / 118 MHz; the counter keeps the quiet-channel result of
Phase 1 with the new marks.

## Phase 3 -- one owner

* Keep the software gate as the owner (Phase 1+2), and neutralise the K1's
  blank-table path (or feed it the same marks) so `g_SquelchLost` has a single
  writer; or
* A/B the K1 path: program the BK4829's hardware squelch (`0x4F`/`0x78`/
  `0x4D`/`0x4E`) from the decoded marks and let the SQL IRQs own
  `g_SquelchLost`; keep the software counter for the BK4815 lane, which the K1
  API does not cover.

Pick after Phase 2 measures both.

## Phase 4 -- validation

* Quiet channel, SQL 1/5, 10 min: transitions counted over CAT `STATUS`.
* Carrier threshold sweep, fading carrier.
* Both lanes and both bands; FAST BOTH and LISTEN.
* Record the outcome in `docs/ra89r_port.md` and update the state table.

## Files

* `firmware/App/driver/rx.h` -- state machine, constants, margins.
* `firmware/App/driver/rx.c` -- `rx_poll()` uses it.
* `firmware/tools/test_squelch.c` -- host test; `check_all.sh`.
* Phase 2: `firmware/App/driver/squelch_cal.{c,h}`, `tools/ra89r_calib.py`
  (dump/decode), `docs/ra89r_calibration.md`.
