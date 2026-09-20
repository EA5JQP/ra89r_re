# RA89R keypad

The 20 buttons, the five-line analog matrix they sit on, the vendor's decode table,
the K5V3 key codes this firmware returns, and the console monitor that checks it.
This is the feature's own write-up; `ra89r_findings.md` keeps the cross-cutting
material (address map, UI strings, the dispatcher).

Implementation: `firmware/App/driver/keypad.{c,h}`, branch `driver/keypad` (merged).
Console: `k` for the monitor.

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
| `0x14` | `PA6` | `KEY_MENU` | the stock's menu is on its held code `0x20` (`FUN_0800C41C`), and an owner sweep of every key confirms F is this code |
| `0x17` | `PA6` | `KEY_EXIT` | the stock's invalid/back beep is on its held/extra codes, and the same sweep confirms AB is this code |
| `4`-`6` / `7`-`9` | `PA2` | `KEY_SIDE1` / `KEY_SIDE2` | the only codes with three press types = the CPS's "Side1/Side2 Short/Long" settings, and a per-key check confirms the order: SIDE1 reads `0x534` (the `{4,5,6}` window) |
| `0x18` / `0x19` | `PA3` | `KEY_STAR` / `KEY_F` | the remaining pair, by keypad row position (`9 * 0 #`) |

This pair was briefly bound the other way round, from a *spoken* label in a
single-key test ("pressing KEY_MENU" holding `PA6` at the A tap, code `0x17`),
which contradicted the stock handlers.  The label was wrong, not the handlers: the
owner's sweep of every key -- press each, read the code -- shows F is `0x14` (the
D tap, whose held code `0x20` is the menu, `FUN_0800C41C`) and AB is `0x17` (the
A tap, whose held/extra codes land on the invalid/back beep, `FUN_0801880c(0x38)`
in every menu context).  Both readings now agree, and the lesson is worth keeping:
a per-key sweep settles a mapping; a single spoken label does not.

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
