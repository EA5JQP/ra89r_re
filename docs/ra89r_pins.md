# RA89R MCU pin inventory

Every PY32F403 GPIO line the stock application touches, and — the point of the
file — the lines it leaves alone, so a new peripheral or feature can pick one
without colliding with the radio.  It is a **static** inventory of the current
decode (`work/stock_v49_raw.bin`, base `0x08004000`), cross-checked against the
per-feature docs; it is deliberately *not* a hardware-validation claim.  Where
this file says **free** it means "not referenced by the stock image", which is
not the same as "bonded on the package and routed to a pad on the PCB" — see
"Open points".

The pin semantics themselves live in the feature docs
([`ra89r_lcd.md`](ra89r_lcd.md), [`ra89r_keypad.md`](ra89r_keypad.md),
[`ra89r_led.md`](ra89r_led.md), [`ra89r_rfpath.md`](ra89r_rfpath.md),
[`ra89r_bk1080.md`](ra89r_bk1080.md), [`ra89r_eeprom.md`](ra89r_eeprom.md),
[`ra89r_beeper.md`](ra89r_beeper.md), [`ra89r_battery.md`](ra89r_battery.md));
this file is the board-wide index of which pin is which direction.

## Bottom line

**GPIOA and GPIOB are fully allocated**: all sixteen lines of each are driven,
read or configured by the stock.  The only lines the stock never references are
on GPIOC and GPIOD (and GPIOE, if the package bonds it):

| port | used by the stock | free (never referenced) |
|---|---|---|
| GPIOA | `PA0`–`PA15` (all) | none |
| GPIOB | `PB0`–`PB15` (all)¹ | none¹ |
| GPIOC | `PC13`, `PC14`, `PC15` | **`PC0`–`PC12`** |
| GPIOD | `PD0`, `PD1` | **`PD2`** (and `PD3`+ where bonded) |
| GPIOE | none | all (where bonded) |

¹ `PB11` is the borderline case: the only thing referencing it is the USART3
bring-up (§GPIOB).  It is still *configured* by the stock, so do not treat it as
free without checking.

## How the stock talks to its pins

There is **no whole-port access**: the stock never loads a GPIO `IDR`, `ODR` or
`BSRR` as a word.  Everything goes through four helpers (the same finding the
keypad write-up relies on), which is what makes a static sweep tractable:

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

One cluster bypasses the helpers: the **RF filter-switch** bodies
`0x08020368`/`0x08020348` clear bits in a GPIO `PUPDR`/`ODR` to switch the VHF/UHF
filter (a `0xC0`, `0x120` and bit-0 mask).  It has **no static caller** — a full
`bl`/`blx` and pointer-table sweep finds no reference to its entry
`0x08020260` (see [`ra89r_findings.md`](ra89r_findings.md)), so it is either dead
code for the other variant or reached through a RAM-held pointer.  The GPIO base
it acts on comes from a state struct, not a literal, so it cannot be pinned to a
port statically.

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

**The `PB11` caveat.**  The only code that names `PB11` is the USART3 init
(`GPIO_Init` mask `0xC00` = `PB10`+`PB11`, mode AF, AF2).  But `PB10` is read at
run time as a PTT input (`0x0802192E`), exactly as `PA2`/`PA3` are really keypad
lines despite their USART2 AF config.  That makes the USART3 block look like the
same debug bring-up, in which case `PB11` would be effectively spare — but it is
still configured as an output AF pin in the image, it is not proven dead, and
this has not been checked on the radio.  Confirm with a breakpoint on
`0x080134B8` before reusing it.

## GPIOC — only `PC13`/`PC14`/`PC15` used

| pin | role | evidence |
|---|---|---|
| `PC13` | key/PTT read, and driven low by the key dispatcher; amplifier-enable candidate (`AUDIO_PATH_PIN`) | `GPIO_Init` mask `0xA000` @ `0x08013D2E`; reads mask `0x2000` in the key path (`0x08013F3C`, `0x080218F6`); writes `0x2000` (14 sites) |
| `PC14` | BK1080 FM SCL | `GPIO_Init` mask `0x4000` @ `0x0800D4A6`; 13 `WriteBit` sites |
| `PC15` | plain output; purpose not identified | `GPIO_Init` mask `0x8000` @ `0x08013970`, and `0xA000` @ `0x08013D2E` |
| **`PC0`–`PC12`** | — | **no reference anywhere in the image** |

