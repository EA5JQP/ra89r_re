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
  `0x33`, and for each bit `n` set in `mask` clears output bit `0x40 >> n` and the
  paired bit `14 - n`, setting `0x40 >> n` when bit `n` is set in `value` (so mask
  bit `n` is the K1's `BK4819_GPIO_PIN_t` pin number and the output bit is
  `0x40 >> n`; the full call table is under "Band and path selection: the pins").
  `FUN_08013790(band)` is **BK4815-only**: it rewrites the low six bits of
  register `0x75` with `0x09` for band 0, `0x11` for 1, `0x0A` for 2 and `0x12`
  for 3.  The bring-up calls `FUN_08013790(3)` and `FUN_080137D4(3, 0)` — band 3
  on the BK4815, and clear the mask-`3` output bits on the BK4829.
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

**Correction: the TX setup is selected, and the stock selects the BK4829 on
every band.**  This paragraph previously read `FUN_08017280` as running "for
every TX" and left "which part radiates above 134 MHz" open.  It does not:
`FUN_08018AB8` (the transmit entry) calls `FUN_08017306(chan, 1)` with the
argument **hard-coded to 1**, and `FUN_08017306` runs `FUN_08017280` (BK4829) for
`1` or the BK4815's `FUN_080171D0` for `0`.  The `0` branch has no caller, so the
BK4815's TX config is **dead code** and the stock's transmitter is the BK4829 for
all bands.  The BK4815 is instead the **receive path above 134 MHz**
(`FUN_08016EE0`'s `flag == 0` branch powers it up, `FUN_08005218` reads its
`0x43`/`0x44` meters).  `ra89r_bk4815.md` has the call sites and the register
values.  The codeplug's TX ranges (144–146 and 430–440 MHz) are both above 134,
so above 134 the stock receives on the BK4815 and transmits on the BK4829; below
134 the BK4829 does both.  Whether the BK4815 can radiate at all (it is a full
transceiver on its datasheet) is untested, not a finding.

## Band and path selection: the pins

The stock has **no VHF/UHF mode** and no VHF/UHF string in the image.  The RF path
is selected by two independent things: the **frequency split at 134.0 MHz** (the
transceiver select above) and a **band index** derived from the frequency.  The
per-path lines are the **BK4829's register `0x33`** (its GPIO outputs) plus a
handful of MCU pins, not a dedicated MCU band-select pin.

### The band index

