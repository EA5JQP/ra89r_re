# Stock RF feature routines — AF, AGC, signalling, idle states

**Status: first pass.  The routines are located and their register writes
extracted; nothing here has been exercised on the radio yet.**

`ra89r_bk4829.md` covers the part, its identity and its boot configuration.  This
file is the layer above: where the stock implements the features the K1-compatible
port (`firmware/App/driver/bk4819.c`) exposes as `SetAF`, `SetAGC`, the
CTCSS/CDCSS/DTMF/scramble/VOX group and the sleep/idle/bypass states.  Those were
listed in `AGENTS.md` as "not visible in the stock image" — that was wrong, and
this file is the correction: they are all there, they simply had not been looked
for by register.

## How they were found

The K1 driver says which registers each of its features touches, so the search can
run backwards: take the register set of each feature, then ask which stock
functions write those registers.  The port's own file gives the first half —

| feature (K1 entry points) | registers |
|---|---|
| AF | `0x47` output select (bits 8–11), `0x48` DAC gain, `0x6F` read, `0x30`/`0x70` tone gate, `0x71` tone frequency |
| AGC | `0x10`–`0x14` gain table, `0x7E` (bit 15 = fix mode), `0x49`, `0x7B`, `0x7C`, `0x06`, `0x20` |
| CTCSS/CDCSS/tail | `0x07`, `0x08`, `0x51`, `0x52` |
| DTMF | `0x21`, `0x24`, `0x71`, `0x72`, `0x0B` |
| compander | `0x28`, `0x29`, `0x31` |
| scramble | `0x2B`, `0x31`, `0x71` |
| VOX | `0x31`, `0x46`, `0x79`, `0x7A`, `0x64` |
| idle/sleep/bypass | `0x30`, `0x37`, `0x2B`, `0x73`, `0x7E`, `0x36`, `0x50` |

— and scanning the stock listing for writers of those registers gives the second.
The scan is mechanical (`movs r1, #<reg>` immediately before a `bl` to the write
wrapper `FUN_080220A0` for the BK4829 or `FUN_08022082` for the BK4815), so a
register written through a computed value would be missed; every register below
was found by an immediate.

## AF / audio

* **`FUN_08015F48`** picks which part carries audio.  On a channel-state test it
  writes either the BK4829's `0x47` = `0x6040`/`0x6042` (chosen by the state bytes
  at `+0x75`) or the BK4815's `0x49` = `0x9A02`, then clears a flag.  Both `0x47`
  values put `0` in bits 8–11, i.e. AF source 0 — the mute/source-select field
  `afOutRegSpec` describes — so this reads as "route audio to the active part and
  mute the other", not as a general AF selector.
* **`FUN_08005D2C(value, frequency, enable)`** is the stock's **tone player**: it
  stores the first argument in a state struct and, when enabled, writes **`0x71`**
  after a soft-float scale (`FUN_08004528`/`FUN_08004344`/`FUN_08004542`).  Same
  register and same float-scaling shape as the K1's `PlayToneRaw` inside
  `scale_freq`, so the port's tone path has a stock counterpart to compare against.
