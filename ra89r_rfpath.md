# RA89R RF path and power

**Status: the bus, the boot bring-up order and the power picture are mapped from
the stock image; the driver work starts at the BK4829 (`ra89r_bk4829.md`).**

Two transceivers are fitted — a **BK4829** on chip select `PB8` and a **BK4815**
on `PB13` (`ra89r_bk4829.md`, `ra89r_bk4815.md`).  This file is the shared
physical layer, the order in which the stock brings them up, and what powers
them.

## The shared bus

| line | pin | direction |
|---|---|---|
| clock | `PA12` (GPIOA mask `0x1000`) | output, driven low between transfers |
| data | `PB12` (GPIOB mask `0x1000`) | **bidirectional** — output to send, released to read |
| select | `PB8` = BK4829, `PB13` = BK4815 | output, active low |

Both selects are driven by hand, not by a peripheral; every access brackets its
transfer with its own select low → transfer → high.

**Validated on the radio**: a read-only probe of both parts on a freshly booted
radio returned `0x4829` from the BK4829 and `0x4816` from the BK4815 — the clock,
the shared data line, both selects and both address encodings — and both still
answered after each had been sent its full stock boot configuration
(`ra89r_bk4829.md`, "Validated on the radio").  What the configuration *does* is
still unobserved.

The bit layer is shared by both chips:

| address | role |
|---|---|
| `0x08017D6C` | shift a byte out over `PB12`, `PA12` as the clock, MSB first |
| `0x08017FE4` | shift 16 bits in, MSB first |
| `0x0801DCF0`, `0x0801DCB8` | switch `PB12` between output and input (a pin reconfiguration, not just a level) |
| `0x08021FF4`, `0x08021F78` | the two write cores, differing only in which select they take |
| `0x080220A0`, `0x08022082` | the value-packing wrappers above them |
| `0x080180F0`, `0x08018060` | the two read paths |

Underneath those sit four generic helpers that are worth naming, because "which
pin" is otherwise guesswork:

| address | role |
|---|---|
| `0x08011B74(port, mask, level)` | drive a pin — `level == 0` writes `BRR` (port + `0x28`), otherwise `BSRR` (port + `0x18`) |
| `0x08011B64(port, mask)` | read a pin from `IDR` (port + `0x10`) |
| `0x0801199C(port, cfg)` | reconfigure a pin from a small struct: `{mask, mode, pull, speed, af}` → `MODER`/`OTYPER`/`OSPEEDR`/`PUPDR`/`AFR` |
| `0x0802422A(n)` | busy-wait delay |

The port bases are the literals the stock loads: **`0x48000000` is GPIOA** and
the data/select port loaded everywhere else is **`0x48000400`, GPIOB**;
`0x48000800` is GPIOC and `0x48000C00` GPIOD.  The read path is therefore: after
the command byte, `FUN_0801DCF0(1)`/`FUN_0801DCB8(1)` puts `PB12` into
`MODER = input` (mode 0) for the sample window and `(0)` returns it to
`MODER = output`.  The two functions are separate copies of the same
`PB12` reconfiguration, one per chip's read path.

Bus timing, as the stock generates it: the clock **idles low**.  To write a bit,
`FUN_08017D6C` drives `PB12` to the bit, waits, raises `PA12`, waits, and drops
`PA12` again — so the data is set while the clock is low and the rising edge is
what latches it.  To read, `FUN_08017FE4` samples `PB12` while `PA12` is low and
*then* pulses `PA12` high, 16 times, MSB first, with a longer delay after the
last bit.  Every `BSRR`/`BRR` write is separated by a short delay loop, which is
what makes the bus safe to bit-bang from the firmware.

The two parts frame differently, so the selects are not interchangeable: the
BK4829 is addressed with `reg | 0x80` (read) / `reg & 0x7f` (write), the BK4815
with `((reg & 0x7f) << 1) | 1` / `(reg & 0x7f) << 1`.

## The boot bring-up

`FUN_08016788` is the whole RF bring-up, and the order is deliberate:

```
FUN_080093EC()   -- selects the band/filter (FUN_08013790(3), FUN_080137D4(3, 0))
                    and drives PA1 low
FUN_08009C9C()   -- drives PC13 low
FUN_08006B78()   -- BK4829 configuration (37 writes + FUN_0801BAF4's write to 0x7d + 2)
FUN_08006A0C()   -- BK4815 configuration (33 writes + the 18-register table)
FUN_0800D434()
if (state[0x20000301] == 1) FUN_080139E4()
FUN_08007F90()
```

