# CTCSS / DCS (tone squelch)

State: branch `driver/ctcss`, **not validated on the radio**.

## The fault (2026-11-01)

A channel with an RX tone heard every carrier on its frequency, toned or not.

The K1's tone logic was intact:

- `RADIO_SetupRegisters()` (`radio.c`) programs the BK4829 decoder (`0x51`,
  `0x07`, `BK4819_SetCTCSSFrequency` / `SetCDCSSCodeWord`) and arms the
  CxCSS found/lost/tail interrupts in `0x3F`.
- `CheckRadioInterrupts()` (`app/app.c`) turns `REG_02` into `g_CTCSS_Lost` /
  `g_CDCSS_Lost`. The K1's naming is inverted: `g_CTCSS_Lost` set = tone present.
- `HandleIncoming()` stays in `FUNCTION_INCOMING` until the code matches, and only
  then calls `APP_StartListening()` → `FUNCTION_RECEIVE`. `HandleReceive()` ends
  the reception about 1 s after the tone goes away.

The port bypassed that decision. `rx_apply_audio_gate()` (`driver/rx.c`) drives the
BK4829 AF (`0x47`) and the speaker amplifier from the port's **carrier** squelch
(`s_squelch_open`) on every 10 ms poll. So any carrier above squelch was heard,
even while the K1 was still in INCOMING.

## The fix

On the BK4829 lane the gate is `rx_tone_gate_open()` (`driver/rx.h`, a pure
function tested in `tools/test_rf_dual.c`): open = carrier open **and** (no tone
set, i.e. `gCurrentCodeType == CODE_TYPE_OFF`, **or** the K1 is in `RECEIVE` /
`MONITOR`). `gCurrentCodeType` is the variable `HandleIncoming()` itself checks;
`FUNCTION_Init()` sets it to OFF on a non-FM channel, so AM is unaffected.

`g_SquelchLost`, the green LED and the status line still follow the carrier, as
the K1 expects: it needs a carrier before it looks for a tone.

## Open

- **BK4815 lane: no tone squelch.** The BK4829 doesn't hear the carrier when the
  VFO is on the BK4815 (`0x67` 53-62 with a carrier on or off), so its decoder
  never reports a tone. Gating there would make every toned BK4815 channel silent,
  so that lane still opens on the carrier alone. The BK4815 has its own CTCSS
  decoder (`0x59`, `0x5B`-`0x5D`, interrupt mask `0x73`; `ra89r_bk4815.md`) that
  is not used yet.
- **TX tone: located, not verified.** PTT → `FUNCTION_Transmit()` →
  `RADIO_SetTxParameters()` writes `0x51`/`0x07`; `tx_start()` afterwards writes
  only `0x30`/`0x36`/`0x37`/`0x40`/`0x50`/`0x52`. The stock's TX tone level is
  lower (`0x51` = `0x9032` vs the K1's `0x9040`, `ra89r_rffeatures.md`).
- **Bench gate** (needed before merging): a tinySA FM carrier on a BK4829 channel
  with RX CTCSS set:
  1. no tone → muted (the screen may show the signal);
  2. the matching tone → audio;
  3. a wrong tone → muted;
  4. the tone removed mid-reception → audio closes within about 1 s;
  5. monitor opens regardless of the tone.
  Repeat with a DCS code.