* `0x48` (AF DAC gain, the K1's `SetRxAudioGain`) is written by `FUN_08019DF4`
  twice and read by `FUN_080247E0` — the volume path, not extracted further.

## AGC

The stock's AGC gain table is **`0x10`–`0x14`**, and this is the clearest result of
the pass because it can be cross-checked against the K1's own file:

| reg | stock boot init (BK4829) | K1 `InitAGC` |
|---|---|---|
| `0x10` | `0x0318` | `0x0318` |
| `0x11` | `0x033A` | `0x033A` |
| `0x12` | `0x03DB` | `0x03DB` |
| `0x13` | `0x03DF` | `0x03DF` |
| `0x14` | `0x0210` | `0x0210` |
| `0x49` | `0x2A32` | `0x2AB2` |
| `0x7E` | `0x303E` | — |

The five table entries are **identical**, and the K1 source says why: it carries a
comment that its "new strategy" keeps the AGC profile "aligned with the stock
BK4829 firmware".  The one value that differs, `0x49`, is exactly the difference
already recorded in `ra89r_bk4829.md`'s K1 comparison table.  So the port's AGC
table is not a guess on either side — two independent implementations agree.

`0x7E` = `0x303E` puts bit 15 clear, i.e. the K1's `SetAGC` notion of "AGC not in
fix mode".  The stock never *reads* `0x7E` for a gain figure, so it has no
counterpart to `GetRxGain_dB`.

**The runtime knob is `0x13`.**  `FUN_080052B8` — the same routine that reads the
RSSI for the squelch — walks `0x13` in an eight-step ramp (`0x3B0 | (8 - level)`,
up to `0x3FF`).  The K1 names `0x13` "RX AGC Gain Table[3]", so the stock's squelch
response is an **AGC gain step**, which ties together two things this project had
recorded separately: the squelch ramp and the AGC table are the same register.

The BK4815 gets its own table update in **`FUN_080171D0`** (`0x11`, `0x12`, `0x14`,
plus `0x0C`/`0x16`/`0x2B`/`0x6D`/`0x73`/`0x12`) — the per-band routine
`ra89r_bk4815.md` already lists.

## Signalling

* **CTCSS/CDCSS/tail — `FUN_08006DE4`**: writes the BK4829's **`0x51`** =
  `0x9032` or `0x801D` (chosen by the state byte `+0x175`) and **`0x52`** =
  `*(u16 *)(0x20003C9C + 4)`.  Those two registers are exactly what the K1's
  `SetCTCSSFrequency`, `GenTail` and `Play*Tail` use.  The `0x52` source is worth
  noting on its own: `0x20003C9C` is the settings struct `FUN_0800DCAC` fills from
  the codeplug (`+1`, `+2` and `+4` from record fields), so **the CTCSS/tail code
  is a codeplug setting**, not a constant.
* **Compander — `FUN_0801C15C(on, mode)`**: reads the **BK4815's `0x28`**, masks
  with `0x1FFF` and ORs `0xA000`/`0xE000` (per `on`) or `0x8000` (mode 2) back in.
  The K1's `SetCompander` uses `0x28`/`0x29`/`0x31`, so the register is the same
  on this part too.
* **Scramble — `FUN_08019E2C(word)`**: writes `0x31 |= 2` and then calls
  `FUN_0801C2BC(word)`, which writes **`0x71`**; when `word == 0` it writes `0x31`
  with only the config bit.  That matches the K1's
  `SetScrambleFrequencyControlWord` (0x71) plus `EnableScramble` (0x2B/0x31/0x71),
  and says bit 1 of `0x31` is the enable.
* **VOX — `FUN_0801754A`**: writes `0x46 = 0xA00A`, `0x79 = 0x6005`,
  `0x7A = 0x589A`, then `0x31 |= 4` and `0x30 = 0xBDF1`.  The K1's `EnableVox`
  register set is `0x31`/`0x46`/`0x79`/`0x7A` — an exact match, so this routine is
  the stock's VOX enable, and `0x46`/`0x79`/`0x7A` are VOX parameters.
* **DTMF — not on the part, most likely.**  The K1 gives DTMF its own second tone
  register (`0x72`, `PlayDTMF`) and reads `0x0B` for the 5-tone code; the stock
  writes **no `0x72` anywhere**.  The two obvious remaining candidates turned out to
  be something else:
  * `FUN_0801BF54` writes the **BK4815's `0x0B`** as a three-step sequence
    (`0x0800`, `0x8800`, `0x9800` — bits 11 then 12 walking up), which is a mode or
    calibration pulse, not tone data;
  * `FUN_0801D268` writes the **BK4815's `0x0C`** as `0x0203` (with a call to
    `FUN_080247A0`) or `0x0A03`, gated on the channel-state bytes `+2`/`+0x1c`.

  That second one is a useful confirmation on its own: `0x0C` = `0x0203`/`0x0A03`
  is the pair `FUN_08016228` and `FUN_08009CC4` already use for the T/R state, so
  **`0x0C` is the BK4815's receive/transmit state register**, now from three
  independent sites.

  With no second chip tone register, the likely reading is that the stock generates
  DTMF on the **MCU** side — it already has its own tone generator (the beeper is
  TIM4 plus the DAC, `ra89r_beeper.md`) — rather than in the transceiver.  That
  stays an inference until the DTMF menu path is followed; it is not a located
  routine.

