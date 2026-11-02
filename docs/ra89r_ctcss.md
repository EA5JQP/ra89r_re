# CTCSS / DCS (tone squelch)

State: merged 2026-11-02, **validated on the radio** (BK4829 lane): a matching
CTCSS tone from a second radio opens the audio; no tone or a wrong tone stays
muted; dropping the tone closes the audio within about 1 s; monitor opens
regardless of the tone; the same checks pass with a DCS code.

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

- **BK4815 lane DCS: on branch `driver/dcs-4815`, not validated on the radio**
  (see "DCS on the BK4815 lane" below). CTCSS on that lane is done and validated.
- **TX tone: located, not verified.** PTT → `FUNCTION_Transmit()` →
  `RADIO_SetTxParameters()` writes `0x51`/`0x07`; `tx_start()` afterwards writes
  only `0x30`/`0x36`/`0x37`/`0x40`/`0x50`/`0x52`. The stock's TX tone level is
  lower (`0x51` = `0x9032` vs the K1's `0x9040`, `ra89r_rffeatures.md`).
- **Bench gate** (passed 2026-11-02; repeat after touching the gate): a carrier
  on a BK4829 channel with RX CTCSS set:
  1. no tone → muted (the screen may show the signal);
  2. the matching tone → audio;
  3. a wrong tone → muted;
  4. the tone removed mid-reception → audio closes within about 1 s;
  5. monitor opens regardless of the tone.
  Repeat with a DCS code.

## The BK4815 lane (merged 2026-11-02, validated on the radio)

The BK4829 can't decode a tone on a BK4815 lane, because it doesn't hear the carrier
there. So the BK4815's own detector is used, and its result is handed to the K1
through the same flag, so `HandleIncoming()`/`HandleReceive()` run unchanged.

**What the stock does (located).** `FUN_08006D10` sets up the BK4815's
receive sub-audio for a channel (datasheet register numbers are decimal):

| case | `0x5B` [91] symbol | `0x5C` [92] | `0x42` [66] | other |
|---|---|---|---|---|
| RX CTCSS | `FUN_08016F28(tone, 0)`: `round((tone_0.1Hz << 19) / 18466 / 10)`, address 0 | `0x4001` (HPF bypass, 1 symbol) | `0xD103` (bit 8 CTCSS rx) | |
| RX DCS | – | `0x4000` | `0xD083` (bit 7 CDCSS rx) | `0x59 = 0xF7A1`, `0x61 = 0x0100`, the code |
| none | `0x061A` (55 Hz) | `0x4000` | `0xD103` | |

`0x5D` [93] = `0x0060` in every case, and the receive entry's `0x73` = `0x81D0`
enables the CTCSS found (bit 8) / lost (bit 7) interrupts. **The stock never
reads the result over SPI.** Its only BK4815 reads are `0x00`, `0x0B`, `0x28`,
`0x43`, `0x44`, `0x73` and `0x75`. So it most likely takes the chip's INTN pin on
a GPIO (not located).

**What the port does (validated on the radio 2026-11-02).**
- `bk4815_set_rx_ctcss()` replays the CTCSS/none rows above.
- `rx_4815_ctcss_service()` (`driver/rx.c`, every 10 ms service pass):
  1. Programs the detector whenever the selected VFO's RX tone changes.
  2. Polls the datasheet's interrupt flags `0x74` [116] over SPI (bit 8 found,
     bit 7 lost) and clears them by writing 1. If both are set, `0x5C` bit 13
     decides (`rx_4815_tone_step()`).
  3. Publishes the result as `g_CTCSS_Lost`, which is the K1's name for "tone
     present".
- `CheckRadioInterrupts()` ignores the BK4829's CTCSS bits on this lane.
- The audio gate on the BK4815 lane uses `rx_tone_gate_open()` for CTCSS only.

**Settled on the radio:** `0x74` latches over SPI without INTN being serviced. Console `$` prints the detector registers, the published
state and 5 s of raw `0x5C`/`0x74`/RSSI samples.

**Bench gate (passed 2026-11-02):** VFO on the BK4815 (UHF, e.g. 446.00625), RX CTCSS set, toned
carrier from a second radio. Run the same five checks as the BK4829 lane, plus a
tone change in the menu while on the channel. Then confirm the BK4829 lane is
unchanged.

## Adjacent tones on the BK4829 (2026-11-02)

**Radio finding:** with RX 69.3 Hz on a BK4829 VFO, a second radio sending
67.0 Hz opened the squelch. 71.9 Hz stayed muted. None of these narrowed it
(console `}` writes, radio checks):

| write | 67.0 into a 69.3 channel |
|---|---|
| `REG_52 = 0x0148` (found/lost thresholds 5/8 instead of 10/15) | opens |
| `REG_52 = 0x128F` (bit 12, the ~0.1 % unit) | opens |
| `REG_51 = 0x9140` (bit 8, automatic CTCSS bandwidth off) | opens |

