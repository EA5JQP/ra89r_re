# RETEVIS RA89R / RA89G — firmware reverse engineering findings

Target firmware: `FIRMWARE_RA89R_20260203_V49.icf`.
A second, newer sample of the same family is also in the repository:
`Ra89G_R_UpDataFile20260401_V52_10W_Enable.icf` (RA89G, V52, "10W enable") --
same container and baseline (`0x66`, 72 records from `0x08004000`), 146,064
bytes, 83% of bytes differing from the RA89R V49 image at equal offsets (they are
different builds, so compare by content, not by offset).  Useful as a
cross-check for any subsystem when the two models disagree.
Decoded with `ra89r.py` -> `work/FIRMWARE_RA89R_20260203_V49.bin`
(72 records, baseline `0x66`, 145,772 bytes, flash `0x08004000..0x0802796C`).
Analysis: `tools/ra89r_analyze.py` -> `work/analysis/{listing,functions,strings,
peripherals,datarefs}.txt|csv`.

**This document supersedes the earlier revision**, which was written from images
produced by an older decoder. That decoder decrypted the payload bytes correctly
(71 of 72 records are byte-identical to the current decode) but laid the image
out wrongly, so every address in the earlier revision is shifted. See
"Address drift" below before reading any address from an older note.

## Hardware

| block | identification | evidence |
|---|---|---|
| MCU | **Puya PY32F403** (Cortex-M4F), 384 KB flash `0x08000000`, 64 KB SRAM `0x20000000`, `PY32F403xD` register map | vendor SDK + datasheet in `PY32F4xx_Firmware/`, `PY32F403_Datasheet_V1.8.pdf`; `FLASH_END = 0x0805FFFF` in `py32f403xD.h` |
| Bootloader | 16 KB at `0x08000000`, dumped separately (`bootloader.bin`), SP `0x20003190`, reset `0x08000145`, contains the `.icf` record validator at `0x08000BE0` | `ra89r.py` docstring; `bootloader.bin` |
| RF transceiver | **BK4815 / BK4829** (both supported by this build) | UI/debug strings `4815 Error`, `4829 Error` at `0x080270F4`ff; the four-byte register frame below matches the BK481x 3-wire write |
| External flash | **SPI NOR, Winbond-class (`0xEF`), device id `0x16`**, on SPI1 | firmware reads it itself: `0x08018C7C` sends `0x90` + 24-bit address and compares the reply with `0xEF16` |
| Display | **128x64 mono dot matrix, ST7565-family controller, bit-banged 4-wire bus** | see `ra89r_lcd.md` |
| Bluetooth | module on a UART, AT-command driven | AT command strings at `0x08022344..0x080225D0` and `0x08022820` |
| Models | **RA89R and RA89G** share this firmware | `RETEVIS RA89R`, `RETEVIS RA89G` at `0x080223E8`, `0x08019ED8`, `0x0802689C` |

## Address drift (read this before quoting any address)

The earlier revision's images (`~/Repos/ra_re/RA89R_decoded*.bin`, and the Ghidra
programs of the same name in the `~/Repos/h8_re` project) start the application
image at `0x08000000` instead of `0x08004000` and emit **one extra byte per
record**, so an address `A` in those images corresponds to

```
A_actual = A_old + 0x4000 - record_index(A_old)      record_index ~ (A_old - 0x08004000) / 0x800
```

Verified examples (payload found by content match):

| old note | actual | symbol |
|---|---|---|
| `0x0801C298` | `0x08020260` | RF filter-switch cluster |
| `0x080158E7` | `0x080198BC` | band/range table marker |
| `0x0800F250` | `0x08013232` | RF band setup |
| `0x0800D9A2` | `0x080119A2`+ | (see "GPIO/BSP helpers" below) |

Older analysis is therefore still useful for its *conclusions*, but addresses must
be re-derived from the current decode.

## `.icf` container (verified, authoritative: `ra89r.py`)

One CR-separated ASCII-hex record per line: `[6-byte header][payload][1 check]`.
Header bytes are XORed with a per-family baseline (`0x66` RA89R, `0x88` UV8800,
`0x90` TH9000D); payload **and the check byte** are XORed with a per-record key
`key = h[0]^h[1]^h[2]^h[3]^h[4]^h[5] ^ baseline`; `length = (h[0]<<8)|h[1]`,
`addr = ((h[2]<<16)|(h[3]<<8)|h[4]) * 0x100`; the record is valid iff
`(sum(h) + sum(payload) + check) mod 256 == 0`. The check byte is *computed*, not
brute-forced (the bootloader routine at `0x08000BE0` does the same sum).

The earlier revision described a "v3/v4" per-section key heuristic
(`key = h[4] ^ (h[3] ^ 0x66)`). That heuristic reproduces the true key for 71 of
this file's 72 records, which is why the old decodes looked mostly right, but it
is not the format: it fails on the short final record and does not generalise
(`ra89r.py` is validated on 616 records from nine stock files across three
radios). There is no "v3/v4" question any more — use `ra89r.py`.

## Address map of the current decode

### Display subsystem (full detail in `ra89r_lcd.md`)