## Idle, sleep and mode restore

**`FUN_0800CE1C`** is a mode-change routine and the most informative of the four
groups' leftovers.  On a state flag it either

```
park the BK4815:   0x0C = 0xFFFB  (or 0xFFEB, per a mode byte)
restore the BK4829: 0x30 = 0x0200, 0x37 = 0x9D00, 0x37 = 0x9D1F
```

and then re-selects the BK4815's band (`FUN_08013790(3)`).

Read against the K1's states this gives the bits of `0x37` a meaning:

| value | written by | meaning |
|---|---|---|
| `0x9D1F` | stock boot init, `FUN_08017280`, `FUN_08017668`, and the restore above | the part's normal/active state |
| `0x9F1F` | K1 `RX_TurnOn` | receive — bit 9 added |
| `0x1D00` | K1 `Sleep` | bit 15 **cleared**, i.e. powered down |
| `0x9D00` → `0x9D1F` | the restore above | a two-step pulse back to active |

That two-step write is the stock's own way of bringing the part back, and it
brackets the K1's single `0x9D1F` from one side and `0x1D00` from the other.  The
BK4815's "off" state is a low `0x0C` word (`0xFFFB`/`0xFFEB`) rather than a
register clear, which is consistent with `0x0C` being that part's mode register
throughout (`0x0A03`/`0x0203`/`0xF823`/`0xF023` in `ra89r_bk4815.md`).

`0x30` is the other state register and the busiest: writers include `FUN_08018A46`
(three writes in a row), `FUN_08009CC4`, `FUN_0800C094`, `FUN_08017280`.  The K1's
`Idle`, `Disable`, `Sleep`, `PrepareToPlayTone` and `EnterTxMute` all touch `0x30`
as well, so it carries several independent bit fields and is not yet mapped.

## Does anything enable a speaker PA when the squelch opens?

**Yes — `PA14`.**  It is the only MCU pin whose level follows the squelch, and the
level it takes is selected by a codeplug bit.  The whole chain is:

* `FUN_080052B8` reads `0x63`/`0x65`/`0x67`, ramps the AGC step `0x13`, prints
  `RSSI R67 %d` and returns 0/1/2 — no pin, and no register beyond `0x13`;
* `FUN_08005440` only picks between it and a second verdict routine,
  `FUN_08005218`, for the other mode;
* `FUN_0801D420` acts on the verdict: 1 → `FUN_0801D3F0(2, 1, state)`,
  0 → `FUN_0801D3F0(1, 2, state)`;
* `FUN_0801D3F0` is a **counter**: it steps a byte at `state + 0x21` up or down,
  clamped to 0..10.  Verdict 1 is the increment, and verdict 1 is the
  *signal-present* answer (`FUN_080052B8` returns it when `0x67 & 0x1ff` is above
  the codeplug mark), so **`state[0x21]` high = squelch open**;
* the counter's consumer is **`FUN_08004C84`**, the per-tick RX handler (gated on
  the mode byte `state[2] == 0`), and that is where the pin is:

| `state[0x21]` | path | `PA14` |
|---|---|---|
| `< 3` | `FUN_0801D458`, the RX teardown (also writes BK4829 `0x3f`/`0x42`/`0x5a`) | **LOW** |
| `3..6` | dead band (hysteresis), nothing happens | unchanged |
| `>= 7` | the servicing block, which calls `FUN_08018A10` on first entry | **HIGH** |

  (a squelch setting of 0, `DAT_08004dfc[0x11] == 0`, takes the `>= 7` branch
  unconditionally -- i.e. monitor/open);