Each chip's own init re-tests it first and, if the identity check fails, calls
`FUN_08015D14(0x0b)` and returns without configuring that part.

**What that error call actually shows is unresolved.**  `FUN_08015D14(code)`
renders one short string with `FUN_08014CF4(0x10, 0x1a, s)`, where
`s = *(u32 *)(table[index] + code * 4)`, `table` is the pointer array at
`0x08024E98`, and `index` is the byte at `0x20009F28 + 0x1c` — a runtime
configuration byte (written as `1` by `FUN_0800FE18`), whose entry 1 is a **RAM**
pointer (`0x20000258`), so the table it selects is built at run time, not in the
image.  The image does contain both candidate literals, `4815 Error`
(`0x0802710C`) and `4829 Error` (`0x08027118`, with a pointer in the pool at
`0x08027910`), and the two detects do use **different** call sites — but they pass
the **same** code `0x0b`, so the string cannot be chip-specific on the strength of
the call sites alone.  An earlier version of this write-up assigned one literal to
each chip; that claim is withdrawn, and the index-to-language-to-string mapping is
an open point rather than a finding.

## What powers what

A sweep of **every GPIO write in the stock image** (88 call sites resolved, with
their port, mask and level) leaves no candidate for an MCU-driven supply enable:
no pin is taken high as a rail gate.  The pins the stock actually drives are the
buses, the indicators and the audio path:

| pin | role |
|---|---|
| `PA12`, `PB12`, `PB8`, `PB13` | the RF bus and its two selects |
| `PC14`, `PB2`, `PD0` | the companion gauge/charger chip (`ra89r_battery.md`) |
| `PA15`, `PB3`, `PB4`, `PB5` | the external SPI NOR flash (`ra89r_eeprom.md`) |
| `PB15`, `PA8`–`PA11` | the LCD |
| `PA0`, `PA1` | the TX/RX indicator field (see `ra89r_led.md`) and RF control |
| `PC13` | a static audio/RF control line: driven low by `FUN_08009C9C`/`FUN_08009CB0` (11 call sites) and raised by `FUN_080177A8` from the T/R transition; on this codeplug it is held **high** and does **not** follow the squelch — see `ra89r_rffeatures.md` |
| `PA13`, `PA14` | the **audio-path pair**: configured together as push-pull outputs by the boot GPIO init `FUN_08013C74` (GPIOA mask `0x6000`), with `FUN_08020028`/`FUN_08018A10` driving each to a level selected by codeplug settings byte 2 bit 2 (`0x20009F28 + 0xc`).  **`PA14` goes high while the squelch is open and low when it closes** (`FUN_08004C84` vs `FUN_0801D458`) and high in TX; `PA13` is its counterpart.  They are the `SWDIO`/`SWCLK` pads, given up for GPIO — see `ra89r_rffeatures.md` |

So the picture is:

* **`PA1` is not a lamp line** — it is driven low as part of the RF bring-up
  (`FUN_080093EC`) and toggles with the TX/RX indicator work, i.e. it is RF
  control, not a supply.  This is the same pin the LED search tested and found
  "does nothing visible" (`ra89r_led.md`).
* **`PC13`** is likewise a control line in the RF/audio paths, not a rail.
* **`PA14`** (with `PA13`) is the one pin whose level tracks the squelch, so it is
  the audio-path enable the T/R-only reading missed.
* **`PD0`** is pulsed low → high at boot (`FUN_0801D69C`, which clears bit 0 of
  `GPIOD` at `0x48000C00`, does a handshake and sets it again) and driven low from
  a handful of other paths; it behaves as the companion chip's reset/handshake, so
  **charging and pack management are delegated to that chip**, with the MCU's
  only line to it being `PD0` plus the two-wire bus.
* The **MCU's own clock** is the other half of "power": the stock runs from the
  PLL its bootloader leaves running (`CR = 0x0040e583`, PLLON) while this firmware
  forces HSI at 8 MHz.  That is why every bit-banged bus here runs slower than
  the stock's.

A decompiler pass over the same image (Ghidra, program based at `0x08004000`)
re-derived each pin claim above from the decompiled functions rather than the
listing, and every one held.  What is still missing is the pass over the
power-on/off and sleep paths.

## The rest of the RF path

A decompiler pass over the runtime paths (not the boot init) now answers most of
"which chip does what":

