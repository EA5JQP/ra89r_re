# RA89R MCU pin inventory

Every PY32F403 GPIO line the stock application touches, and — the point of the
file — the lines it leaves alone, so a new peripheral or feature can pick one
without colliding with the radio.  It is a **static** inventory of the current
decode (`work/stock_v49_raw.bin`, base `0x08004000`), cross-checked against the
per-feature docs; it is deliberately *not* a hardware-validation claim.  "Free"
here means "not referenced by the stock image"; the package is known too (LQFP48
C1xT, §"The package"), so the two lists can be combined: only **`PB11` and
`PD1`** are both bonded and unused.  Whether they are *routed* is a separate
question — see §"What the radio proves" and "Open points".

The pin semantics themselves live in the feature docs
([`ra89r_lcd.md`](ra89r_lcd.md), [`ra89r_keypad.md`](ra89r_keypad.md),
[`ra89r_led.md`](ra89r_led.md), [`ra89r_rfpath.md`](ra89r_rfpath.md),
[`ra89r_bk1080.md`](ra89r_bk1080.md), [`ra89r_eeprom.md`](ra89r_eeprom.md),
[`ra89r_beeper.md`](ra89r_beeper.md), [`ra89r_battery.md`](ra89r_battery.md));
this file is the board-wide index of which pin is which direction.

## Bottom line

The MCU is the **LQFP48 `PY32F403C1xT`** bond-out (12 pins per side — see
§"The package" below).  It bonds **37 GPIOs**: all of GPIOA, all of GPIOB,
`PC13`–`PC15` and `PD0`/`PD1`.  No other port pins exist on it — `PC0`–`PC12`
and the whole of GPIOE are *not bonded*, so "free in the image" for those means
"there is no pad", not "there is a spare pin".

The stock **uses 35** of those 37 GPIOs; the only two it configures but never
uses are:

| pin | state in the stock | note |
|---|---|---|
| `PD1` | input, no pull, **never read** | the `OSC_OUT` pad, freed because the part runs from HSI |
| `PB11` | USART3 alternate-function init only, **never read or driven** | see §GPIOB |

So the board has effectively **no spare pins**: anything new has to share a line
or replace a function.  Neither spare pin has an ADC channel or a DAC output
(§"Analog capability"), so there is no spare *analog* pin either.  `BOOT0`
(pin 44) is a boot strap, not a GPIO.

## How the stock talks to its pins

Almost everything goes through four helpers (the same finding the keypad
write-up relies on), which is what makes a static sweep tractable.  The one
exception is the bit-bang bus code on GPIOB, which reads and writes the
registers directly (§"What was ruled out"):

| helper | address | signature | role |
|---|---|---|---|
| `GPIO_WriteBit` | `0x08011B74` | `(port, mask, value)` | `value ? port->BSRR = mask : port->BRR = mask` |
| `GPIO_ReadInputDataBit` | `0x08011B64` | `(port, mask)` | `port->IDR & mask` |
| `GPIO_Init` | `0x0801199C` | `(port, descriptor*)` | per-pin config from a descriptor |
| `AF_Init` | `0x08011884` | `(port, mask)` | alternate-function-only init (mask form) |

`GPIO_Init` walks the 16-pin mask in `descriptor[0]` and, per set bit, programs
(with `descriptor[+4]` = mode, low two bits = `MODER`: 0 input, 1 output,
2 alternate-function, 3 analog):

| descriptor offset | field |
|---|---|
| `+0x00` | pin mask |
| `+0x04` | mode (`& 3` → `MODER`) plus flags; bit 4 → `OTYPER`, `0x10000`/`0x20000`/`0x100000`/`0x200000` → initial `ODR`, `0x10000000` → route an EXTI line (enables the `SYSCFG` clock and writes `EXTICR`) |
| `+0x08` | pull (`PUPDR`, 2 bits/pin) |
| `+0x0C` | speed (`OSPEEDR`, 2 bits/pin) |
| `+0x10` | alternate function (`AFRL`/`AFRH`, 4 bits/pin) |