* the two paths latch through **`state[0x1e]`**: the open block drives the pin only
  when the latch is 0, then sets it to 1; `FUN_0801D458` is guarded by the latch
  being 1 and clears it.  So the pin is driven once per transition, not per tick.

`FUN_08018A10` is not a constant -- it drives `PA14` to the level held in the
decoded settings struct, `*(char *)(0x20009F28 + 0xc)`:

```
08018A12  ldr  r0, [pc, #0x24]   ; =0x20009F28
08018A14  ldrb r0, [r0, #0xc]    ; 0x20009F34
08018A16  cbnz r0, 0x08018A26
08018A18  movs r2, #1            ; -> BSRR = 0x4000   (PA14 HIGH)
08018A26  movs r2, #0            ; -> BRR  = 0x4000   (PA14 LOW)
```

That byte is **bit 2 of codeplug settings byte 2** (`FUN_0800FE18` computes
`pw[0xc] = (local_22 >> 2) & 1`).  This radio's 32-byte settings block at `0x2020`
is `96 53 00 02 ...`, so byte 2 = `0x00` and `PA14` is **active high**: **high while
the squelch is open, low when it closes**.  It is also driven high in TX (below).

`PA14` is not squelch-only: the mode/audio handlers `FUN_08015D88` and
`FUN_08015E28` end with the same two-way choice -- `state[2] == 0` (RX) → low,
otherwise `FUN_08018A10()` → high -- and those are the routines that write the
BK4815's `0x0C` mode word and call the T/R path, so the pin is high in TX as well.

**`PC13` is a real line -- but not this one.**  It is driven both ways, and scanning
every GPIO write in the image by pin and level (keeping only pins driven *both*
ways, i.e. a real enable rather than a pulse or a bus) finds it, together with the
companion gauge's `PB2`/`PD0` and the `PA13`/`PA14` pair above:

* driven by **`FUN_080177A8`**, which has exactly **one caller**: `FUN_08016200`,
  called from the **transmit/receive path** `FUN_08016228`;
* its branches are `config+0x38 == 0` → **HIGH** (raw: `movs r2,#1` → `BSRR`),
  otherwise LOW or HIGH per `config+0x39` (`movs r2,#0` → `BRR`, or `#1` → `BSRR`);
* both gates are **codeplug** bits: `config+0x38` ← settings byte 9 bit 0 and
  `config+0x39` ← bit 5, via `FUN_0800FE18`.

On this radio that byte is **`0x00`**, so `config+0x38 = 0` and `FUN_080177A8` takes
its unconditional `PC13` HIGH branch: the conditional low-drive is unreachable as
this radio is configured, and `PC13` therefore **does not follow the squelch**.  The
11 unconditional `FUN_08009C9C`/`FUN_08009CB0` call sites elsewhere drive PC13 low,
and `FUN_08016200` is the only thing that raises it again, so the line's state is
whatever ran last -- a static enable, not an unmute.

**How to check this on the radio.**  With the stock firmware running, `PA14` should
be **low at idle, high while a carrier holds the squelch open, low again after it
drops**, and **high while transmitting**; `PA13` should sit **high** in normal
operation and drop low during a reset/disable or an RX/scan step.  That is a
two-channel scope (or two meter runs) on the `PA13`/`PA14` pads.  On our side the
callback the port stubbed is the thing to point at them: the `driver/audiocontrol`
branch currently drives **`PC13`**, which this analysis says is the wrong pin --
it should drive `PA14`, and mirror it on `PA13` if the pair turns out to be
enable+unmute rather than one line.

**`PA13` is `PA14`'s twin.**  It sits on the same settings bit, in the same boot
init, and is driven as the second half of the same pair:

* the boot GPIO init **`FUN_08013C74`** (called from the boot path `FUN_0801D718`)
  configures GPIOA mask **`0x6000` = `PA13|PA14`** as push-pull outputs, speed 0,
  pull-down -- in the same block as `PA0|PA1` (mask 3) and `PB13|PB15` (mask
  `0xA000`).  Both pins are therefore driven outputs, not debug pads;