* **T/R**: `FUN_08016228` is the transmit/receive entry and `FUN_08009CC4` is its
  counterpart (back to the idle state).  The body only runs if the per-channel
  state (`param_1 + 0x1c`) says so, or if **`PC13` reads low** — the one place
  `PC13` is an *input* rather than the output line `FUN_08009C9C` drives.  Both
  functions then branch on the **same flag** at `0x20000303`
  (`DAT_08016380` in one, `DAT_08009d40` in the other), and the flag *chooses
  which chip is driven*:
  * flag `== 1` → the **BK4829**: register `0x47` = `0x6042`/`0x6142` (when the
    state byte at `+0x75` is 0) or `0x6040`/`0x6740` (otherwise, also clearing the
    RAM flag at `0x20000336`), the choice within each pair set by the flag at
    `0x20003DDC`; register `0x13` = `0x03BE` or `0x03FF` along with it; register
    `0x30` = `0xBFF1` (skipped only when the config byte at `0x20009F28 + 0x34`
    is 0 *and* state `+0x75` is 0); and register `0x31` is read and bit 2 cleared.
  * otherwise → the **BK4815**: one register, `0x0c` — `0x0203` to enter the
    state, `0x0a03` to leave it, which is exactly the value its boot init writes.
* **Band/filter — correction.**  The two calls are *not* two halves of one
  register.  `FUN_080137D4(mask, value)` is **BK4829-only**: it reads register
  `0x33`, clears bit `14 - n` for every bit `n` set in `mask`, and sets bits
  `0..6` from `value`.  `FUN_08013790(band)` is **BK4815-only**: it rewrites the
  low six bits of register `0x75` with `0x09` for band 0, `0x11` for 1, `0x0A`
  for 2 and `0x12` for 3.  The bring-up calls `FUN_08013790(3)` and
  `FUN_080137D4(3, 0)` — band 3 on the BK4815, and clear the mask-`3` filter bits
  on the BK4829.
* **Squelch / metering**: `FUN_080052B8` reads **BK4829** registers `0x63`,
  `0x67` (masked `& 0x1ff` — the RSSI the debug page prints as `RSSI R67 %d`) and
  `0x65`, plus `0x99` elsewhere, and walks **BK4829** register `0x13` in an
  eight-step ramp (`0x3b0 | (8 - level)`, all the way to `0x3ff`).
* **TX power**: the UI carries `Power Select` (`0x08017900`), `Power 5W`
  (`0x08017910`) and `Power 10W` (`0x0801791C`) — the setting the RA89G V52
  "10 W enable" build is named for.  Which registers it lands in has not been
  traced.  The strongest lead is the BK4829's `0x7d`: the stock computes it as
  `0xe940 | v`, where `v` comes from a **three-bit codeplug setting**
  (`buffer[10] & 7` of the 32-byte block at EEPROM `0x2020`) and a flag that this
  image only ever writes as 0 — which resolves to `0xE958` on this radio's
  codeplug.  The six-step ladder that formula produces fits a power level better
  than anything else in the menu, but the field itself is still unnamed
  (`ra89r_bk4829.md`).
* **Status LED**: the LED is an RF-chip indicator rather than an MCU pin
  (`ra89r_led.md`), so it comes with this bring-up.

## What the T/R flag is

The flag at `0x20000303` is not a compile-time or menu option — it is computed
from the channel's frequency.  `FUN_0800E560(channel)` returns the channel's
frequency field (`+0x94`, or `+0x98` when the direction/offset flags at `+3`,
`+0x73` and `+0x74` select the other one), and three functions set the flag from
it:

| function | test | effect |
|---|---|---|
| `FUN_0800978C`, `FUN_08017340`, `FUN_0800D684` | `freq < 0x03567E00` → flag `0`, else `1` (or `1` outright when state `+0x75` is set) | flag from the comparison |
| `FUN_08006360` | raw `+0x94 <= 0x00CC77C0` | sets state `+0x75` **and** the flag to `1` |

The units matter and are settled: the codeplug stores frequencies in **10 Hz**
steps — a 145.7500 MHz channel is the u32 `0x00DE6598` (14,575,000; the record
bytes are `98 65 DE 00`) in its 21-byte record, the band table at EEPROM `0x1F40` reads 10,800,000 / 17,400,000
/ 43,000,000 / 52,000,000 for 108 / 174 / 430 / 520 MHz, and the firmware itself
contains `0x00A4CB80` (10,800,000) with no Hz-unit 108 MHz constant anywhere.
So the two thresholds are **560 MHz** and **134 MHz**.

560 MHz is above the radio's tunable range, so that comparison never turns the
flag on by itself; the operative rule is `FUN_08006360`'s — at or below 134 MHz
both the state byte and the flag go to `1`, and everything above it leaves the
BK4815 branch of the T/R path in charge.