`FUN_08009AEC(channel, freq)` returns a **band index** from the RAM band table at
`0x20009BB8` (the same 3 × 16-byte `Rx lo/hi, Tx lo/hi` table `FUN_08019804`
fills, matching the codeplug's at EEPROM `0x1F40`):

* entry 0 — 108–174 MHz → `0`
* entry 1 — 250 M (disabled on this codeplug) → `1`
* entry 2 — 400–520 MHz → `2`
* none → `6`

`FUN_0800E7E2(channel, 0|1)` stores it in `channel + 0x11` (0 = Rx frequency, 1 =
Tx frequency), and `FUN_080105F0(channel)` returns the **Tx** band index.  That
byte `+0x11` is what `FUN_08016DE8`/`FUN_08016CEC` then read to pick the BK4815's
`0x75` band (`1` → `0x11`, otherwise `0x0A`).

### The BK4829 `0x33` GPIO outputs

`FUN_080137D4(mask, value)` reads `0x33`; for each bit `n` set in `mask` it clears
output bit `0x40 >> n` **and** the paired bit `14 - n`, and sets output bit
`0x40 >> n` when bit `n` is also set in `value`.  So mask bit `n` *is* the K1's
`BK4819_GPIO_PIN_t` pin number and the output bit is `0x40 >> n`.  Every call site
in the image:

| output | mask | routine | args | condition |
|---|---|---|---|---|
| `0x40` | `1` | `FUN_0801BDE8` | `(3,1)` | **TX band 0 (VHF)** |
| `0x20` | `2` | `FUN_0801BDE8` | `(3,2)` | **TX band 1/2 (UHF)** |
| `0x10` | `4` | `FUN_08005638` / `FUN_08015D44` / `FUN_08004E20` | `(4,4)` / `(4,0)` | tone/CTCSS, plus a config-gated case |
| `0x08` | `8` | `FUN_0800D35C` / `FUN_0800D434` | `(8,8)` / `(8,0)` | band byte `0x20003EE4` |
| `0x04` | `0x10` | `FUN_08016CEC` / `FUN_08016DE8` | `(0x10,0x10)` / `(0x10,0)` | **BK4815 branch** (> 134 MHz) / **BK4829 branch** (<= 134 MHz) |
| `0x02` | `0x20` | `FUN_08013A70` / `FUN_08013B12` | `(0x20,0x20)` / `(0x20,0)` | **T/R (PA enable)** |
| all | `0x7f` | `FUN_08013C24` | `(0x7f,0)` | full clear |

**Only `FUN_0801BDE8` sets pins 0/1**, and it is reached only from the transmit
start (`FUN_08016A2C` → `FUN_0801830C` → `FUN_0801BDE8`, the "Pow AdjData"
routine).  It is the **TX path pair**:

* `band == 0` (VHF) → `FUN_080137D4(3, 1)` → **`0x40`**
* `band == 1 or 2` (250 M / UHF) → `FUN_080137D4(3, 2)` → **`0x20`**

so **VHF selects `0x40`, UHF selects `0x20`** — exactly the two-PA / two-filter
path select.  The receive path does *not* use a matching pair: it **clears** pins
0/1 (`FUN_080093EC`, `FUN_08016DE8`) and selects the path with the transceiver
(`0x20000303`) and the BK4815 `0x75` band.

**Pin 4 belongs to the BK4815 branch, not the BK4829's.**  `FUN_08016EE0(chan,
flag)` picks the per-transceiver receive config from the flag at `0x20000303`
(`FUN_08017340` drives it; `FUN_08006360` sets it to `1` at or below 134 MHz):
flag `0` (BK4815, > 134 MHz) runs `FUN_08016CEC` -> `FUN_080137D4(0x10, 0x10)`
(**sets** pin 4); flag `1` (BK4829, <= 134 MHz) runs `FUN_08016DE8` ->
`FUN_080137D4(0x10, 0)` (**clears** pin 4).  So the BK4829's own receive state
leaves pin 4 **clear**, and pin 4 is set only when the BK4815 is the receive
branch.

The port uses the **BK4829** above 134 MHz (its own choice — see the open
question below), so it must clear pin 4.  Measured on the radio with console
`F`: pin 4 set gave `0x67` = 199 at 145.500, clearing it gave 232 — about 16 dB
at the chip's 0.5 dB/step.  `pa_apply_chip_path()`'s AUTO and the port's
`BK4819_PickRXFilterPathBasedOnFrequency()` both clear both LNA pins now.

### The MCU pins

`FUN_08013A70(mode)` / `FUN_08013B12(mode)`:

| mode | PA1 | PA0 | chip `0x02` | use |
|---|---|---|---|---|
| 0 | 1 | 1 | set | flag-1 path TX |
| 1 | 1 | 1 | clear | flag-1 path RX |
| 2 | 1 | 0 | set | TX (normal) |
| 3 | 1 | 0 | clear | RX (normal) |

`FUN_08013B12` is the same with PA1 = 0; `config+0x34` (settings byte 15 bit 7)
selects which routine runs, and the per-VFO bytes `0x20000306` (Rx) /
`0x20000307` (Tx) override the mode to 1/0.  On this codeplug `config+0x34 = 0`
(settings byte 15 = `0x00`), so the stock uses modes 2/3: **PA1 = 1, PA0 = 0,
chip `0x02` = T/R**.

The other MCU lines the RF path touches:

* `FUN_0800D35C` (band byte `0x20003EE4`): byte `0` → `0x33` pin 3 set, **PC14
  (GPIOC `0x4000`) low**, **PB2 (GPIOB `0x04`) low**, PC13 low; byte `≠ 0` → pin 3
  clear and PC13 high.
* `FUN_080139E4` (the power-up handshake) pulses **PD0 (GPIOD `0x01`)** and
  **PC15 (GPIOC `0x8000`)**, then calls `FUN_08013A70(3)`/`(1)`.
* `FUN_08016CEC` (> 134 MHz) drives **PA14 low**; the squelch path drives PA14 too.

### Summary

* **Transceiver select**: flag `0x20000303` — `≤ 134.0 MHz` → BK4829,
  `> 134.0 MHz` → BK4815.
* **TX band path**: `0x33` pin 0 (`0x40`) for VHF, pin 1 (`0x20`) for UHF.
* **RX band path**: the transceiver select plus the BK4815 `0x75` band register;
  no separate RX VHF/UHF pin pair on the BK4829 `0x33`.
* **T/R**: `0x33` pin 5 (`0x02`), set in TX and cleared in RX; MCU PA1 = 1,
  PA0 = 0.

### Observing the pins on the stock

The stock drives the lines above from the same routines, but it has no console
that shows them.  Two ways to watch them:

* **Scope / logic analyser** on the candidate lines.  `0x40`/`0x20` are *chip*
  outputs (the BK4829's own pins), so they must be probed at the chip; the MCU
  candidates are PA0, PA1, PA13, PA14, PC13, PC14, PB2, PD0 and PC15.  Switch
  bands and watch.
* **Patch the stock** (size-preserving: edit `work/fw.bin` in place, re-encode
  with `tools/ra89r.py encode`).  A hook on the writers captures everything:
  `FUN_08011B74(port, mask, level)` is the single MCU GPIO writer, and
  `FUN_080137D4(mask, value)` / `FUN_080220A0(value, reg)` are the BK4829 writers
  (`FUN_080137D4` is the `0x33` one).  Appending `(pc, port/reg, mask, level)` to
  a RAM ring buffer logs the whole sequence.  The stock already carries a serial
  printer — `FUN_080168A4` formats through `FUN_08022FA4` on the programming
  port, gated on the byte at `0x2000007e` (its own format string is
  `RSSI_R67:%d\n` at `0x08005424`) — so a dump can reuse that path with
  `0x2000007e = 1`, or write the values into the LCD framebuffer and read them
  off the panel.

The port's console is the cheaper option for the *port's* behaviour (`F`/`m`/`Q`
read `0x33` back and print the band path), but only a stock patch shows what the
*stock* does — which is what the map above is derived from.

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

**The transmit power is a transceiver register (`0x7D`) *plus* a PA-bias PWM on
`PB14`/TIM1 channel 2** (the PWM subsection below).  The MCU's TX-side actions
are the band/path switch, that PWM, and the indicators.  The stock's TX entry,
walked from both ends:

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

* `FUN_08013A70` / `FUN_08013B12` drive `PA0`, `PA1` and the chip's `0x33` pin 5
  (output `0x02` -- **not** `0x20`; `FUN_080137D4` maps mask bit `n` to output
  `0x40 >> n`) as the **T/R / PA-enable select**.  The mode is **not the
  frequency**: `FUN_0800948C`/`FUN_08008F2C`/`FUN_080139E4` pick `FUN_08013A70` or
  `FUN_08013B12` from `config+0x34`, and the mode argument is `config+0x32` (or
  2/3/0/1 from the per-VFO bytes `0x20000306` (RX) / `0x20000307` (TX), which
  `FUN_0801B018`/`FUN_0801AFF4` read).  `config+0x32/0x34` are bits of settings
  byte 15 (`FUN_0800FE18`); on this radio byte 15 = `0x00`, so `config+0x34 = 0`
  and the stock takes `FUN_08013A70(2)` for transmit and `(3)` for receive --
  **`PA1 = 1, PA0 = 0`, the same for both bands**.  So `PA0` is not the VHF/UHF
  selector here; the TX band path is the `0x33` pins 0/1 (`0x40`/`0x20`, see "Band
  and path selection: the pins") and the RX path is the transceiver select plus the
  BK4815 `0x75`.  The port's `PA0`-high-for-UHF is an inference, and the console's
  `B` command exists to settle it;
* `PA13`/`PA14` are the red/green LED (TX = red), and `FUN_08016228` even *reads*
  `PA13` as part of its T/R decision;
* `PC13` is raised by `FUN_080177A8` in the T/R path and lowered by
  `FUN_08009C9C`, which the RF bring-up `FUN_08016788` calls;
* no timer channel is routed to a pin for a PA ramp, and the only DAC reference in
  the tree arrives through the audio/DMA path (`FUN_0800A968`, DAC at `0x40007400`).

So the port's TX work is the K1 `PrepareTransmit`/`EnableTXLink` sequence (present
already) plus **`0x7D = 0xE958`**, the band/path pins and the antenna switch, with
the level taken from the codeplug the way `FUN_0801BAF4` does.

### The PA power itself is a PWM: PB14 / TIM1 channel 2

The one part of the TX chain that is *not* a transceiver register, and the reason
a bench that set every register correctly still produced a weak, hissing,
unstable signal with no usable modulation: **the PA bias is a PWM**, on a pin that
was not in the pin map at all.

```
FUN_08016A2C   the TX start (FUN_08018AB8, 5 ms, then this)
  -> FUN_0801830C -> FUN_0801BDE8        "Pow AdjData" <- FUN_080201CC(channel)
       -> FUN_08018A88(value)            value * *(0x08018AA0) / 0xFF
          -> FUN_080167B4                clamp: if (period <= value) value = period/2
             -> FUN_0801306E(TIM1, cfg, 4)     channel 2 of TIM1
                -> FUN_0801DEEC(TIM1,cfg)      TIM1->CCR2 = the value
             -> FUN_08012F26(TIM1, 4)          BDTR |= MOE, CR1 |= CEN