`PC4` deserves a caution even though it is free here: the ADC scan has a second
variant (`FUN_0801BE90(1)` → `FUN_08004E58`) that reads the keypad/aux channel as
**channel 14 = `PC4`** instead of channel 2 = `PA2`.  In this RA89R build the pin
always configured is `PA2` (`GPIO_Init` mask `0x4` @ `0x0801BF2C`, mode analog or
input depending on the variant flag), and no `GPIO_Init` targets `PC4` — so `PC4`
is not driven.  But if the port is to cover both hardware variants / the RA89G,
`PC4` is the one GPIOC line to keep in reserve.

## GPIOD — `PD0` and `PD1` used

| pin | role | evidence |
|---|---|---|
| `PD0` | output (boot init); also read as a strap | `GPIO_Init` mask `0x1` @ `0x080139A4`, `0x08013D62`; 7 `WriteBit` sites; read @ `0x080066E2` |
| `PD1` | output | `GPIO_Init` mask `0x2` @ `0x08014B70` |
| **`PD2`+** | — | no reference |

`PD0` is also the `OSC_IN` pad and `PD1` the `OSC_OUT` pad on the packages that
expose them — the stock uses the internal HSI, so it gives the crystal pads up
for GPIO, exactly as it gives `PA13`/`PA14` (SWD) up for the LED.

## GPIOE

Never referenced by name in the application; `0x48001000` appears only as a base
constant inside the `GPIO_Init`/`GPIO_ReadInputDataBit` dispatch tables (the
helpers know all five ports), not in any pin code.  On the larger pinouts
(`PE2`–`PE15`) those lines are free; on a 64-pin or smaller part they are not
bonded.

## What was ruled out

* **No whole-port access.**  No `ldr`/`str` against a GPIO `IDR`/`ODR`/`BSRR`
  beside the RF filter-switch bodies; every other access is through the four
  helpers above.
* **No free serial bus.**  The stock references only USART1, USART2 and USART3 —
  no `I2C1`/`I2C2`, no `SPI2`/`SPI3`, and SPI1 is the external NOR.  Any further
  chip is on a UART, a bit-banged line, or analog (see `ra89r_findings.md`).
* **No USB bring-up.**  The USB-C port's MCU USB device peripheral is never
  enabled.
* **No `GPIOC` `PC0`–`PC12` / `GPIOD` `PD2` / `GPIOE` pin use**, under any of the
  four helpers or in the boot-time `FUN_08013C74` config.

## Open points

1. **Package and PCB routing.**  The repository does not record which PY32F403
   package the radio uses.  The pin-description table in
   `PY32F403_Datasheet_V1.8.pdf` gives the per-package bonding: on **LQFP64**
   `PC0`–`PC12`, `PC13`–`PC15` and `PD0`–`PD2` are bonded; on **LQFP48**/
   **QFN48** several `GPIOC` lines are absent.  "Free in firmware" must be
   combined with a board photo/continuity check before assuming a pad exists.
2. **`PB11`'s run-time state** — is the USART3 block (`0x080134B8`) ever reached
   with the USART3 handle?  A breakpoint or an `IDR`/`AFR` read on the radio
   settles it.
3. **`PC15`'s function** — driven from `0x08013970`/`0x08013D2E` at boot and held,
   but nothing in the docs attributes a behaviour to it.
4. **The RF filter-switch GPIO base** (`0x08020260`'s `state->[0]`) — which port
   those `0xC0`/`0x120`/bit-0 clears land on, if the cluster ever runs.
5. **`PC4` as a spare** — free in this build, but reserved by the alternate ADC
   variant; not a clean spare if both revisions must be supported.

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

## Status

**Located, not hardware-validated.**  The inventory is a static reading of the
V49 application plus the existing per-feature docs.  The used-pin roles that
matter for the port (`PA13`/`PA14` LED, `PA5` backlight, the keypad ladders, the
RF bus) are separately confirmed on the radio in their own docs; the *free*
lines have not been probed.