That makes this board's "which chip does what" a frequency-derived split after
all, and it is an odd place for one: 134 MHz sits *inside* the stored VHF range
(108–174 MHz) rather than on a band edge, and there is no AM/airband mode in the
UI strings.  The state byte `+0x75` is also read by several other per-band
routines, so a plausible reading is that the BK4829 is the bottom-of-VHF
(air-band-shaped) path and the BK4815 the main path — but that is a hypothesis to
test on the radio, not a finding.  What *is* established is that the choice comes
from the channel frequency plus that state byte, never from a build option.

**The exclusivity is only in the T/R mode registers.**  The per-mode routine
`FUN_08016DE8` configures *both* parts in one pass: it selects the channel's
filter bandwidth on the BK4829 (`FUN_0800B634(channel[0x79] - 1)`, programming
register `0x09`), writes the BK4829's `0x32`/`0x24`/`0x47`/`0x4a`/`0x50`,
rewrites its band/filter register `0x33` twice (`FUN_080137D4(3, 0)` then
`FUN_080137D4(0x10, 0)`), and in between calls `FUN_08013790(2)` — the **BK4815**
band register `0x75` — whenever the channel byte at `+0x11` is 1 or 2.  So both
transceivers are detected, initialised and re-tuned per channel; what the
frequency flag decides is only which part receives the *T/R state* writes
(`0x47`/`0x13`/`0x30`/`0x31` versus `0x0c`).  Whether both parts are then in the
signal path at once, or one is a band the other is not, is not established.

## The two calls at the end of the bring-up

`FUN_0800D434` is identified: it sets `RCC_AHB2ENR` bits 3 and 4 (the GPIOB and
GPIOC clocks) and then configures **`PB2` and `PC14` as outputs** (mode 1, no
pull, speed 2) through `FUN_0801199C`.  Those are exactly the two wires
`ra89r_battery.md` attributes to the companion gauge — so the "RF bring-up" also
establishes the gauge bus, which is worth knowing before blaming the RF code for
that bus.  It finishes with `FUN_080137D4(8, 0)`, i.e. a second poke at the
BK4829's filter register `0x33`.

`FUN_08007F90` is a mode dispatch on the byte at `0x20009F28 + 0x24`:

```
< 2:  clear state[0x20000016]
      0 -> FUN_08009D80()   = FUN_0800A968(0, 2)
      1 -> FUN_0801638C()   = FUN_0801533C(config[0x2d])
      clear state[0x20000016] again
>= 2: FUN_0801537C() then FUN_0801638C()
```

It touches no RF register directly; the three callees are not identified yet.

## TX, and how the power is handled

**There is no PWM and no MCU pin that enables a PA: the transmit power is a
transceiver register, and the MCU's TX-side actions are the band/path switch and
the indicators.**  The stock's TX entry, walked from both ends:

```
FUN_08018AB8   enter TX (PA13 high = red LED, PA14 low = green off)
  -> FUN_08017306(p, 1)
     -> FUN_08017280   the TX setup
          FUN_08017176 -> FUN_08017158          tune (0x38/0x39)
          FUN_08006E6A(state[0x91]) -> FUN_0801763C -> 0x43 = filter bandwidth
          FUN_08019E2C(state[0x77])             scramble
          FUN_080220A0(0, 0x24)
          FUN_080220A0(0x6142, 0x47)            AF source
          FUN_080220A0(0x9D1F, 0x37)
          FUN_080220A0(0x3B20, 0x50)
          FUN_080220A0(0, 0x70)
          FUN_080220A0(0, 0x30) then 0xC1FE     PA gain (bit 3) + mic ADC (bit 2) + TX DSP
```

`0xBFF1` is the other TX-stage value: the T/R path `FUN_08016228` writes it with
`0x47`/`0x13` and the chip-GPIO cluster, and `FUN_08018A46` writes `0x37 = 0x9D1F`
then `0x30 = 0xBFF1` (or `0xBDF1` in the VOX case).  The port's imported
`BK4819_PrepareTransmit()` already produces the same key words (`0x36 = 0`,
`0x37 = 0x9D1F`, `0x52 = 0x028F`, `0x30 = 0` then `0xC1FE`), because it is the
same K1 sequence -- so the register side of TX is already available here.

**The power itself is `0x7D`, and it is codeplug-driven.**