One cluster looked like it bypassed the helpers: `0x08020260`/`0x08020348`/
`0x08020368` clear `0xC0`/`0x120`/bit-0 fields of `[r0+0xc]` and `[r0+0x14]` and
have no static caller.  Those are **not GPIO**: the neighbouring `FUN_0802037C`
computes a USART **BRR** divisor against `USART1` (`0x40013800`), and the state
bytes at `+0x45`/`+0x46` take `0x21`/`0x22` (the HAL's `BUSY_TX`/`BUSY_RX`), so
this is the vendor HAL's UART module — dead code in this image, not a hidden
GPIO actor.  The only direct (non-helper) register access that *is* live is on
GPIOB, in the bit-bang bus code (`0x0801DDxx`) and the `PB14`/TIM1 area
(`0x080132xx`), which read/write `PUPDR`/`IDR`/`ODR`/`MODER`/`AFRH`.

## GPIOA — all sixteen used

| pin | role | evidence |
|---|---|---|
| `PA0`,`PA1` | band-path select (stock leaves `PA1`=1, `PA0`=0 in both TX and RX) | `GPIO_Init` mask `0x3` @ `0x0801393A`, `0x08013CB2`; 7 / 10 `WriteBit` sites |
| `PA2`,`PA3` | keypad ADC ladder (ch 2/3) at run time; *also* muxed to USART2 AF2 in the debug/BT path | analog `GPIO_Init` mask `0xCC` @ `0x08010D4A`; AF2 @ `0x0800734C`/`0x0800735E`; `PA2` read @ `0x08014AE8` |
| `PA4` | `DAC_OUT1` (beep) | `GPIO_Init` mask `0x10` @ `0x08009DAE`; DAC mask `0x30` @ `0x0800A930` |
| `PA5` | `DAC_OUT2` = backlight | `GPIO_Init` mask `0x30` @ `0x0800A930`; [`ra89r_led.md`](ra89r_led.md) |
| `PA6`,`PA7` | keypad ADC ladder (ch 6/7) | analog `GPIO_Init` mask `0xCC` @ `0x08010D4A` |
| `PA8`–`PA11` | LCD `SCLK`/`RST`/`DC`/`CS` | `GPIO_Init` mask `0xF00` @ `0x080242D8`; [`ra89r_lcd.md`](ra89r_lcd.md) |
| `PA12` | RF bus clock | `GPIO_Init` mask `0x1000` @ `0x0801374A`, `0x08013876`; 11 `WriteBit` sites |
| `PA13`,`PA14` | status LED (`PA13` red / `PA14` green), the `SWDIO`/`SWCLK` pads | `GPIO_Init` mask `0x6000` @ `0x08013CCE` (boot init `0x08013C74`); [`ra89r_led.md`](ra89r_led.md) |
| `PA15` | SPI-NOR chip select, also SPI1 `NSS` AF | `GPIO_Init` mask `0x8000` @ `0x0801D246`; `AF_Init` `0x8000` @ `0x08012ACE`; [`ra89r_eeprom.md`](ra89r_eeprom.md) |

`PA2`/`PA3` are a dual use worth recording: the USART2 AF2 init (`0x0800734C`/
`0x0800735E`) coexists with the keypad's analog config (`0x08010D4A`), and the
radio's ladder pins must stay analog and undriven at run time — so the USART2
path is a debug/test bring-up, not the live function.

## GPIOB — all sixteen used

| pin | role | evidence |
|---|---|---|
| `PB0` | keypad ADC ladder (ch 8) | analog `GPIO_Init` mask `0x3` @ `0x08010D5C` |
| `PB1` | battery ADC ch 9 | analog `GPIO_Init` mask `0x3` @ `0x08010D5C`; [`ra89r_battery.md`](ra89r_battery.md) |
| `PB2` | BK1080 FM SDA | `GPIO_Init` mask `0x4` @ `0x0800D164`, `0x0800D470`; 13 `WriteBit` + reads |
| `PB3`–`PB5` | SPI-NOR `SCK`/`MISO`/`MOSI` | `GPIO_Init` mask `0x8`/`0x30` @ `0x08012BAA`/`0x08012BC0`; `AF_Init` mask `0x38` |
| `PB6`,`PB7` | USART1 (programming / console jack) | `GPIO_Init` mask `0xC0` @ `0x0801332E` |
| `PB8` | BK4829 chip select | `GPIO_Init` mask `0x1100` @ `0x080138AC`; 2 `WriteBit` (`0x100`); [`ra89r_bk4829.md`](ra89r_bk4829.md) |
| `PB9` | PTT2 / bootloader gate | `GPIO_Init` mask `0x200` @ `0x08014B3C`; read @ `0x080169F8` |
| `PB10` | PTT1 read (second PTT) | `ReadInputDataBit` mask `0x400` @ `0x0802192E` |
| `PB11` | **only the USART3 bring-up** (AF2 = `USART3_TX`/`RX`) | `GPIO_Init` mask `0xC00` @ `0x080134B8` |
| `PB12` | RF bus SDA | `GPIO_Init` mask `0x1000` @ `0x0801DCE4`, `0x0801DD1C`; 12 `WriteBit` + read @ `0x0801800E` |
| `PB13` | BK4815 chip select | `GPIO_Init` mask `0x3000` @ `0x08013780`; 2 `WriteBit` (`0x2000`); [`ra89r_bk4815.md`](ra89r_bk4815.md) |
| `PB14` | PA bias PWM (`TIM1_CH2`, AF4) | `GPIO_Init` mask `0x4000` @ `0x08013208`; [`ra89r_rfpath.md`](ra89r_rfpath.md) |
| `PB15` | LCD SDA | `GPIO_Init` mask `0x8000` @ `0x0802430E` |

**The `PB11` caveat.**  `PB11` is one of the two spare lines on the package (the
other is `PD1`).  The only code that names it is the USART3 init (`GPIO_Init`
mask `0xC00` = `PB10`+`PB11`, mode AF, AF2 = `USART3_TX`/`USART3_RX`), i.e. on
this part `PB11` is configured as the **RX input**, not an output.  `PB10` is
read at run time as a PTT input (`0x0802192E`), exactly as `PA2`/`PA3` are really
keypad lines despite their USART2 AF config, so the USART3 block looks like the
same debug bring-up — but whether it is ever reached with the USART3 handle is
not proven.  Confirm with a breakpoint on `0x080134B8` before reusing the pin.

## GPIOC — only `PC13`/`PC14`/`PC15` used

| pin | role | evidence |
|---|---|---|
| `PC13` | key/PTT read, and driven low by the key dispatcher; amplifier-enable candidate (`AUDIO_PATH_PIN`) | `GPIO_Init` mask `0xA000` @ `0x08013D2E`; reads mask `0x2000` in the key path (`0x08013F3C`, `0x080218F6`); writes `0x2000` (14 sites) |
| `PC14` | BK1080 FM SCL | `GPIO_Init` mask `0x4000` @ `0x0800D4A6`; 13 `WriteBit` sites |
| `PC15` | RF/band bring-up line: raised once by `FUN_080139E4` (`FUN_08011B74(GPIOC, 0x8000)`), never lowered; destination unknown | `GPIO_Init` mask `0x8000` @ `0x08013970`, `0xA000` @ `0x08013D2E`; write @ `0x080139F6` |
| **`PC0`–`PC12`** | not bonded on the 48-pin package | no reference anywhere in the image |

`PC14`/`PC15` are also the `OSC32_IN`/`OSC32_OUT` pads: the stock runs without
a 32 kHz crystal and repurposes them, the same way it gives up `PA13`/`PA14`
(SWD) and `PD0`/`PD1` (`OSC_IN`/`OSC_OUT`) for GPIO.

`PC4` is not bonded on the 48-pin part (it appears only on the 64/100-pin
bond-outs), so it cannot be probed or used here.  The ADC scan's alternate
variant (`FUN_0801BE90(1)` → `FUN_08004E58`, which reads channel 14 = `PC4`
instead of channel 2 = `PA2`) must therefore belong to another board or package
— worth remembering only if the RA89G turns out to use a 64-pin part.

## GPIOD — `PD0` and `PD1` used

| pin | role | evidence |
|---|---|---|
| `PD0` | output (boot init); also read as a strap | `GPIO_Init` mask `0x1` @ `0x080139A4`, `0x08013D62`; 7 `WriteBit` sites; read @ `0x080066E2` |
| `PD1` | **input, no pull, never read — spare** | `GPIO_Init` mask `0x2` @ `0x08014B70` (mode 0) |
| **`PD2`+** | — | no reference |

`PD0` is also the `OSC_IN` pad and `PD1` the `OSC_OUT` pad on the packages that
expose them — the stock uses the internal HSI, so it gives the crystal pads up
for GPIO, exactly as it gives `PA13`/`PA14` (SWD) up for the LED.

## The package: LQFP48 `PY32F403C1xT`

The 48-pin part comes in **two different bond-outs** that disagree about the
first pins — which matters as soon as a board is read against the wrong figure:

| signal | LQFP48 **C1xT** (Pinout1, Fig. 3-4) | LQFP48 **C2xT** (Pinout2, Fig. 3-5) |
|---|---|---|
| `PE5`, `PE6` | **not bonded** | pins 2, 3 |
| `PC13`, `PC14`, `PC15` | pins 2, 3, 4 | pin 4 only |
| `PB12`, `PB13` | pins 25, 26 | **not bonded** |
| `PB14`, `PB15` | pins 27, 28 | pins 25, 26 |
| `PC6`–`PC9` | **not bonded** | pins 27–30 |
| `PC10`, `PC11` | **not bonded** | pins 38, 39 |
| `PA12` / `PA13` | separate pins (33, 34) | one pad, pin 35 |
| `PA14` / `PA15` | separate pins (37, 38) | one pad, pin 37 |

(Numbers from Table 3-2's per-package columns, cross-checked against the
figures; `PC12` exists only on the 64/100-pin bond-outs.  Figures and table are
in `PY32F403_Datasheet_V1.8.pdf`.)

**This board is C1xT.**  The port runs the RF bus on `PB12`, the BK4815 select
on `PB13`, the LCD on `PB15` and the PA bias on `PB14`, and the stock's FM bus
is on `PC14` — all validated on the radio, and C2xT bonds none of `PB12`,
`PB13`, `PC14`.  That also settles pins 2/3: on this part they are **`PC13` and
`PC14`**, the audio-path line and the BK1080 clock, which is why they look
connected.

`PE5`/`PE6` — and the `TRACED2/3`, `TIM9_CH1/2` functions they carry on other
packages — do not exist here.  The `GPIOE` base appears only in the helpers'
port-dispatch tables (`0x08011980`, `0x08011B48`, and the bootloader's copy at
`0x080013F4`), never as a port argument, so nothing in the stock image drives a
GPIOE pin.  Likewise `PC0`–`PC12`: "free in the image" because there is no pad.

### The 48 pins, as wired

| pin | C1xT signal | stock use |
|---|---|---|
| 1 | `VBAT` | power |
| 2 | `PC13` | audio-path / amplifier-enable candidate; idle high, low on T/R (`AUDIO_PATH_PIN`) |
| 3 | `PC14` | BK1080 FM I²C clock |
| 4 | `PC15` | RF/band bring-up line, raised once by `FUN_080139E4` |
| 5 | `PD0` | output: bring-up/handshake pulses |
| 6 | `PD1` | **input, never read — spare** |
| 7 | `NRST` | reset |
| 8–9 | `VSSA`, `VCCA` | analog supply |
| 10–11 | `PA0`, `PA1` | band-path select |
| 12–13 | `PA2`, `PA3` | keypad ADC ladder (ch 2/3) |
| 14 | `PA4` | `DAC_OUT1` — beep |
| 15 | `PA5` | `DAC_OUT2` — backlight |
| 16–17 | `PA6`, `PA7` | keypad ADC ladder (ch 6/7) |
| 18 | `PB0` | keypad ADC ladder (ch 8) |
| 19 | `PB1` | battery ADC (ch 9) |
| 20 | `PB2` | BK1080 I²C data |
| 21 | `PB10` | PTT read (also USART3 AF init) |
| 22 | `PB11` | **USART3 AF init only — spare** |
| 23–24 | `VSS_1`, `VCC_1` | power |
| 25 | `PB12` | RF bus data |
| 26 | `PB13` | BK4815 chip select |
| 27 | `PB14` | PA bias PWM (`TIM1_CH2`) |
| 28 | `PB15` | LCD SDA |
| 29–32 | `PA8`–`PA11` | LCD SCLK / reset / DC / CS |
| 33 | `PA12` | RF bus clock |
| 34 | `PA13` | LED red (`SWDIO` pad) |
| 35 | `VSS_2` | power |
| 36 | `VCAP` | core capacitor |
| 37 | `PA14` | LED green (`SWCLK` pad) |
| 38 | `PA15` | SPI-NOR chip select |
| 39–41 | `PB3`–`PB5` | SPI-NOR SCK / MISO / MOSI |
| 42–43 | `PB6`, `PB7` | USART1 console / programming jack |
| 44 | `BOOT0` | boot strap (not a GPIO) |
| 45 | `PB8` | BK4829 chip select |
| 46 | `PB9` | PTT2 read / bootloader update gate |
| 47–48 | `VSS_3`, `VCC_3` | power |

The two spare lines are ordinary GPIOs: `PB11` is USART3_RX on AF2 and
`TIM2_CH4` on AF1, `PD1` is `OSC_OUT`/`EVENT_OUT`.  Configuring either is the
same recipe as any pin — port clock in `RCC->AHB2ENR` (`GPIOB` bit 3, `GPIOD`
bit 5), then `MODER`/`OTYPER`/`OSPEEDR`/`PUPDR`/`AFR`; `driver/gpio.c` already
does it (`gpio_config_input`, `gpio_config_output`, `gpio_config_af`), and
`gpio_port_clock()` maps all five ports.

Neither spare pin is analog-capable (§"Analog capability"), so they are digital
only.

## Analog capability

The ADC inputs and the DAC outputs are fixed to particular pins, and on this
package every one of them is already committed.  From the datasheet's
pin-definition table (Table 3-2, "Additional function" column) and the vendor's
channel numbering:

| ADC channel | pin | used for |
|---|---|---|
| `ADC123_IN0`, `IN1` | `PA0`, `PA1` | **digital** band-path select — ADC-capable, but taken |
| `ADC123_IN2`, `IN3` | `PA2`, `PA3` | keypad ADC ladder |
| `ADC12_IN4`, `IN5` | `PA4`, `PA5` | **DAC** outputs (beep, backlight) |
| `ADC12_IN6`, `IN7` | `PA6`, `PA7` | keypad ADC ladder |
| `ADC12_IN8`, `IN9` | `PB0`, `PB1` | keypad ladder, battery sense |
| `ADC123_IN10`–`IN13`, `ADC12_IN14`/`IN15` | `PC0`–`PC5` | **not bonded** on LQFP48 C1xT |
| `IN16`–`IN19` | — | internal only (temperature, VREFINT, VBAT, DACVIN) |

The two DAC channels are `PA4` (`DAC_OUT1`, the beep — measured) and `PA5`
(`DAC_OUT2`, the backlight — measured); `DAC1` is at `0x40007400` and the stock
configures both pins as analog at `0x0800A930`.

Consequences:

* **No spare analog input.**  Every bonded ADC channel either samples something
  (keypad, battery), drives a DAC, or drives the band switch.  Adding an analog
  input means giving up `PA0`/`PA1` (band path) or a keypad line.
* **The two spare pins are digital only.**  `PB11` has no ADC channel (its
  functions are `USART3_RX`, `I2C2_SDA`, `TIM2_CH4`, `ESMC_SS1`) and `PD1` has
  none (`CAN_TX`, `EVENT_OUT`); neither can produce an analog output.

## What the radio proves, and what the image cannot

Some connections are settled by behaviour, which outranks both the image and a
visual check of the board — several parts, the RF final among them, are fitted
on the **underside** of the PCB, so traces are not always visible:

| connection | how it is proven |
|---|---|
| `PA8`, `PA9`, `PA10`, `PA11`, `PB15` (LCD) | the panel works with the port driving exactly these pins; `PA8` clocks twice per bit and `PB15` is set/cleared per bit at `0x0801501A`–`0x08015050` |
| `PB14` (PA bias PWM) | transmit works; with the PWM unprogrammed the PA has no bias and only hisses |
| `PA12`, `PB12`, `PB8`, `PB13` (RF bus) | both transceivers initialise and their IDs read back |
| BK4829 `EARO` (pin 8) → audio node | the BK4829's `0x47` AF mute silences the speaker, including the BK4815's downlink (measured 2026-10-17); the BK1080's `LOUT`/`ROUT` share that node |
| `PC13` (audio-path line) | driven by the stock's T/R path and by the port; the amplifier-enable candidate |

Two rules follow for any pin experiment:

* **A stock-driven pin is safe to replicate.**  `PC15` (held high), `PA1` (band
  select) and `PA9` (LCD reset) are outputs the stock already drives, so driving
  the same level adds no risk.  A stock-only *input* (`PB10`) should only be
  read.  A pin the stock never touches (`PB11`, `PD1`) should be left alone
  until the board is traced.
* **No visible trace proves nothing.**  `PA8` and `PB15` look unconnected from
  the top side yet are the display's clock and data (the routing is on the
  bottom layer), so absence of a trace is not evidence of absence.

