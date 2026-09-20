# RA89R battery gauge

**Status: the protocol is decoded and implemented; it does not work yet.**
`App/driver/battery.c` on branch `driver/battery` (unmerged) is the implementation,
console command `u`.  The stock firmware reads this radio's pack fine (8.1 V), so
this is not a hardware question -- it is one of the few remaining differences between
this firmware and the stock, and the troubleshooting log below records exactly which
differences have already been eliminated.

## The chip

A companion gauge/charger on a two-wire bus.

| line | pin | direction |
|---|---|---|
| clock | `PC14` (`GPIOC`, mask `0x4000`) | output, idle low |
| data | `PB2` (`GPIOB`, mask `4`) | **bidirectional** -- output to send, released to read |
| reset / handshake | `PD0` (`GPIOD`, mask `1`) | output |

Every port/pin pair is *verified in the stock image*, not assumed: each of
`FUN_08006E78` (read byte), `FUN_0800705C` (write byte), `FUN_08006EF0` (start),
`FUN_08006F4C` (stop) and `FUN_08007158` (read register) resolves to `GPIOC/0x4000`
for the clock and `GPIOB/4` for the data, and `FUN_0800D138` configures `GPIOB/4`.

## Protocol, taken from the stock's own bit-bang

| address | what it does |
|---|---|
| `0x08006EF0` | **start**: clock low, data high, clock high, then data **low** while the clock is high |
| `0x08006F4C` | **stop**: clock low, data low, clock high, then data **high** |
| `0x0800705C` | write a byte, MSB first; then release the line and poll it (up to 250 times) for the chip pulling it low -- its acknowledge |
| `0x08006E78` | read a byte, MSB first, sampled while the clock is high |
| `0x08007240(reg, len, buf)` | **write** a register: start, `0x80`, `(reg << 1) \| 0`, `len` bytes, stop, clock low |
| `0x08007158(reg, len, buf)` | **read** a register: start, `0x80`, `(reg << 1) \| 1`, 16-bit words, stop, clock low |
| `0x0800D138(dir)` | data pin direction: output (1) or released (0) |
| `0x08007284(reg, value)` | 16-bit write: packs the value and calls `FUN_08007240` with `len = 2` (bytes) |
| `0x0802422A(n)` | the delay helper: a *fixed loop* of `(n+1)` x 21 iterations -- a cycle count, not a time |

Three details that cost real time here and are easy to get wrong again:

* **The data pin idles as an output, driven high.**  The stock's byte write ends by
  restoring the output direction (`FUN_0800D138(1)`) and its stop condition ends on
  data high; only a *read* releases the line.  Leaving it an input keeps the bus
  silent -- the start condition and the first byte go nowhere at all.
* **A read word is two bytes with the high half first**: `(first & 3) << 8 | second`.
  A write is the opposite order: low byte first, which is how `FUN_08007284` packs it.
* **The master drives the acknowledge bits** -- low after the first byte of a word,
  and **high** on the last word of a read.

## Registers, and what the stock polls

`FUN_08017BB4` is the whole poll:

1. reads registers `11`, `5`, `10`, `7`, `2` -- register 11 **first, with no
   configuration write before it**, so a fresh chip is expected to answer immediately;
2. read-modify-writes register `2` (sets bits 0, 1, 2 or clears bit 0) and register `3`
   (clears bits 0-6) to clear the chip's latched status;
3. clamps: while charging, a floor of 7,600,000 uV;
4. caches the value (`state[+0xC] = FUN_08006400(v)`) and calls `FUN_08006F9C`, which
   *writes back* the measured voltage.

### The voltage

```
raw    = reg11: ((first byte) & 3) << 8 | (second byte)      -- 10 bits
gain   = reg5:  (second byte) >> 6
offset = gain == 0 ? 875 : (gain == 1 or gain == 2) ? 760 : 640
pack   = (raw + offset) * 10000 uV                           -- 10 mV per count
```

The two clamp constants in `FUN_08017BB4` are `DAT_08017c34 = 0x0073F780`
(7,600,000 uV) and `DAT_08017c38 = 0x0061A800` (6,400,000 uV), i.e. a **~7.6 V nominal
pack** -- which is also what the `875/760/640` offsets mean: the divider's zero point,
8.75 / 7.60 / 6.40 V, one per battery variant.  The CPS calls the same thing
"7.4 V reference voltage", and its key list carries a `Side1/Side2`-style entry for it.