```

* **the pin is `PB14`**: `FUN_080131AC` configures `GPIOB` mask `0x4000` as
  alternate function **4** (mode 2), enabling the port clock (bit 3 of
  `RCC_AHB2ENR`, the same bit `gpio.c` uses) and the timer clock (`0x800` in
  `RCC_APB2ENR` = TIM1EN).  That is why no GPIO-write sweep ever turned it up:
  a PWM pin is configured as AF, not driven through `BSRR`;
* **the timer is TIM1** (`0x40012C00`, the literal at `0x08016C4C`), and the
  channel is 2 -- `FUN_0801DEEC` writes `+0x38` (CCR2);
* **the period is 1439, i.e. a 100 kHz PWM.**  `FUN_08016C58`/`FUN_08016764`
  compute it in floating point from the boot argument `0x64` (100):
  `1.44e8 / 100 / 1000 = 1440`, minus one.  `1.44e8` is the 144 MHz APB2 clock
  (the double at `0x08016CB0`), `1000.0` the divisor at `0x08016CB8`;
* **the duty is the codeplug's power value**: `FUN_080201CC` returns 0..252
  (`(& 0x3F) << 2`), `FUN_08018A88(v)` sets `FUN_080167B4((v * ARR) / 0xFF)`, and
  `FUN_080167B4` clamps to `ARR/2` and writes it to `CCR2`.  The boot arms the
  timer with `CCR2 = 0` and the receive path (`FUN_08017340`) sets it back to 0,
  so **the PA is biased only while transmitting**.

**There is only one PWM, and it is a common bias.**  Both PWM config structs
(`0x20003720`, `0x20003760`) are initialised to TIM1 (`0x40012C00`, stored by
`FUN_08016BEC` at `0x08016BF2`), every `FUN_0801306E` call uses channel 2, and
`FUN_080131AC` configures only `PB14` as AF4 -- there is no second timer channel
or AF pin.  The **same routine**, `FUN_0801BDE8`, sets that compare and the `0x33`
band pin together:

| line | what it sets |
|---|---|
| `PB14` / TIM1_CH2 | **how hard** -- the common PA bias (`value * ARR / 255`, clamp `ARR/2`) |
| `0x33` pin 0/1 (`0x40`/`0x20`) | **which one** -- the VHF or UHF PA/filter path |

So the PWM is not per-PA: it is one bias line whose level applies to whichever
path the pin selects.  (That the `0x40`/`0x20` pin physically routes the bias to
the selected PA is the reading that fits one PWM + one path pin set together and
the two-PA board; a scope on `PB14` plus the two chip pins would confirm it.)

### The transmit configuration, validated on the radio

Voice was heard on a second receiver with exactly these values, so this is the
list the port keeps:

| what | value | why |
|---|---|---|
| `0x7D` | `0xE958` | the stock's own power/bias for this codeplug (`FUN_0801BAF4`, level 3) |
| `0x36` | `0x8822` | **the amplifier enabler**: PA-CTL (bit 7) + bias `0x88` + gain.  The stock's own TX path never writes `0x36`; the K1 sets it in `BK4819_SetupPowerAmplifier`, and our imported `BK4819_TxOn_Beep` wrote it to **0** -- which is why the carrier existed and was never amplified |
| `0x33` | `0x0020` | the port's measured value -- but note it is the **UHF band path pin** (`0x20`), not the T/R pin.  The stock's TX `0x33` is the band pin (`0x40` VHF / `0x20` UHF, `FUN_0801BDE8`) **plus** the T/R pin `0x02` (`FUN_08013A70(2)`), i.e. `0x42` / `0x22`; see "Band and path selection: the pins" |
| `PA1`/`PA0` | `1` / `0` | the stock's transmit band path |
| `PB14`/TIM1_CH2 | ARR 1439, compare 128 | the PA bias PWM (100 kHz) |
| `0x30` | `0xC1FE` | mic ADC (bit 2) + TX DSP (bit 1) + PA gain (bit 3) |
| `0x37` | `0x9D1F` | the stock's TX value |
| `0x47` | `0x6042` | AF muted -- the AF DAC is not the modulation source |
| `0x50` | `0x3B20` | **the TX unmute.**  The stock writes this; our imported `ExitTxMute` sends `0x3B18` (the value from the K1's `bk4829.c`), and `BK4819_EnterDTMF_TX` leaves `0xBB18` -- a muted TX audio path is a carrier that carries nothing |
| `0x40` | `0x3700` | microphone gain `0x70`; the field is a **byte** in bits 11:4 (`FUN_0801C3A8`: `(old & 0xE000) | 0x1000 | gain << 4`), not the nibble it first looked like |
| BK4815 `0x0C` | `0x0203` | the T/R path's other-branch state |
| `0x38`/`0x39` | the channel in 10 Hz | |

**Order matters for `0x36`.**  `BK4819_PrepareTransmit()` ends in
`BK4819_TxOn_Beep()`, which writes `0x36 = 0`; the PA enable must therefore be
written **after** it, or the whole transmission is unamplified (the chip's own
low-level carrier only, audible to a nearby receiver but not to a power meter).
The bench did this correctly; the extraction into `driver/tx.c` reversed the two
and shipped the PA disabled.  `tools/test_rf.c` now asserts the final `0x36`
write (`test_tx_order`), and `driver/tx.c` calls `pa_tx_enable()` after
`BK4819_PrepareTransmit()`.

The chip's own DTMF tone (`BK4819_EnterDTMF_TX` -> `EnableTXLink` ->
`BK4819_PlayDTMF`) is audible on a second receiver through the same path, and is
what settled the tone-versus-carrier question when the microphone was silent.

With that in place the CPU-side TX picture is complete: chip registers
(`0x30`/`0x37`/`0x47`/`0x50`/`0x7D`), the band/path pins (`PA1 = 1, PA0 = 0`, chip
PA_ENABLE on -- `FUN_0800948C(1)` -> `FUN_0801B018` -> `FUN_08013A70(2)`), the
second transceiver's state and the PB14 power PWM.  What is still open is only the
*level*: which duty the codeplug asks for on this radio, and the microphone gain
(`FUN_0801C3A8`, `0x40`) and AF level (`0x48`/`0x6C`) the stock fills from its own
RAM.

**The port's power ladder (the K1 wired to this radio).**  The K1's
`TXP_CalculatedSetting` (0..255, from the channel's `OUTPUT_POWER` via
`RADIO_ConfigureSquelchAndOutputPower`) now drives **both** `0x36`
(`bias << 8 | PA-CTL | gain`, the K1's `SetupPowerAmplifier`, with the band's gain
`0x08` VHF / `0x22` UHF) and the PB14 compare (`power * ARR / 255`, the stock's
arithmetic) -- `pa_tx_enable(power)` does both, and `tx_start()` takes the
setting.  The K1 reads its per-band low/mid/high calibration from EEPROM
`0x100D0`, which is **erased (`0xFF`)** on this radio, so
`SETTINGS_GetTxCalibration()` supplies the K1's values for the nearest bands as a
**provisional** stand-in; the per-level duty for each band still needs a measured
sweep, and the provisional table is where its results go.

## Open

1. **Which chip does what.**  Answered as far as static reading goes: the BK4829
   carries the filter (`0x33`), the RSSI/metering (`0x63`/`0x65`/`0x67`/`0x99`),
   the squelch ramp (`0x13`), the T/R set (`0x47`/`0x30`/`0x31`) and the **whole
   TX setup** (`0x38`/`0x39`/`0x30`/`0x50`/`0x7D`; `FUN_08017306` selects it for
   every TX); the BK4815 carries the band select (`0x75`), the T/R state (`0x0c`),
   the per-mode config above 134 MHz and its own `0x43`/`0x44` RSSI/SNR.  Still
   open: why the crossover is 134 MHz, what the state byte `+0x75` means on its
   own, and **whether the BK4815 can radiate** -- its TX config is dead code, but
   the part is a transceiver, so this needs the radio, not the image.
2. The power-on/off and sleep handling, and whether anything else gates the RF
   rails — the decompiler is now available, but these paths have not been walked.
3. The per-channel/per-band routines that feed the T/R registers.  The TX power
   setting is answered above: `0x7D`, computed by `FUN_0801BAF4` from the codeplug
   level, plus the `PB14`/TIM1_CH2 bias PWM (`value * ARR / 255`); still open is
   which of `0x7D`, `0x30` bit 3 and the PWM actually enables/limits the PA on the
   radio.
4. `FUN_0800A968`, `FUN_0801533C` and `FUN_0801537C` (the `FUN_08007F90` callees)
   are not identified.
5. Which string `FUN_08015D14(0x0b)` actually renders, and what the byte at
   `0x20009F28 + 0x1c` selects (the language table it indexes is built in RAM).