### The RF final

The PA is a **Watech/HOLTO `HTL7G06S011P`**: an 11 W LDMOS wideband RF power
transistor (1.8–600 MHz, `VDD` 7.2 V, 12 dB gain, 60 % efficiency at `Psat`,
`DFN5×5-10L`, integrated gate-source Zener).  It is the RF *final stage*, not
the audio amplifier (that is the `LM2904B`/`TDA2822` pair in the teardown).  Its
gate bias is the `PB14`/`TIM1_CH2` PWM network — see
[`ra89r_rfpath.md`](ra89r_rfpath.md) — which is why `PB14` is load-bearing even
though it never carries RF.

## What was ruled out

* **No whole-port access outside GPIOB.**  Direct `PUPDR`/`IDR`/`ODR`/`MODER`/
  `AFRH` accesses exist only in the bit-bang bus code (`0x0801DDxx`) and the
  `PB14`/TIM1 area (`0x080132xx`); every other pin goes through the four helpers
  above.
* **No `GPIOC` `PC0`–`PC12` / `GPIOD` `PD2` / `GPIOE` pin use**, under any of the
  four helpers or in the boot-time `FUN_08013C74` config — and on the 48-pin
  package those pins are not bonded at all.
* **No free serial bus.**  The stock references only USART1, USART2 and USART3 —
  no `I2C1`/`I2C2`, no `SPI2`/`SPI3`, and SPI1 is the external NOR.  Any further
  chip is on a UART, a bit-banged line, or analog (see `ra89r_findings.md`).