### The write-back (`FUN_08006F9C`)

After every poll the stock writes the measured voltage back:

```
count  = value_uv / 10000
if count < 760: offset, sel = 640, 0xC0 | 0x1F   else: 760, 0x40 | 0x1F
n      = count - offset
write16(5, (10 << 8) | sel)
write16(3, ((n >> 8) & 3) << 8 | (n & 0xFF))
write16(3, the same | 0x8000)
```

For the 8.1 V this radio's stock firmware reports that is `reg5 = 0x0A5F`,
`reg3 = 0x0032` then `0x8032`.  `driver/battery.c` replays exactly this as a bring-up
step and reports how many of the three writes were acknowledged.

### The configuration block

`FUN_08007124` writes a **68-byte block to register 0**, then register 5 and register 3
twice.  Its contents are in RAM at `0x20000088` and are filled by the battery-type
setup, not by a table that could be lifted directly.  The chain that reaches it:

```
FUN_0800D334(arg)   -- "set battery type": state[8] = arg, then FUN_0801935C(...)
  -> FUN_0800D35C   -- battery type 0 parks the bus lines; non-zero runs the config
       -> FUN_08007124   -- the 68-byte block + registers 5 and 3
       -> FUN_08006F9C   -- the write-back above
```

The battery type itself comes from `*(uint16_t *)0x08025FF0`, which is `0x2000` in the
stock image.  `FUN_08007124`'s only caller chain is not on the poll path, so the
*stock's normal reads do not need it* -- but it is the leading candidate for why this
firmware, which never runs it, gets no answer at all.

## Troubleshooting log

Symptom in every attempt: every register reads back `0x03FF` with no acknowledge --
both bytes `0xFF`, i.e. the data line sitting high with nothing pulling it down, at
every speed.  That is a chip that never hears a command, not a framing mistake.

Eliminated, with the evidence:

| ruled out | evidence |
|---|---|
| wrong pins | all six port/pin constants read out of the image (table above) |
| the LSE owning PC14 | `BDCR` reports it off, and the `clock.c` note about the bootloader's PLL aside, the *stock* drives PC14 without ever configuring the LSE |
| the timing / bus speed | the stock's delay is a cycle count, so it was *swept*: five delay scalings plus a **zero-delay** entry which measures **~250 kHz** on the console -- far above the stock's ~57 kHz -- and the writes were still unacknowledged |
| the pins not being ours | a self-test drives each pin low then high and reads it back; both pass, and `lse off, clk ok, data ok` is what the boot log reports |
| the data pin's idle direction | a real bug, fixed (see above) -- it did not change the outcome |
| the write path | mirrored from `FUN_08007240` and offered as a bring-up: `acked 0/3` |
| PD0 / reset | the stock's boot drives PD0 low (`FUN_0801D69C`) and the poll then releases it high, which is the state this firmware leaves; it is not the difference |
| an ADC channel | there is no battery ADC: the stock scans exactly channels 2, 3, 6, 7, 8, 9 (the keypad) plus 14 (PC4) on the other board variant, and the datasheet's pin map accounts for every ADC-capable pin |
| the ADC feeding the display | the sixth channel (PB1) is a *level*, not a voltage: `FUN_0800E514` averages it to 0-63 and `FUN_08007664` to a 0-255 bar |

So the on-screen voltage really is this gauge, and the fault is not on the MCU side of
the bus as far as any of that can tell.

**What is left, in the order worth trying:**

1. **The 68-byte configuration** -- trace what fills `0x20000088` in the stock and
   replay it before the first read.  It is the one thing the stock does that this
   firmware does not.
2. **A scope on PC14 and PB2**, while the stock firmware runs and while this one does,
   comparing the two.  Everything else is inference; that would be measurement.
3. Ask whether the *charger* side of the chip gates its bus (the stock's boot also
   writes `state[5]` from a value read via `FUN_080246CC`, and PD0 behaves like a
   handshake line rather than a plain reset).

**Do not re-suspect the hardware**: the stock reads this chip on this radio, at 8.1 V,
with the same pins.