None of this was a porting error. The stock programs the same tone word
(`FUN_08016F7C`: `round(tone_0.1Hz x 20.64888 / 10)`, the K1's
`(tone x 206488 + 50000) / 100000`). It also programs the same thresholds:
`FUN_08006DE4` writes `REG_52` from the factory block at external flash
`0x3880` (`(b[0x0A] << 6) | b[0x0B]` = `0x028F` on this radio, in all three dumps).
The only difference is `REG_51`'s TX tone level, `0x9032` vs `0x9040`.

**The chip's measurement is accurate.** `REG_68` read `0x0573` = 67.5 Hz
during the 67.0 transmission, and bit 15 was set (no tone) with no signal.

**The fix (merged 2026-11-02, validated on the radio):** the chip's tone report is
kept as a raw input (`rx_4829_ctcss_event()`). The K1 sees `g_CTCSS_Lost`
only when that report holds **and** the tone measured in `REG_68` is nearest to
the channel's tone in `CTCSS_Options` (`rx_ctcss_nearest_is()`). This runs both
in `CheckRadioInterrupts()` and on every receive poll; it resets when the
carrier closes and is skipped while the CTCSS scanner runs. Console `$` prints
`REG_68`, the last measured tone and both inputs.

**Bench gate (passed 2026-11-02):** RX 69.3 on a BK4829 VFO:
1. 67.0 stays muted; 69.3 opens; 71.9 stays muted.
2. Repeat at the top of the table, e.g. RX 250.3 against 254.1.
3. Confirm an exact-tone reception still opens as quickly as before.

## DCS on the BK4815 lane (branch `driver/dcs-4815`, 2026-11-02)

**What the stock does (located).** The DCS branch of `FUN_08006D10`:
1. `0x59 = 0xF7A1`.
2. `FUN_08016FE6`: `0x61` [97] = `0x0100` (one code, address 0), then
   `FUN_0801C576(code, 0, 0)`, which burst-writes `0x5F`/`0x60` [95/96] =
   `0xF000 | w[22:12]`, `0xF000 | w[11:0]`.
3. `0x5C = 0x4000`, `0x5D = 0x0060`, `0x42 = 0xD083` (bit 7 CDCSS receive,
   bit 8 CTCSS off).

`w` comes from `FUN_080085B0(code, polarity)`. Emulated against the K1's
`DCS_GetGolayCodeWord()` for all 104 codes: polarity 1 gives the K1's word,
polarity 0 its complement (`^ 0x7FFFFF`). **Receive programs polarity 0**, i.e.
the code's inverted word; transmit (`0x26`/`0x27`) uses the other polarity.

`0x62` [98] (match count 3:1, slice enable 0) is a boot value in the stock,
`0x2000449E`, which `FUN_0800DCAC` fills from factory byte `0x3880 + 0x0C` =
`0x03` (slice on, 2 matches) in all three dumps. The port's boot table can't
source it and writes 0.

**What the port does (not verified):**
- `bk4815_set_rx_dcs()` writes the sequence above plus `0x62 = 0x0003`.
- `rx_4815_subaudio_service()` programs the detector for the selected VFO's code
  (CTCSS, DCS or none, as one key). For DCS it also sets the CDCSS-found enable
  in `0x73` (`0x81F0`; the stock's `0x81D0` has only CDCSS-lost) and polls
  `0x74` bits 5/4.
- The result goes to the K1 the way `CheckRadioInterrupts()` reports the
  BK4829: `g_CDCSS_Lost` (= code present) and `gCDCSSCodeType` from `0x5E` bit 2
  (0 → POSITIVE, 1 → NEGATIVE). `HandleIncoming()` opens on a POSITIVE match only.
- The BK4829's CDCSS bits are ignored on this lane (`app.c`).
- A BK4815 receive entry rewrites `0x73`, so it invalidates the detector setting.

**Open hardware questions:**
1. Does `0x74` bit 5 latch with the mask bit set?
2. Is `0x5E` bit 2 relative to the programmed (inverted) word as assumed?
3. Does the chip match both polarities, as the datasheet implies ("exactly same
   or inversed")?

Console `$` prints `0x5E`-`0x62`, the published `g_CDCSS_Lost` /
`gCDCSSCodeType`, and samples `0x5E`/`0x74`.

**Bench gate:** a BK4815 VFO (UHF) with RX DCS, e.g. 023N, and a second radio:
1. No code → muted.
2. 023N → opens.
3. A different code → muted.
4. **023I → muted** (the polarity check).
5. Dropping the code → closes within about 1 s.
6. Monitor → opens.
7. Repeat with an inverted channel code (023I against 023I / 023N).
8. Confirm BK4815 CTCSS and the BK4829 lane are unchanged.