* **No USB bring-up.**  The USB-C port's MCU USB device peripheral is never
  enabled.

## Open points

1. **Package:** resolved to LQFP48 `C1xT` (§"The package").  Still unchecked is
   whether the two spare lines (`PB11`, `PD1`) are routed to anything at all.
2. **`PB11`'s run-time state** — is the USART3 block (`0x080134B8`) ever reached
   with the USART3 handle?  A breakpoint or an `IDR`/`AFR` read on the radio
   settles it.
3. **`PC15`'s destination** — the stock raises it once inside the RF bring-up
   (`FUN_080139E4`) and never lowers it, but what it enables (front-end supply,
   switch latch, ...) is not known; it needs a scope or meter on pin 4.
4. **Bottom-side routing** — the RF final sits on the underside (with much of the
   RF section), so a top-side visual check cannot settle a net (and has already
   been misleading: `PA8`/`PB15` look open yet carry the LCD).  Until a
   continuity probe or a beacon test is done, use the rule in §"What the radio
   proves": replicate what the stock drives, read what it reads, leave alone
   what it never touches.

## Reproduce

Decode the stock image and load it into Ghidra (raw binary,
`ARM:LE:32:Cortex`, base `0x08004000`, then auto-analyse) as described in
[`ra89r_findings.md`](ra89r_findings.md) §"Loading the current decode into
Ghidra".  Every claim above is then a decompile of one of:

