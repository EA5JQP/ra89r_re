# RA89R Bluetooth audio and PTT parity

**Status:** Proposed design for the remaining parity work; awaiting user review.
The current branch contains preliminary PC13/`0x33` and SCO-recovery changes,
but they are not a complete or radio-validated implementation of this design.

## Goal

Make the port's user-visible Bluetooth behavior match the radio's required
behavior across RX routing, PTT key source, and menu feedback, without damaging
the measured RF/TX chain. Keep the earpiece microphone path as an explicit
investigation item until its source selection is demonstrated.

## Agreed behavior

1. **RX output selection:** while a Bluetooth earpiece is linked, received radio
   audio is heard through BT and not the radio's local speaker. When BT is not
   linked, receive audio is heard through the radio. The current simultaneous
   output is not acceptable. The stock code drives PC13 from the Speak Switch
   setting; the documented current codeplug has that switch off, which should
   yield BT-only output while linked. Confirm the actual byte on the user's
   radio during validation.
2. **PTT key source:** the BT PTT mode selects which key source may key the
   transmitter:
   - `BT`: headset PTT events only;
   - `Radio`: radio PTT keys only;
   - `Both`: either source.
3. **Menu key feedback:** ordinary key presses beep in both the normal menu and
   BT menu, subject to the stock Beep setting. The beep must follow the selected
   audio destination while BT is linked.
4. **SCO lifecycle:** headset link state and SCO/call state are distinct. An SCO
   teardown is not a headset disconnect; BT RX audio must recover when returning
   from PTT to RX while the headset remains linked.
5. **TX mic:** the headset mic must reach the transmitted signal in BT use, while
   the stock's observed radio-mic mute behavior is preserved. This is separate
   from the PTT key-source mode above and remains unresolved until the stock
   route is identified.

## Stock evidence and current gap

- The stock's `FUN_080177A8` drives PC13 from BT link state and the codeplug
  Speak Switch. `FUN_08015D44` drives BK4829 GPIO pin 2 (`0x33` bit `0x0010`)
  while BT is enabled and linked; the stock helper also clears paired bit
  `0x1000`.
- The port now mirrors those two state changes with stock-style masked writes.
  On-radio tests so far show PC13 level changes do not select the mic, and the
  simultaneous RX output remains. Neither control is yet proven to implement
  exclusive RX routing.
- The stock parser clears its call-active flag on `+IM_SCO_DISCN` and
  `+IM_CALL_DISCONED`; it does not send `AT+BT_CALL=OFF` there. The stock can
  reopen CALL on the receive/T-R transition. Commit `8346274` mirrors this
  lifecycle and is still awaiting radio validation.
- The port's `AUDIO_PlayBeep()` currently returns while the K1 function state is
  RECEIVE or MONITOR. `BT_ProcessKeys()` also does not request a key beep. This
  explains why menus are silent in the port even though the stock key dispatcher
  plays its DAC beep.
- `tx_poll_ptt()` currently consumes the BT PTT flag, but it must apply the
  agreed BT/Radio/Both source policy. The raw YBT100 event emitted by the
  user's headset PTT has not yet been captured for the failing case.
- The stock's normal RF mic gain path does not read `config+0x3a`; a separate
  binary-control override exists but has not been tied to the BT mic. Do not
  infer a mic route from `MICGAIN`, PC13, or a BK4829 register name alone.

## Design

### BT state and call lifecycle

Keep separate state for physical BT link, SCO/call active, radio TX active, and
headset PTT input. On SCO/call disconnect, retain the BT link and defer a single
CALL restart until the UART event batch is drained and the radio is back in RX.
On a physical BT disconnect or BT disable, clear the link and audio-route state.
Never send `BT_CALL=OFF` merely because SCO ended during PTT.

### PTT source policy

Centralize the key-source decision in the TX polling boundary:

```text
BT     -> headset PTT state
Radio  -> local PTT / PTT2 state
Both   -> headset PTT OR local PTT / PTT2
```

The selected key source controls whether TX is keyed; it does not implicitly
change microphone gain or source. Clear any latched headset PTT state on a real
BT disconnect. Confirm the headset's actual event strings from a UART `U` capture
before altering the parser.

### RX routing and PC13 / BK4829 outputs

Use one route decision derived from the agreed invariant (BT link selects BT RX;
no link selects local RX). Keep PC13 and BK4829 `0x33` writes in their respective
drivers, and preserve the stock paired-bit semantics for pin 2. Do not perform a
whole-register `0x33` replacement that discards unrelated bits. The physical
effect of PC13 and pin 2 must be verified against local-speaker and BT-headset
audio separately; if those stock pins do not enforce the invariant on this
board, stop and trace the actual analog route rather than adding another guessed
register write.

### Menu beeps

Add a key-feedback path shared by normal and BT menu handling. It should not be
suppressed solely because `gCurrentFunction` remains RECEIVE/MONITOR while a menu
is open, but it must respect the stock Beep setting. Route the beep through the
same selected destination as RX while BT is linked; use the existing PA4/DAC
beeper rather than a new RF-chip tone sequence.

### TX microphone route

Keep this separate from PTT key-source selection. First capture the YBT100 PTT
events and compare the stock BT-linked TX setup with the port's BK4829 mic path.
The radio's own mic is reported silent under stock BT operation, while the
earpiece mic is reported low but possibly present on the port. Do not change the
mic ADC enable or set its gain to zero unless an experiment establishes that
this leaves the earpiece signal intact.

## Verification

### Host checks

- Test the BT/Radio/Both PTT truth table, link/SCO teardown ordering, and one
  CALL restart after paired disconnect events.
- Test PC13 state transitions and BK4829 `0x33` pin-2 paired-bit behavior without
  replacing unrelated register bits.
- Test that a menu key press reaches the beep driver in both normal and BT menu
  paths when Beep is enabled, and is suppressed when it is disabled.
- Run `firmware/tools/check_all.sh` and verify the generated `.icf`.

### Radio validation before merge

On the radio, verify separately:

1. BT linked: RX audio is heard only in the BT earpiece; disconnected: only the
   radio speaker plays RX audio.
2. BT, Radio, and Both PTT modes key only their specified sources.
3. After a headset PTT cycle, SCO RX audio and menu beeps recover without
   re-pairing.
4. A second receiver confirms which microphone is transmitted in each mode and
   that BT gain changes affect only the intended source.

Unvalidated work remains on `driver/bluetooth`; do not merge to `develop` until
these radio checks pass.

## Explicit non-goals

- Do not merge the BT driver or route experiments to `develop` before radio
  validation.
- Do not implement SPP/BLE-GATT data transfer; it is outside this audio/PTT
  behavior.
- Do not change RF output/PA tuning or add a register write based solely on the
  symptom that the BT mic is quiet.
