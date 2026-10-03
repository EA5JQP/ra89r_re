# RA89R battery

**Status: merged into `develop` and wired to the K1.**
`App/driver/battery.c` reads **ADC channel 9 (PB1)** -- the sixth channel the stock scans
and the one the keypad ladders do not use -- via `keypad_aux_raw()`.  It reports the raw
sample and the stock's 0..255 level, and `board.c`'s `BOARD_ADC_GetBatteryInfo()` feeds the
K1's `BATTERY_GetReadings()` with the pack voltage in 10 mV.  Console command `u`.

## The "two-wire gauge" was the BK1080 FM receiver

**Everything below this section is the earlier investigation, and its central premise was
wrong.**  The bus on `PC14`/`PB2` is not a battery gauge:

- The BK1080 datasheet (`docs/BK1080.pdf`, section 6.2.2) gives its I2C **device ID as
  `0x80`** and its control word as `(7-bit register << 1) | R/W` -- byte for byte what
  `FUN_08007034`/`FUN_08007158` emit.  The stock's "gauge" functions are the BK1080's I2C
  driver.
- The register they read, `0x0B`, is `REG11` = **RSSI/status** (`RSSI<7:0>`, in dBuV), not
  a voltage.  The `875/760/640` "battery offsets" were a misread of an RSSI field.