* `FUN_0801BAF4` writes `0x7D = 0xE940 | bias`, with `bias` computed from the
  decoded settings field `0x20009F28 + 0xf` (filled by `FUN_0800FE18` from bits
  2:0 of codeplug byte 10) and the 2-bit field at `+0x10`:
  `bias = 0x0C + 4*level`, or `0x0C + 3*level` when `+0x10 == 1`;
* this radio's byte 10 is `0x03` (level 3, variant 0) -> `bias = 0x18`, so
  **`0x7D = 0xE958`**;
* it is applied per *radio configuration*, not per transmission: the only caller
  is the BK4829 configuration `FUN_08006B78`, reached from `FUN_08016788` (band
  select, `PC13` low, both chip configurations, chip-GPIO clear) via `FUN_0800F32C`;
* the K1 driver writes the same register at init with its own bias (`bk4819.c`:
  `0xE940`, `bk4829.c`: `0xE920`), so this is the part's power/bias control and the
  stock is simply computing the value.

**Checked and *not* power**, because each looked like it:

* `0x43` -- `FUN_0801763C(state[0x91])` writes `0x3028` (levels 0/1) or `0x4048`
  (level 2), and in the K1 this register is the **filter bandwidth**
  (`BK4819_SetFilterBandwidth`): 0x3028/0x4048 differ in the RF/weak-RF/AF-LPF
  fields, so `state[0x91]` is a bandwidth mode, not a power level;
* `0x48`/`0x6C` -- `FUN_0802481C(state[0x91], tx)` -> `FUN_080247E0` writes
  `0x48 = 0xB00F | (t << 4)` and `FUN_080247A0` writes `0x6C = 0x8127 | (t << 11)`,
  with `t` read from codeplug RAM: the audio/deviation trims for that mode;
* `0x36` -- the K1's `BK4819_SetupPowerAmplifier` register (PA bias + PA-CTL enable
  + gain) is **never written by the stock**: there is no `r1 = 0x36` anywhere in
  the image.  On this part the PA settings live in `0x7D` and `0x30` bit 3.

**The MCU-side TX actions**, from the whole TX callee tree (47 functions):

* `FUN_08013A70` / `FUN_08013B12` drive `PA0`, `PA1` and the chip's GPIO pin 1
  (register `0x33`, mask `0x20`) as a **4-way RF path/band select** -- the very pin
  the K1 calls `BK4819_GPIO1_PIN29_PA_ENABLE`, wired as a path switch on this board;
* `PA13`/`PA14` are the red/green LED (TX = red), and `FUN_08016228` even *reads*
  `PA13` as part of its T/R decision;
* `PC13` is raised by `FUN_080177A8` in the T/R path and lowered by
  `FUN_08009C9C`, which the RF bring-up `FUN_08016788` calls;
* no timer channel is routed to a pin for a PA ramp, and the only DAC reference in
  the tree arrives through the audio/DMA path (`FUN_0800A968`, DAC at `0x40007400`).

So the port's TX work is the K1 `PrepareTransmit`/`EnableTXLink` sequence (present
already) plus **`0x7D = 0xE958`**, the band/path pins and the antenna switch, with
the level taken from the codeplug the way `FUN_0801BAF4` does.  Open: which single
write actually turns the PA on for this board (`0x30` bit 3 or `0x7D`), and where
`PC13` goes -- a bench TX with a power meter settles both.

## Open

1. **Which chip does what.**  Answered as far as static reading goes: the BK4829
   carries the filter (`0x33`), the RSSI/metering (`0x63`/`0x65`/`0x67`/`0x99`),
   the squelch ramp (`0x13`) and the T/R set (`0x47`/`0x30`/`0x31`); the BK4815
   carries the band select (`0x75`) and the path switch (`0x0c`); and the T/R
   path picks between them from the channel frequency (`0x20000303`, see "What
   the T/R flag is").  Still open: why the crossover is 134 MHz, what the state
   byte `+0x75` means on its own, and which part actually radiates.
2. The power-on/off and sleep handling, and whether anything else gates the RF
   rails — the decompiler is now available, but these paths have not been walked.
3. The per-channel/per-band routines that feed the T/R registers.  The TX power
   setting is answered above: `0x7D`, computed by `FUN_0801BAF4` from the codeplug
   level; still open is which of `0x7D` and `0x30` bit 3 actually enables the PA.
4. `FUN_0800A968`, `FUN_0801533C` and `FUN_0801537C` (the `FUN_08007F90` callees)
   are not identified.
5. Which string `FUN_08015D14(0x0b)` actually renders, and what the byte at
   `0x20009F28 + 0x1c` selects (the language table it indexes is built in RAM).