* **`FUN_08020028`** is `FUN_08018A10`'s exact counterpart for `PA13` (mask
  `0x2000`, same `0x20009F28 + 0xc` level);
* the pair is driven together and in opposition:

| path | `PA13` | `PA14` |
|---|---|---|
| `FUN_08018AB8` (enable / power-up) | config level (HIGH) | LOW |
| `FUN_0801FFA0` (state reset: `FUN_0800419A` over `0x45c` bytes) | LOW | LOW |
| `FUN_08017340` (RX/scan step setup, ~9 call sites) | LOW | -- |
| `FUN_0801D458` (squelch closed) | -- | LOW |
| `FUN_08004C84` open block, `FUN_08015D88`/`FUN_08015E28` (TX), `FUN_08019C58` | -- | config level (HIGH) |

* both also have a *toggle* wrapper that reads the pin and flips it -- `FUN_08005F90`
  for `PA13`, `FUN_0800656C`/`FUN_08006070`/`FUN_08019C58` for `PA14` -- so the two
  look like a two-line path control (power/enable plus unmute or anti-pop), not like
  two independent indicators.

`PA13`/`PA14` are the Cortex-M `SWDIO`/`SWCLK` pads: the stock reconfigures both as
GPIO, so it gives up SWD as soon as `FUN_08013C74` has run.  What is *not* settled is
the physical destination -- amplifier enable and mute, an analogue path switch, or an
indicator -- because that needs a scope on the pins.  What is settled is that the
earlier "`PA13`/`PA14` are the debug pins, reused" reading (and the resulting "no pin
follows the squelch" conclusion) was wrong.

**The chip-side third place.**  A path can also be switched from the transceiver's
own GPIO register — `0x33` bits 0..6, the K1's `ToggleGpioOut`.  Its only writer is
`FUN_080137D4(mask, value)`, and the 16 call sites set chip pins 0/1 (bring-up), 2
(`FUN_08004E20`), 3, 4, 5 (`FUN_08013A70`/`FUN_08013B12`) and clear all seven
(`FUN_08013C24`).  The T/R transition does the whole cluster at once:

```
FUN_08008F2C()  -> FUN_0801AFF4() -> chip pin 2 = 0, chip pin 5 = 0
FUN_080177A8()  -> MCU PC13, gated by the codeplug bits above
FUN_08021888()  -> sets a RAM state flag only
```

Unlike `PA14`, none of these chip pins is driven by the squelch state.

## Open

1. **Nothing here has been run on the radio.**  These are register-set and code-
   shape identifications, corroborated where the K1's own file agrees (AGC, VOX),
   but not exercised.
2. `0x30`'s bit fields are unmapped, and it has the most writers of any register.
3. DTMF has no stock counterpart located (above).
4. DTMF: no chip-side routine found (see above).  The MCU-side tone generation
   is the likely explanation and needs the DTMF menu path followed to confirm.
5. The AF gain path (`FUN_08019DF4`, `FUN_080247E0`) and the tone player's float
   scale constant (the double at `0x08005D7C`) are not extracted.
6. `0x64` (VOX amplitude, read by `FUN_08017618` on this part) and the K1's
   `GetVoxAmp` were not compared.
7. **`PA13`/`PA14`'s physical destination is not identified.**  Their behaviour is
   established (above) but not what they switch, which is why the port cannot yet
   wire the audio-path callback with confidence.  It is a scope job: `PA14` at idle
   / squelch open / squelch closed / TX, `PA13` at reset and during an RX/scan step.
   The 32-byte settings block's byte-2 bit 2 (`0x20009F28 + 0xc`) is worth finding in
   the CPS menu for a name -- whatever option it is, it selects this pair's level.
8. The **squelch-off path's register work** (`FUN_0801D458` writes BK4829
   `0x3f`/`0x42`/`0x5a` and the `FUN_08022082`/`FUN_080220A0` pair) is not decoded;
   it is the chip-side half of the same mute.