| address | role |
|---|---|
| `0x08014F42` | `lcd_init()` — reset pulse + 21-byte ST7565-family init |
| `0x08014C94` | `lcd_clear()` (fills 64 rows x 128 columns with 0x00) |
| `0x08015004` / `0x08015080` / `0x080150A6` | bit-banged byte / command / data writers |
| `0x0801C9A4` | send page+column address triple |
| `0x0800F698` | (row, column) -> page/column packet, adds the panel's `+4` column offset |
| `0x08014E38` | glyph/data blitter (page-major, one byte per column) |
| `0x08014D0E` / `0x08014CF4` | text renderer (8x16 and 5x7 fonts, GB2312 path) |
| `0x08026072` | 8x16 font, ASCII `0x20..0x7A`, 16 bytes/glyph |
| `0x08026622` | 5x7 font, ASCII `0x20..0x5A`, 5 bytes/glyph |

LCD pins: SDA `PB15`, SCLK `PA8`, A0/DC `PA10`, CS `PA11`, RESET `PA9`.

### External SPI flash (SPI1: SCK `PB3`, MISO `PB4`, MOSI `PB5`, NSS `PA15`)

| address | role |
|---|---|
| `0x20000D28` | SPI handle (SPI1) in RAM; `0x20000D84` = DMA1_CH1 handle used for it |
| `0x08012A9C` | clock + pin init for the SPI instance (dispatches on the handle's base) |
| `0x08018B6C` | write-enable (`0x06`) |
| `0x08018BB8` | 4 KB sector erase (`0x20` + 24-bit address) |
| `0x08018C7C` | read ID (`0x90` + 24-bit 0) -> `0xEF16` |
| `0x08018D38`, `0x08018E10` | page program (polled and DMA variants) |
| `0x0801D1D8` | SPI1/handle bring-up; ends by validating the flash ID |
| `0x0801CE90` | walks a 512-entry table (8 bytes/entry) and erases sectors past `0x1F8` |

### GPIO / BSP helpers (the earlier revision's names were guesses)

| actual | earlier name | role |
|---|---|---|
| `0x0801199C` (with a config struct) | "`FUN_0800d9ba`" | per-pin GPIO configuration from a descriptor (`mask`, mode, pull, speed, AF) |
| `0x08011884` | — | same, mask-only form (used for alternate functions) |
| `0x08011B74(port, mask, value)` | "`FUN_0800db9c`" | pin set/reset: `value ? port->BSRR = mask : port->BRR = mask` |
| `0x08011C48` / `0x08011C74` | — | NVIC enable / disable |
| `0x08011154`, `0x080111D8`, `0x080115FC` | — | peripheral/DMA init, deinit and clock helpers; they dispatch on the peripheral base held in the first word of a handle (`(base - DMA1_CH1)/0x14` = channel index), the same pattern the SPI-flash handle (`0x20000D28`) uses |
| `0x08024E80`-`0x08024F80` | — | mixed table of code/data pointers used by that layer |

Register offsets actually used (from the vendor header, not guessed):
GPIO `MODER 0x00`, `OTYPER 0x04`, `OSPEEDR 0x08`, `PUPDR 0x0C`, `IDR 0x10`,
`ODR 0x14`, `BSRR 0x18`, `LCKR 0x1C`, `AFRL 0x20`, `AFRH 0x24`, `BRR 0x28`.
(The earlier revision labelled `+0x0C` "MODER"; `+0x0C` is PUPDR and MODER is
`+0x00`.) SPI `CR1 0x00 SR 0x08 DR 0x0C`; DMA channel `CCR 0x00 CNDTR 0x04
CPAR 0x08 CMAR 0x0C`; USART `SR 0x00 DR 0x04 BRR 0x08 CR1 0x0C`.

### RF: filter switching and band data

`RF_FilterSwitch` cluster, **actual addresses**:

* `0x08020260` — `RF_FilterSwitch_VHF_UHF(state)`: `state->[0]` is a GPIO base;
  the routine reads its `ODR` (`+0x14`) bit 7 and bit 6, compares the state bytes
  `+0x45 == 0x21 '!'` and `+0x46 == 0x22 '"'`, and when the matching ODR bit is
  set calls `0x08020368` (from `0x08020280`) or `0x08020348` (from `0x0802029C`).
* `0x08020368` — `*(gpio + 0x0C) &= ~0xC0`, then `state[0x45] = 0x20`.
  `0x08020348` — `*(gpio + 0x0C) &= ~0x120`, `*(gpio + 0x14) &= ~1`, then
  `state[0x46] = 0x20`.
  Caveat: in the vendor register map `+0x0C` is `PUPDR` and `+0x00` is `MODER`,
  so the earlier revision's "clears MODER bits PB2/PB3/PB4" label is wrong even
  though the masks are the ones it quoted; whether those pins are really the
  filter-select lines (and which band is which) still needs a hardware check.
* **No static caller**: a full-image sweep of every 2-byte-aligned `bl`/`blx`
  and of all function-pointer tables finds no reference to `0x08020260`; the
  cluster is reached through a runtime-built or RAM-held pointer, or it is
  legacy for the other model in this build. (This confirms the earlier
  revision's item 4 — the earlier addresses were just shifted.)

Band/range table, **actual address `0x080198BC`** (10 Hz units):

| address | value | meaning |
|---|---|---|
| `0x080198BC` | `0x00A4CB80` | 108 MHz marker |
| `0x080198C0` | `0x20009BB8` | RAM pointer (runtime struct) |
| `0x080198C4` | 174 MHz | VHF max |
| `0x080198C8` | 400 MHz | UHF min |
| `0x080198CC` | 520 MHz | UHF max |
| `0x080198D0` / `0x080198D4` | 144 / 146 MHz | VHF refs |
| `0x080198D8` / `0x080198DC` | 430 / 440 MHz | UHF preset |
| `0x080198E0` | 148 MHz | UHF/VHF preset |
| `0x080198E4` | 420 MHz | preset |
| `0x080198E8` / `0x080198EC` / `0x080198F0` | 450 / 460 / 470 MHz | presets |
| `0x080198F4` | 136 MHz | VHF min |
| `0x080198F8` / `0x080198FC` | `0x20009C14` / `0x20009D98` | RAM pointers |

These match the CPS "Frequency Range" presets (see below), and no code path
compares a live frequency against them in-line — the ranges are CPS/EEPROM data,
so a band change is an EEPROM/`CPS` edit, not a flash patch.

### Backlight / lamp

**GPIOA pin 1** (mask `0x2`), plain push-pull output, **level 1 = on**: the stock
**bootloader** blinks exactly this pin when it enters update mode
(`0x08000582`: clear -> 100 ms -> set -> 100 ms, three times) and leaves it
**high** while it shows the "Update..." screen.  The panel lamp was observed to
follow that blink on the radio (hardware check, not inference).

Correction to the previous revision of this section: it claimed the stock
*application* "drives the same pin from ~78 sites".  That was a misreading --
the app's GPIOA writes are dominated by the **panel bus** (PA8 SCLK, PA9 RST,
PA10 A0, PA11 CS, plus PB15 SDA -- see `ra89r_lcd.md`), the bit-banged buses
(PA12/PA13/PA14 with PB8/PB12/PB13, e.g. the RF register transport at
`0x080180F0`), the SPI NOR (PA15) and PD0/PC13-PC15.  Exactly **eight** writes
touch pin 1, and every one sits in a `switch (0..3)` helper that sets a two-bit
field made of **PA0** plus a BK4815/BK4829 register bit (`0x08013A70`,
`0x08013B12`, and the single sites at `0x0800940A` / `0x0801BE70`), each right
next to an RF register write (`0x080137D4` -> reg `0x33`, `0x08013790` -> reg
`0x75`).  PA0 + PA1 are also configured together (one `GPIO_Init`, mask `0x3`).

**Resolved, with one loose end (radio test).**  Pin 1 alone left the panel dark,
so the driver also drives **GPIOA pin 5** -- the pin the bootloader blinks five
times at `0x08000AD2`, immediately before the pin-1 blink -- and with both lines
driven the lamp comes on and `l` switches it.  So the pin-5 line does something
that pin 1 on its own does not.  Still undetermined: whether pin 5 alone is
enough or both lines are required, and how either squares with the stock
application configuring PA4/PA5 as **DAC outputs** (`0x0800A8F4` -> DAC1 at
`0x40007400`, called from the boot path at `0x0801D748`).  Narrowing that is
follow-up work; the second line is `BACKLIGHT_AUX_PIN` in `board_pins.h`.

No PWM/DMA-to-BSRR path was found in this build (unlike the UV-K1/K5V3, whose
backlight is dimmed with TIM7+DMA), so on/off is all this radio needs.

Implemented as `firmware/App/driver/backlight.{c,h}` (API mirrors the K1 driver
of the same name) and driven on at boot; the console command `l` toggles it.

### Keypad (SOLVED: a 5-line analog key matrix)

**Read this section bottom-up.**  The intermediate conclusion in its middle
("the keys are NOT on the ADC") was itself wrong and is withdrawn; the answer is
the "SOLVED: a 5-line analog key matrix" block further down.  Neither the
"recon" title this section used to carry nor that paragraph should be quoted.

The stock keypad is **not** an MCU GPIO matrix, which is worth recording because
the sibling port tree uses exactly that (`UV-K1/K5V3`, `App/driver/keyboard.c`:
4x4, cols PB3-PB6 driven, rows PB12-PB15 read).  There are 20 buttons on this
radio and they are read **through the ADC**, as a resistor ladder.

How to reproduce the analysis (see "Loading the current decode into Ghidra"):

* **No port-level GPIO access at all.**  Every GPIO access goes through the two
  single-bit helpers (`GPIO_WriteBit` `0x08011B74`, `GPIO_ReadInputDataBit`
  `0x08011B64`) or the `GPIO_Init` wrapper (`0x0801199C`); no `ldr`/`str`
  against a GPIO `IDR`/`ODR`/`BSRR` exists, so nothing reads a whole port.
* **Only 17 single-bit input reads**, on 7 lines: `PC13`, `PA14`, `PA13`, `PB9`,
  `PB10`, `PA2`, `PD0`.  Seven lines cannot scan 20 keys.
* **No interrupt path either.**  With the vector table read at the correct
  `IRQn + 16` offset, every `EXTI*` vector is the default handler
  (`0x0800415F`) -- keys are polled.  (The real handlers are DMA1 streams,
  `TIM3`/`TIM5`, `SPI1`, `USART1`, `SysTick`.)

**Correction: the keys are NOT on the ADC.**  The 6-channel ADC scan is real and
worth recording, but it is the **S-meter**, not a keypad ladder.  `FUN_08004E58`
programs a regular scan of six channels -- `2` *or* `14`, then `3`, `6`, `7`,
`8`, `9` (`FUN_080109E4` per channel) -- and `FUN_0801BE90`/`FUN_08005460` start
a DMA conversion (`FUN_08010DB8(inst, buffer, 0x30)`) whose results land at
`0x20000C28`; the channels map to **PA2, PA3, PA6, PA7, PB0, PB1**, the pins the
app puts in **analog** mode (`GPIO_Init` mode `3`, mask `0xCC` for PA2/PA3/PA6/PA7,
`GPIOB` mask `0x3` in `FUN_08014B00`).  The two variants (`FUN_0801BE90(0/1)`:
channel 2 on PA2 vs channel 14 on PC4) look like RA89R/RA89G or two revisions.

The **only** consumers of those results are:

* `FUN_08024260(ch)` -- average of 8 samples of channel `ch` (sample stride
  `0x18`, channels 4 bytes apart) -- and it is called from exactly two places,
  both reading **channel index 5** (the 6th = ADC channel 9 = **PB1**):
  `FUN_08007664` (main loop: `>> 4`, compare and draw a bar through the display
  module = the S-meter) and `FUN_0800E514` (average 5 samples, `>> 6`, smoothed
  value).

No function maps an ADC reading to a key code, and no threshold table is
referenced from any key path, so the earlier "keypad = ADC ladder" claim in this
file was wrong and is withdrawn.

**The key codes (extracted).**  The app keeps its key state in a struct at
**`0x20009F80`**: "new key" flag at **`+0`**, **key code at `+3`**.  It is read by

* `FUN_08013F28` -- the key dispatcher (reached from the main loop through
  `FUN_08013E2C`, and from the radio loop `FUN_0801A228`), which normalises the
  code through `FUN_08013DB0` first;
* `FUN_0801A228` -- the radio loop; digit keys arrive as `FUN_0800B7A4(code - 10)`.

The codes the dispatcher handles:

| code | meaning (from the code) |
|---|---|
| `1` | power/standby; `100` is its long-press variant |
| `4`..`9` | six **programmable** keys, remapped by setting through `FUN_0800996C` / `FUN_0800990C` / `FUN_08009938` (`0x08013DB0`) |
| `10`..`19` | **digits 0..9** (`FUN_0800B7A4(code - 10)` writes the digit) |
| `0x15`, `0x16`, `0x18`, `0x1a`..`0x1f` | navigation / mode keys |
| `0x20`..`0x25`, `0x26` | menu area (MENU/UP/DOWN/EXIT/`*`/`#`, plus `0x26`) |
| `0x2a`, `0x2d`, `0x30`, `0x33`, `0x36`, `0x39`, `0x3c`, `0x3f`, `0x42`, `0x45`, `0x48`, `0x4b`, `0x4e`, `0x51` | **step of 3** -- a base code plus `+1`/`+2` for long / extra-long press |

What those codes *do* (the dispatcher `FUN_08013F28` is the map; these are the
handlers worth knowing):

| action | code | handler |
|---|---|---|
| enter the menu | `0x20` | `FUN_0800C41C` |
| menu / channel up, down | `0x15`, `0x16` | `FUN_080094CC(param, 1\|0)` |
| change volume (or another stepped value) | `0x18` | `FUN_08015D14(3\|4)` |
| type a digit | `10`..`19` | `FUN_0800B7A4(code - 10)` |
| **channel / frequency change** | -- | `FUN_08005638`: writes the u16 at state `+0x20`, clears the key state, `FUN_080220A0(0xC001, 0x2B)` on the BK (reg `0x2B`) |

The CPS corroborates that the radio has a real keypad: `_8890DTest/RadioSet.cs`
carries the same `P1 Short` / `P2 Short` / `P1 Long` / `P2 Long` labels the
firmware uses, its `Key Lock` options distinguish **`按键` (buttons) from `侧键`
(side keys)**, and the CPS project also contains `Cmx138Set.cs` (a CMX138 voice
chip), i.e. the radio does have companion silicon.

**The physical keys.**  Only three lines are read as keys: `PC13`, `PB9`,
`PB10` (the other four reads -- `PA2`, `PA13`, `PA14`, `PD0` -- are straps or
bus lines).  `FUN_080218E8` (main loop via `FUN_08021A38`) requires **`PC13`
low**, then reads **`PB10`** and calls `FUN_08021950`, which debounces
(`counter == 8`), times long presses (up to 56000 ticks vs a configured
timeout) and also samples **`PB9`**; a `PB9` high keeps the hold timer running.
`PB9` is exactly the line the **bootloader waits on** to leave update mode
(`0x080005B0`: loop until `GPIOB` pin 9 reads high).  That matches the radio's
"**PTT1 and PTT2 must be pressed to reach the bootloader**": the app reads the
two PTTs individually (`PC13`, `PB10`) and `PB9` is the combined line the
bootloader gates on.

**SOLVED: a 5-line analog key matrix.**  The 20 buttons are read as **analog
levels**, which is why no digital scan exists: 19 of them hang on the five
ADC-capable pins, one key group per line, and `PTT2` is the one digital key
(`PB9`).  The owner's console probe confirms it and matches the code exactly --
pressing `PTT1` pulled `PA2`, `AB` pulled `PA6`, `3` pulled `PA7`, `6` pulled
`PB0` and `9` pulled `PA3`.

* scanner: `FUN_08024324` (called from the main loop, gated by the flag at
  `0x20009F80+1`).  It reads ADC ranks 0..4 with `FUN_08024260(rank)` and calls
  one tiny handler per key;
* each handler tests **one ADC window** and stores its **key code** into a
  per-key state byte at `0x20009F80+5 .. +0x19` (`FUN_08005724` = a bounded
  store); the UI reads the resulting code through `0x20009F80+3`;
* the four windows are the **same on every line** (a four-value resistor ladder
  per line), so *line x window* = 20 keys:

| window (12-bit ADC) | `PA7` ADC3 | `PB0` ADC4 | `PA6` ADC2 | `PA3` ADC1 |
|---|---|---|---|---|
| `(0, 0x07C]` | `0x0D` = 3 | `0x10` = 6 | `0x17` | `0x13` = 9 |
| `(0x384, 0x47C]` | `0x0C` = 2 | `0x0F` = 5 | `0x15` | `0x19` |
| `(0x8B2, 0x9AA]` | `0x0B` = 1 | `0x12` = 8 | `0x16` | `0x0A` = 0 |
| `(0xABB, 0xBB3]` | `0x0E` = 4 | `0x11` = 7 | `0x14` | `0x18` |

`PA2` (ADC rank 0, channel 2 -- or channel 14 on the other board variant)
carries the "programmable" keys instead: windows `(0x4AA, 0x5A2]` -> codes
`4/5/6`, `(0x74E, 0x846]` -> `7/8/9` (three codes per window = the press-type
variants the `P1/P2 Short/Long` settings select), plus a **digital** read of the
same pin (`FUN_08014AD0`, stored as code `100`) -- and `PTT1` is the key that
pulls `PA2` fully low.

The `PA6`/`PA3` function slots carry codes `0x14`-`0x19` (20..25) plus `+6` /
`+0xC` variants (26../32..) = the navigation/menu keys with long / extra-long
presses.  Handler addresses are `0x080146A0`-`0x08014B00`; the per-key bytes are
`0x85, 0x86, 0x88, 0x8A`-`0x99` (19 of them) and `PB9` makes the twentieth.

**How a press becomes a code.**  Each handler calls
`FUN_08005724(counter, buf3, in_window)` with `buf3 = {short, held, long}` (`0xFF`
= unused), which turns the *how long* into the *which code*:

| counter reaches | action |
|---|---|
| `4` | post `buf3[0]` -- the **short press** code |
| `0x28` (40) | post `buf3[1]` -- the **held / repeat** code |
| on release, while `4 <= counter < 0x28` | post `buf3[2]` -- the **long press** code |

The counter is the per-key byte at `0x20009F80+5..+0x19`, incremented once per
scan pass while its key stays in-window (`>= 0xF0` clamps to `0xEF`) and reset
when it leaves.  So one button owns three codes -- e.g. `{0x15, 0x1B, 0x21}` is a
single key whose short press is `0x15`, whose hold repeat is `0x1B` and whose
long press is `0x21` -- and since `0x21` is what the menu list uses as "step up",
that button is **UP** (and `{0x16,0x1C,0x22}` is **DOWN**).

**Measured on the radio** (console monitor in `App/driver/keypad.c`; idle level
`0xFF6`-`0xFF8`, i.e. the ladders idle near full scale):

| line | levels measured while pressed | keys |
|---|---|---|
| `PA7` | `0x000` `0x3E9` `0x923` `0xBCE` | 3, 2, 1, 4 |
| `PB0` | `0x000` `0x3EC` `0x91F` `0xB2D` | 6, 5, 8, 7 |
| `PA6` | `0x000` `0x3F0` `0x91B` `0xB2A` | `0x17`, `0x15` (UP), `0x16` (DOWN), `0x14` |
| `PA3` | `0x000` `0x3F5` `0x91D` `0xB2A` | 9, `0x19`, 0, `0x18` |
| `PA2` | `0x000` `0x533` `0x7DB` | `100` (PTT1), codes `4`-`6`, codes `7`-`9` |

Every measured level falls inside the vendor's window, so the stock calibration
transfers to this board unchanged (the reader widens each window by 96 counts
only because it samples once per pass rather than the stock's 8-sample average;
the bands are far apart -- narrowest gap 273 counts -- and the idle level is
above the top band).

**Which button is which.**  The port uses the K5V3/F4HWN `KEY_Code_e`, with the
owner's naming: the RA89R's `F` is `KEY_MENU`, `AB` is `KEY_EXIT`, `#` is
`KEY_F`, and its `SIDE1`/`SIDE2` are `KEY_SIDE1`/`KEY_SIDE2`.

| code(s) | line | K5V3 key | how it was settled |
|---|---|---|---|
| `10`-`19` | `PA7`/`PB0`/`PA3` | `KEY_0`-`KEY_9` | measured with the monitor |
| `100` / `PB9` | `PA2` / `PB9` | `KEY_PTT` / `KEY_PTT2` | measured with the monitor |
| `0x15` / `0x16` | `PA6` | `KEY_UP` / `KEY_DOWN` | the list widget uses their held codes `0x21`/`0x22` as list-up/down |
| `0x17` | `PA6` | `KEY_MENU` | measured: pressing the owner's MENU key holds `PA6` at `0x000` (the A tap) steadily |
| `0x14` | `PA6` | `KEY_EXIT` | the other of the pair, by elimination |
| `4`-`6` / `7`-`9` | `PA2` | `KEY_SIDE1` / `KEY_SIDE2` | the only codes with three press types = the CPS's "Side1/Side2 Short/Long" settings |
| `0x18` / `0x19` | `PA3` | `KEY_STAR` / `KEY_F` | the remaining pair, by keypad row position (`9 * 0 #`) |

Two of these came off the radio against an earlier *inference* from the stock's
handlers, which had them the other way round: `0x14`'s held code `0x20` is the
menu (`FUN_0800C41C`) and `0x17`'s held/extra codes land on the invalid/back beep
(`FUN_0801880c(0x38)` in every menu context), which reads as "0x14 = MENU".  The
radio disagrees: the key the owner presses as MENU holds `PA6` at the A tap
(steady `0x000` for 250 ms in the monitor log), i.e. code `0x17`.  The radio wins.
Worth knowing when the port assigns functions: whatever the stock does with these
two keys, its own *menu* is on the `0x14` key's long press.

The `*` / `#` pair is the one still placed by elimination -- `0x18` and `0x19` sit
in the same keypad row and nothing in the binary separates them -- so it is what
to re-check first (the console monitor prints the code beside the name).
`App/driver/keypad.h` exposes the K5V3 enum so the port can use this reader
unchanged.

**Our reader copies the stock's ADC scheme.**  The vendor does not convert on
demand -- the ADC scans six channels (2, 3, 6, 7, 8, 9) continuously into memory
through DMA, and the accessor averages eight samples per channel out of that
buffer.  `keypad.c` now does the same: `DMA1_Channel1` in circular mode
with 32-bit transfers, into a 48-word buffer shaped exactly like the stock's
(one round of six channels = 24 bytes, the "sample stride 0x18" of its accessor),
and a poll averages the last eight samples of each line.

That matters for the port rather than for elegance: one conversion with the
longest sample time is ~123 us, so a five-line scan that waited for its own
conversions cost ~5 ms -- half of the 10 ms tick the K5V3 application polls the
keypad on (`APP_TimeSlice10ms`, thresholds 20 ms / 400 ms).  Free-running, the
same scan is background hardware and a poll is a memory read.

**One round, not eight.**  The buffer therefore holds a *single* round of the six
channels and a poll takes the newest value of each line, where the stock averages
eight rounds.  Measured reason: with the eight-round window the press and release
*edges* mis-read.  The window still holds pre-press samples, so the average is a
*fraction* of the tap level, and a fraction lands inside a neighbouring window --
one key press was reported as its neighbours (`0x9FF` and `0x3FF` = 5/8 and 2/8 of
an idle `0xFFF`, decoded as the neighbouring two keys; the steady state, all eight
samples, decoded correctly).  The stock tolerates the long window because its own
4-consecutive rule debounces on top; here debouncing belongs to the application
layer, which wants the instantaneous level.

Also set explicitly: `RCC_CFGR.ADCPRE = PCLK2/2`.  Nothing else in the firmware
touches it, so its value was whatever the bootloader left, and the sample time --
hence how levels compare with the stock windows -- depends on it.  The stock's DMA
configuration is `MINC|PSIZE32|MSIZE32|CIRC|very-high priority`; ours is
identical.

**This part maps DMA requests in `SYSCFG`, not with a `CSELR`.**  A channel's
request is a 7-bit field in `SYSCFG->CFGR[2..4]` (DMA1 channels 1-4 at the bottom
of `CFGR[2]`, 8 bits apart); `ADC1` is map value `0`
(`LL_SYSCFG_DMA_MAP_ADC1`), i.e. the reset default.  Write it anyway: the SDK's
own ADC+DMA example (`Projects/PY32F403-STK/Example_LL/ADC/ADC_MultiChannelSingleConversion_TriggerSW_DMA`)
writes it explicitly, and so does the vendor's DMA driver -- `FUN_080111D8`, the
function the stock ADC power-up calls, sits right next to the `0x40010000`
literal.  Relevant to any future DMA user (the port's SPI/RF paths), not just the
keypad.  Also worth knowing from that example: it runs its DMA 16-bit
(`LL_DMA_PDATAALIGN_HALFWORD`) while the stock uses 32-bit slots -- either works,
the value is in the low 12 bits.  **Not yet run on the radio** -- the `k` monitor is the check, and the
raw values it prints should be unchanged (they are the same measurement, still
averaged over eight samples).

**Unrelated to the keypad, but found while reading the dispatcher:** keys
`0x15`/`0x16` (and the held `0x23`/`0x25`) drive **`PC13` low**
(`GPIO_WriteBit(GPIOC, 0x2000, 0)` in `FUN_08013F28`) -- the same line the PTT
path *reads*.  Not explained, and worth revisiting with the PTT / bootloader
work.

The button set is the same as the `UV-K1/K5V3` keyboard enum (`PTT1`, `PTT2`,
`SIDE1`, `SIDE2`, `F`, `UP`, `DOWN`, `AB`, `0`-`9`, `*`, `#`), i.e. the UI is a
sibling codebase, and the CPS agrees the radio has a real keypad (its key-lock
options separate `按键` from `侧键`).

**The probe has been removed.**  It parked those lines as *inputs with pull-ups*,
so pressing a button pulled the ladder node toward ground through its resistor
while the MCU sourced current into it -- that, not the keypad, is why the radio
got warm during the test.  Since the ladder pins must stay in analog mode and
untouched, `App/driver/pinwatch.{c,h}` and its `w`/`W` console commands were
dropped again (git history on `driver/keypad` has them if they are ever needed
for the RA89G variant).  The pins themselves are recorded in
`firmware/App/board_pins.h` as documentation only.

### Other chips on the board (from the same pass)

| bus | pins | what it is |
|---|---|---|
| companion / PMIC | `PC14` clock, `PB2` data, `PD0` reset pulse | battery + charger gauge: `FUN_0800687C` returns the pack voltage (10-bit reading + 875/760/640, x 10000 uV), registers 2/3/5/7/10/11, polled from the main loop by `FUN_08017BB4` |
| BK4815/BK4829 | bit-banged | register layer is `FUN_080220A0(reg, val)` write / `FUN_080180F0(reg)` read (used by the T/R path `FUN_08016228`); reg `0x67` is the RSSI (squelch decision in `FUN_080052B8`, debug string `RSSI R67 %d`), `0x65`/`0x63` are read alongside it |
| SPI NOR | 16-bit serial: `FUN_08017FE4` (read) / `FUN_08018060` (write) | external flash |
| LCD panel | `PA8`-`PA11` + `PB15` | see `ra89r_lcd.md` |
| lamp | `PA1` + `PA5` | see "Backlight / lamp" above |

### Loading the current decode into Ghidra

The Ghidra project in `~/Repos/h8_re` holds programs laid out by the *old*
decoder, so decompiling them gives shifted addresses.  Load the `ra89r.py`
decode instead:

```sh
python3 ra89r.py decode FIRMWARE_RA89R_20260203_V49.icf work/FIRMWARE_RA89R_20260203_V49.bin
cp work/FIRMWARE_RA89R_20260203_V49.bin work/stock_v49_raw.bin
```

then import `work/stock_v49_raw.bin` as a **raw binary** with language
`ARM:LE:32:Cortex`, set its image base to **`0x08004000`**, and run
auto-analysis (1177 functions).  Forcing the language at import time and
rebasing afterwards is what matters: importing the same bytes as an ELF gives
Ghidra's `ARM:LE:32:v8`, whose missing Cortex analyzers leave the program with a
single function, and a raw import analysed *before* rebasing finds nothing
because the vector table's pointers do not resolve.

### UI strings (useful for the port)

### UI strings (useful for the port)

* `0x08022344..0x080225D0` — Bluetooth AT commands (`AT+GMR?`, `AT+BAUD=`,
  `AT+SLEEP=`, `AT+POWEROFF`, `AT+RST`, `AT+BLE_*`, `AT+BT_*`, `AT+SPKGAIN?`,
  `AT+MICGAIN?`, `AT+RING_CONN=`, ...), plus `RETEVIS RA89R` / `RETEVIS RA89G`.
* `0x08022820` — AT setup strings (`AT+WRITE_NAME=`, `AT+EAR_CONN=`,
  `AT+MICGAIN=`, `AT+SPKGAIN=`, `AT+RING_CONN=`).
* `0x08026B60..0x080271E3` — UI label pool, NUL-terminated variable-length
  entries: `CH.NAME`, `1750 TONE`, `FRE`, `CHINESE`, `ENGLISH`, `Main CH`,
  `Last CH`, bandwidths `100K/10K/50K/12.5K/2.5K/6.25K/25K/5K`, `LANG SEL`,
  `TX.SEL`, `CALL/KILL/FULL`, `SQL`, `DIS.NM`, `SCAN`, `WAKEN`, `CODE.ON`,
  `ALW.ON`, `STUN`, `CO`, `VFO`, `MENU`, `PTT`, `VOX LEV`, `MICLEV`,
  `N/W LOW`, `VOX SW`, `Spk Volume`, `Hold Time`, `Menu Time`, `BT Earphone`,
  `PTT Type`, `Led Type`, `Fre Reverse`, `Delete`, `PC Write`, `No Pairing`,
  `Pairing`, `Resetting`, `P1 Long`, `P2 Long`, `BT Switch`, `Tx Inh`, and the
  debug items `Radio/Char/BLE Ver/Carrier/Scrambler/4815 Error/4829 Error`.
* Display strings carry embedded control codes (e.g. `BT\tName`), so the UI
  layer interprets bytes, not pure text.
* `0x08011FE6..0x08012016` — incremental byte table `0x10..0x3F` followed by
  `"!\"#$%&'()*+,-./0123456789:;<=>???"` (character-index table for the
  earlier UI/code path).

## CPS (programming software) — band ranges are CPS/EEPROM data

From the earlier revision's CPS decompilation (sources `cps_decompiled/` in
`~/Repos/ra_re`, **not re-verified in this pass**):

* CPS EEPROM map: channels `7936`, channel names `4416`, band ranges `8000`,
  radio name `8048`, freq code `8080`, CTCSS/DCS `8208`, settings `8224`,
  DTMF `8256/8480`, 2-tone `8544+`, 5-tone `8800+`, contacts `10496/11520`.
* Band data: 3 bands x `{RxLo, RxHi, TxLo, TxHi}` in 10 Hz units at offset
  `8000` (`0xFFFFFFFF` = disabled). RA89R presets: `136-174 400-520`,
  `144-146 430-440`, `144-148 420-450`, `144-148 430-440`.
* The CPS rejects out-of-range frequencies ("BandOver") and clamps them to the
  nearest band edge, so 300 MHz (and the whole 174-400 MHz gap) never reaches
  the radio.
* The firmware's `0x080198BC` table mirrors the same preset list, i.e. it is the
  firmware-side default/fallback copy.

## Status of the earlier revision's claims

| # | earlier claim | now |
|---|---|---|
| 1 | `FUN_0800f250` = RF band-setup (writes `+0x45/+0x46`, MODER/ODR) | **structurally right, address shifted** -> `0x08013232`; register names corrected (`MODER +0x00`, `ODR +0x14`) |
| 2 | `'!'`/`'"'` state bytes only compared, never written | **confirmed**: the two select bodies store `0x20` into `+0x45`/`+0x46`; `0x21`/`0x22` are only ever compared (corrected addresses above) |
| 3 | band limits 136-174 / 400-520 confirmed as config data | **confirmed**, table is at `0x080198BC` |
| 4 | filter-switch cluster has no static callers | **re-confirmed** at `0x08020260` |
| 5 | `0x0800d9a2` is data, real helper `FUN_0800d9ba`/`FUN_0800db9c` | **renamed/corrected** -> `0x0801199C` / `0x08011B74` (see GPIO table) |
| 6 | `FUN_0801c6d0`/`FUN_080064f8` = DMA/streaming, `0x40020080`/`0x40020408` = DMA1_CH7/DMA2_CH1 | **consistent**: those are DMA register addresses; the DMA layer is now located at `0x08011154`/`0x080111D8`/`0x080115FC` and is used by the SPI-flash driver |
| 7 | `FUN_0800b08c` degenerate | unchanged region, not re-checked |
| 8 | "v3 vs v4 decode" story | **superseded** — the container format is exact (`ra89r.py`); only the *layout* of the old images was wrong |
| 9 | 300 MHz absent from any encoding | **confirmed** (no 300 MHz constant in the current decode) |

## Open points

1. **LCD controller part number** and the eight non-ST7565 init bytes
   (`0xFF 0x64 0x72 0xB4 0x90 0x98 0x70 0xFE`) — see `ra89r_lcd.md` §9.
2. **GB2312 font source** `0x000D0000 + idx*32` (referenced by `0x0800EA40`):
   nothing is mapped there (flash is `0x08000000..0x0805FFFF`, SRAM starts at
   `0x20000000`) and a full 16x16 GB2312 set does not fit in the image. Dump the
   external SPI flash and look for a 32-byte-stride glyph table, or trace the
   function on hardware with the language set to Chinese.
3. **SPI flash contents.** The firmware erases/programs sectors
   (`0x08018BB8`, `0x08018D38`); a dump of the chip (Winbond-class, id `0xEF16`)
   would answer (2) and probably hold voice prompts/config.
4. **RAM band/config structs** `0x20009BB8`, `0x20009C14`, `0x20009D98`
   (pointed at by the `0x080198BC` table) — contents and who fills them.
5. **Filter-switch callers**: nothing static references `0x08020260`; confirm on
   hardware whether it executes at all (breakpoint) or is dead code for RA89G.
6. **RF chip driver** is not yet mapped: the strings name BK4815/BK4829 and the
   UI has a "Carrier/Scrambler" debug page, but the 3-wire register layer was
   not identified in this pass (the `0x0800DCxx` region the earlier revision
   mentioned is EEPROM/config parsing, not RF).
7. **Keypad** — the key **codes** are extracted (see "Keypad" above: `1`, `4`-`9`
   programmable, `10`-`19` = digits, `0x20`-`0x25` menu, `0x2a`+3*k with long /
   extra-long variants) and the key state lives at `0x20009F80` (+0 flag, +3
   code).  What is **not** found: a scanner for a 20-button keypad -- the only
   physical key lines are `PC13`/`PB9`/`PB10` (the PTT pair, `PB9` = the
   bootloader's gate).  The earlier "ADC ladder" claim was withdrawn: the ADC
   feeds the S-meter.  **Audio mix, squelch (beyond the RSSI read), battery**
   are still open.
8. ~~Bootloader upload protocol~~ — **done**: see `ra89r_bootloader.md` (frames,
   commands, baud table, record handling) with `tools/ra89r_flash.py` and
   `ra89r.py mkicf` as the working host-side implementation.  Remaining unknowns
   are listed there (update-mode key combination, `E1`/`E3` semantics).

## Reproduce

```sh
python3 ra89r.py verify FIRMWARE_RA89R_20260203_V49.icf     # baseline 0x66, all 72 records valid
python3 ra89r.py mkicf my_firmware.bin my_firmware.icf # wrap an image for the bootloader
python3 ra89r.py decode FIRMWARE_RA89R_20260203_V49.icf work/FIRMWARE_RA89R_20260203_V49.bin
python3 tools/ra89r_analyze.py work/FIRMWARE_RA89R_20260203_V49.bin
```