- There is **no gauge IC on the board** (owner's teardown): BK4815, BK4829, BK1080,
  TDA2822, LM2904B and a Jieli BT chip only.  The BK4815/BK4829 "ADC" is the RX signal ADC
  (`REG67/68` = RSSI/SNR, `REG69` = AFC) and `VBATD` is a supply pin, not an ADC input; the
  BK1080 has no voltage register at all.

That is why every attempt to drive it as a gauge failed: there was nothing to drive.  The
sections that follow are kept as the record of that dead end.

## Where the pack really is

The stock scans six ADC channels (`FUN_08004E58`): 2, 3, 6, 7, 8 -- the keypad ladders --
and **9 = PB1**, which the keypad does not use.  `FUN_0800E514` / `FUN_08007664` read that
sixth channel as a 0..255 level, and it is the battery sense.  The keypad driver already
samples it (its DMA round is `{ 2, 3, 6, 7, 8, 9 }`), so `keypad_aux_raw()` exposes it and
`battery.c` reports it.

The millivolt figure is **calibrated on this radio**: 3413 counts read 8.32 V on a
multimeter, so `battery_mv()` uses **2.438 mV/count** (full scale 4095 -> ~10 V, the
divider's top).  `BATTERY_MV_NUM` in `battery.c` is the one constant to change if that
ever moves.

### K1 integration

`board.c`'s `BOARD_ADC_GetBatteryInfo()` -- the K1's battery hook, previously a fixed
placeholder -- now returns `battery_mv()/10`: the pack in **10 mV**, which is what
`helper/battery.c`'s `gBatteryVoltageAverage = (value * 760) / gBatteryCalibration[3]`
expects.  With the K1's default calibration (760) that is the measured pack directly, and
the K1's own battery-calibration menu can still trim it.  There is no charge-current sense
on this board, so the reported current is zero.  Console `u` prints the same value.


## Enable / power, and the CMSIS (both checked, 2026-10)

**No hidden power/enable pin.**  Tracing every GPIO around the gauge:

- `FUN_0800D434` enables the GPIOB/GPIOC clocks (`RCC+0x34 |= 8`, `|= 0x10`) and sets
  **PB2 and PC14 push-pull output, high speed, no pull**.
- `FUN_080138FC` enables the GPIOD clock (`RCC+0x34 |= 0x20`) and sets **PD0 output**.
- `FUN_0800D1F8` enables the gauge **over the bus**: reg 3 clear bit 7, reg 2 set bits 0-2
  (`FUN_080069A6` / `FUN_08006952`).
- `PD0` is the gauge's **active-low reset**: `FUN_08006850` / `FUN_0801D69C` pulse it
  low->high, `FUN_08008064` holds it low, `FUN_080066CC` reads it.

Nothing in the boot sets a rail for the gauge, so an unpowered chip is not something the
stock gates with a pin we are missing.

**The vendor CMSIS matches the silicon.**  The stock's own GPIO HAL `FUN_0801199C` writes
`MODER/OTYPER/OSPEEDR/PUPDR` at offsets `0/4/8/0xC` and uses `BSRR`/`BRR` -- the vendor
`GPIO_TypeDef` layout.  Port bases `0x48000000/0x48000400/0x48000800/0x48000C00`, `RCC` at
`0x40021000`, `AHB2ENR` at `0x34` with `IOPBEN=bit3`, `IOPCEN=bit4`, `IOPDEN=bit5` all match
the stock's writes.  The compiled driver resolves to exactly those addresses (`battery.o`,
`gpio.o` checked with `objdump`).  And the same `gpio.c` drives the LCD, UART, backlight and
keypad, which work on the radio -- so the register map is not the fault.

**PD0 was the untested line.**  `battery_init()` self-tested PC14 and PB2 but never PD0,
even though the stock asserts the gauge's reset low at boot and only releases it once the
supply has settled (`FUN_08008064` -> `FUN_0801D69C`).  It now self-tests PD0 (reported as
`reset ok/STUCK`) and asserts it low for 10 ms, then releases it high, before the first
transaction.  On the radio it reports `reset ok` -- so all three lines are ours -- and the
gauge still does not ACK.

**The CMSIS claim is now falsifiable at runtime.**  The `u` command dumps the raw
`MODER/OTYPER/OSPEEDR/PUPDR/AFR/IDR` of GPIOC, GPIOB and GPIOD, plus `RCC CR/BDCR/AHB2ENR`.
If the silicon reads back what the driver wrote (PC14 output at MODER bits 28-29, PB2 at
4-5, PD0 at 0-1), the vendor CMSIS matches the register map at runtime and is not the
fault.  The dump also shows the oscillator enables, which matter here: the datasheet gives
**PC14 = OSC32_IN** and **PD0 = OSC_IN**, so an enabled LSE/HSE would own those pins.

## The chip

A companion gauge/charger on a two-wire bus.

| line | pin | direction |
|---|---|---|
| clock | `PC14` (`GPIOC`, mask `0x4000`) | output, idle low |
| data | `PB2` (`GPIOB`, mask `4`) | **bidirectional** -- output to send, released to read |
| reset / handshake | `PD0` (`GPIOD`, mask `1`) | output |
| **power / enable** | **`PC15` (`GPIOC`, mask `0x8000`)** | output, **driven high** |

The power/enable line was the missing piece.  The stock configures `PC15` as an output and
drives it **high** in `FUN_080139E4` -- reached from the gauge init `FUN_08016788`, which
also configures the bus (`FUN_0800D434`) -- before it resets the gauge on `PD0` and talks to
it.  `PC15` is `OSC32_OUT` and is unused elsewhere, so the first driver left it analog (high
impedance): the companion was never powered, and no amount of correct framing, bus speed,
reset state or configuration could get an ACK.  `battery_init()` now configures `PC15` as an
output and drives it high before the first transaction.

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
  A write is the same order: `FUN_08007284` stores the value's high byte first
  (`0800728a  asrs r0,r4,#8 ; strb r0,[sp,#0] ; uxtb r0,r4 ; strb r0,[sp,#1]`),
  then `FUN_08007240` sends the two bytes.  An earlier note here claimed the write
  was low-byte-first; the assembly says otherwise.
* **A register write opens with a start.**  `FUN_08007240` calls `FUN_08007034`
  first, and the bus start (`FUN_08006EF0`) happens *before* the `0x80` device byte.
  The read path always had it; the driver's write path did not, so its device byte
  and every configuration byte after it were clocked out unframed and the gauge
  never latched the write.  Fixed in `App/driver/battery.c` (`bus_write_reg`).
* **The master drives the acknowledge bits** -- low after the first byte of a word,
  and **high** on the last word of a read.

## Registers, and what the stock polls

`FUN_08017BB4` is the poll routine (called by the main loop, but gated by battery state
bytes at offsets `+1` and `+5`):

1. reads registers `11`, `5`, `10`, `7`, `2` -- register 11 first; this poll routine
   itself has no configuration write before the read;
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

`FUN_08007124` writes a **68-byte block to register 0**, then register `0x32` twice.
Its contents are at RAM `0x20000088`; the startup scatter table at `0x08027728` points
to compressed initial data at `0x08027748`, expanded by `FUN_0800483A` to RAM starting
at `0x20000000`.  The battery-type path decides whether to send the block; it does not
build the bytes.  The chain that reaches it:

```
FUN_0800D334(arg)   -- "set battery type": state[8] = arg, then FUN_0801935C(...)
  -> FUN_0800D35C   -- battery type 0 parks the bus lines; non-zero runs the config
       -> FUN_08007124   -- the 68-byte block + registers 5 and 3
       -> FUN_08006F9C   -- the write-back above
```

The gauge setup is conditional on the battery-type state (`state[8]`); `FUN_0800F55C`
sets that state from settings.  `FUN_08007124` is on the setup path, not inside the poll
routine.  Do not infer from the static call graph alone that every stock boot sends the
block or that an unconfigured IC must ACK.

## Troubleshooting log

Symptom: every register reads back `0xFFFF` with no acknowledge -- both bytes `0xFF`,
i.e. the data line sitting high with nothing pulling it down.

Eliminated, with the evidence:

| ruled out | evidence |
|---|---|
| wrong pins | all six port/pin constants read out of the image (table above) |
| the LSE owning PC14 | `BDCR` reports it off, and the `clock.c` note about the bootloader's PLL aside, the *stock* drives PC14 without ever configuring the LSE |
| the timing / bus speed | the stock's delay is a cycle count, so it was *swept*: five delay scalings plus a **zero-delay** entry which measures **~250 kHz** on the console -- far above the stock's ~57 kHz -- and the writes were still unacknowledged |
| the pins not being ours | a self-test drives each pin low then high and reads it back; both pass, and `lse off, clk ok, data ok` is what the boot log reports |
| the data pin's idle direction | a real bug, fixed (see above) -- it did not change the outcome |
| missing write start | found by comparing `bus_write_reg` with `FUN_08007240 -> FUN_08007034 -> FUN_08006EF0`; fixed in the driver, but the subsequent full wake still got 0/8 stage ACKs |
| the full bring-up | after the start fix, `u` tried the full register-0/config/calibration sequence over the wake candidates; 0/8 stages acknowledged |
| PD0 / reset | the stock's boot drives PD0 low (`FUN_0801D69C`) and the poll then releases it high, which is the state this firmware leaves; it is not the difference |
| an ADC channel | there is no battery ADC: the stock scans exactly channels 2, 3, 6, 7, 8, 9 (the keypad) plus 14 (PC4) on the other board variant, and the datasheet's pin map accounts for every ADC-capable pin |
| the ADC feeding the display | the sixth channel (PB1) is a *level*, not a voltage: `FUN_0800E514` averages it to 0-63 and `FUN_08007664` to a 0-255 bar |

The stock voltage display is sourced from this gauge.  For the port, the no-ACK result
does not distinguish a GPIO configuration/drive problem from a disconnected, unpowered,
or otherwise silent IC; the self-test is not a substitute for observing the bus.

**What is left, in the order worth trying:**

1. **Capture the bus** on PC14 and PB2 (and PD0 if possible), while stock displays
   voltage and while `u` runs.  Confirm clock/data transitions and whether the host
   releases PB2 for ACK at the right time.  Compare frame-for-frame before changing
   more timing or reset hypotheses.
2. If the waveforms match but stock alone gets ACKs, investigate the board's power,
   charger-enable, and PD0 handshake state; those are not established by this static
   analysis.

**Do not call the electrical cause proven** until the bus has been measured.  The stock
does read this chip on this radio, but the port's current evidence stops at GPIO
self-test and no-ACK bus attempts.