* the four helpers `0x0801199C`, `0x08011884`, `0x08011B74`, `0x08011B64`, for
  the calling convention and descriptor layout;
* the `GPIO_Init` call sites and their stack descriptors (the mask field is
  `descriptor[0]`, e.g. `GPIOB` mask `0xC0` at `0x0801332E`);
* the `WriteBit`/`ReadInputDataBit` call sites and their `(port, mask)`
  literals.

A quick cross-check without Ghidra: search the decoded
`work/stock_v49_raw.bin` for the little-endian GPIO base constants
(`0x48000000` A, `0x48000400` B, `0x48000800` C, `0x48000C00` D,
`0x48001000` E) and read the surrounding code — the only hits for E are the
helper dispatch tables.

Two caveats for a listing-only sweep: `work/analysis/listing.txt` has gaps where
the auto-analysis lost sync — the LCD bit-loop body (`0x0801501A`–`0x08015058`,
which drives `PB15` and `PA8`) is missing from it, so count from the raw
disassembly with capstone rather than trusting the listing alone; and a call-site
count sees loops as one site, so it says nothing about how often a pin is
written.

## Status

**Located, not hardware-validated — except where §"What the radio proves" says
otherwise.**  The inventory is a static reading of the V49 application plus the
existing per-feature docs.  The used-pin roles that matter for the port
(`PA13`/`PA14` LED, `PA5` backlight, the keypad ladders, the RF bus) are
separately confirmed on the radio in their own docs, and the LCD, PA-bias, RF-bus
and audio-node connections are proven by behaviour (see that section).  What
remains unprobed is the routing of the two free lines (`PB11`, `PD1`).
