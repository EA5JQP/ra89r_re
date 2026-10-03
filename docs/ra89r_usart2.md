# USART2 — the unnamed UART

**Status: identified as a *vestigial bring-up*, not a device link.**  The stock
initialises USART2 (`0x40004400`) on `PA2`/`PA3`, AF2, 115200 8N1, and enables
its NVIC interrupt — but it never transmits a byte, never reads one, has no
protocol, no message set and no handler, and **reconfigures its two pins to
analog mode in the very same function that brings the UART up**.  There is no
device behind USART2.  This is read out of the stock image (RA89R V49 and the
RA89G V52 cross-check); there is nothing to observe on the wire, so no radio
validation is needed — and none is possible on `PA2`/`PA3`, which are the
keypad's own resistor-ladder lines (measured on the radio, `ra89r_keypad.md`).

The earlier cross-cutting note (`ra89r_findings.md`, "The three UARTs the stock
brings up") listed USART2 as unnamed and asked which of USART2/USART3 is the
Bluetooth link.  The Bluetooth link is **USART3** (`ra89r_bluetooth.md`, on
branch `driver/bluetooth`); USART2
is the one left over, and the evidence below says it is dead code rather than a
device.

## Identity and wiring

| | |
|---|---|
| peripheral | **USART2**, base `0x40004400` |
| TX | **`PA2`** (GPIOA mask `0x4`) |
| RX | **`PA3`** (GPIOA mask `0x8`) |
| framing | **115200 8N1**, alternate function **2** (`GPIO_AF2_USART2`) |
| bring-up | `FUN_080072A4` — the only place `0x40004400` is ever written |
| handle | RAM `0x2000034c` — written and used **only** by `FUN_080072A4` |
| IRQ | `USART2_IRQn = 38` (`0x26`), enabled in the NVIC, vector = default handler |

The base, the pins and the rate are all read out of the stock, not assumed:

* The literal pool of `FUN_080072A4` holds **`0x08007378 = 0x40021000`** (RCC),
  **`0x0800737c = 0x40004400`** (USART2) and **`0x08007380 = 0x2000034c`** (the
  handle).  `0x40004400` occurs **exactly once** in the whole 145,772-byte
  image, at `0x0800737c`.
* `RCC_APB1ENR` is at RCC `+0x1c`; `FUN_080072A4` ORs `0x20000` into it, which
  the vendor header names `RCC_APB1ENR_USART2EN` (bit 17).  (`USART3EN` is bit
  18, `USART1EN` is in `APB2ENR` bit 14.)
* The two `GPIO_Init` calls pass `GPIOA` (`0x48000000`) with mask `4` then mask
  `8`; the config struct is `{mask, mode = 2 (AF), pull = 1, speed = 3,
  af = 2}`.  `GPIO_AF2_USART2` is `0x02` in `py32f403_hal_gpio_ex.h`, and the
  PY32F403 maps USART2 to `PA2`/`PA3` at AF2.

## The bring-up — `FUN_080072A4`

`FUN_080072A4` has **one caller**, `FUN_08005460`, and `FUN_08005460` has one
caller, the board bring-up `FUN_0801D718`:

```
FUN_0801D718 (board bring-up)
  FUN_08020B48(0xE100)     USART1 = console, 57600
  FUN_08020BE4(0x1C200)    USART3 = Bluetooth, 115200
  FUN_08005460             ADC/keypad scan init  <-- calls FUN_080072A4
```

`FUN_08005460` itself:

```
FUN_080072A4()            USART2 bring-up
FUN_08004E58(0)           the 6-channel ADC scan (channels 2,3,6,7,8,9)
FUN_080108C0(...)         ADC calibration
FUN_08010DB8(..., 0x30)   start the DMA conversion
```

`FUN_080072A4` does four things:

1. **Clock.** `RCC->APB1ENR |= 0x20000` (`USART2EN`).
2. **Handle.** Fills the struct at `0x2000034c`: `[0] = 0x40004400`,
   `[1] = 0x1C200` (115200), `[5] = 0xC` (`RE|TE`), everything else zero, then
   calls the generic UART initialiser `FUN_0801321A`.  That runs the reset/BRR
   path (`FUN_0802037C`) and finally sets `CR1.UE`; the resulting `CR1` is
   `0x200C` = `UE|TE|RE`, with **no interrupt-enable bits**.
3. **Pins.** `GPIOA` masks `0x4`/`0x8` → `PA2`/`PA3` as AF2 (above).  Note that
   the shared per-UART pin dispatcher `FUN_08013298` has branches for USART1
   (`0x40013800`) and USART3 (`0x40004800`) **only** — it does nothing for
   USART2, so this explicit `GPIO_Init` is the only place the pins are muxed.
4. **Interrupt.** `FUN_08011C9C(0x26, 0, 1)` sets the priority and
   `FUN_08011C74(0x26)` writes `NVIC_ISER` (it is `NVIC_EnableIRQ`), i.e. IRQ
   **38 = `USART2_IRQn` is enabled**.

## Why it cannot be a device link

Four independent facts, each fatal on its own:

1. **The base appears once.**  `0x40004400` is a literal in exactly one place
   (`0x0800737c`, `FUN_080072A4`).  Nothing else in the image can touch a USART2
   register.
2. **The handle is used once.**  `0x2000034c` is referenced only by
   `FUN_080072A4` (writes at `0x080072d4`/`0x080072f2`/`0x080072f6`) and by the
   boot scatter table (it is the destination of the second decompressed region;
   see below).  No generic TX/RX helper is ever handed this handle.
3. **The interrupt is inert.**  `CR1 = 0x200C` sets no `RXNEIE`/`TXEIE`/`TCIE`/
   `PEIE`, and `CR3` is cleared, so no USART2 event can assert IRQ 38.  Even if
   one did, the vector table at `0x08004000` gives IRQ 38 the **default
   handler**: entry `0x080040D8 = 0x0800415F`, i.e. `0x0800415E` = `b .` — an
   infinite loop, not a USART2 ISR.
4. **The pins are taken over immediately.**  `FUN_08005460` calls `FUN_080072A4`
   and then `FUN_08004E58(0)` in the next statement.  `FUN_08004E58` →
   `FUN_08010B4C` → `FUN_08010CC0`, which runs

   ```
   GPIOA (0x48000000), mask 0xCC, mode 3 (analog)
   GPIOB (0x48000400), mask 0x03, mode 3 (analog)
   ```

   `0xCC` is `PA2|PA3|PA6|PA7` and `0x03` is `PB0|PB1` — the ADC channels
   `2,3,6,7,8,9` the scan is about to read.  **`PA2`/`PA3` are switched from
   AF2 back to analog before the UART could ever be used.**

The pin takeover is not a static-only conclusion: `ra89r_keypad.md` records the
measured result — the five analog keypad lines are `PA2`, `PA3`, `PA6`, `PA7`,
`PB0` (pressing `PTT1` pulls `PA2`; `9` pulls `PA3`), i.e. the board's keypad
ladder is physically on the USART2 pins.  There is no room for a UART device on
`PA2`/`PA3`, and the stock never drives them as a UART anyway.

### The scatter-table coincidence

`0x2000034c` is also the destination of the boot scatter table's second entry
(`0x08027728`: `{src 0x0802796c, dst 0x2000034c, len 0xc0e4, fn 0x08023930}`).
`FUN_080072A4` therefore overwrites the first 36 bytes of that decompressed
region with the UART handle.  Nothing reads the handle afterwards, so this is
harmless — it is just where the vendor's template put it.  (The next variable in
that RAM region, `0x20000398`, is a separate 2 KB sector buffer used by the
external-flash writer `FUN_080190C0`; it is not a second USART2 consumer.)

## Cross-checks

| check | result |
|---|---|
| bootloader (`bootloader.bin`) | no `0x40004400` anywhere; the bootloader's only UART is USART1 |
| RA89G V52 (`Ra89G_R_..._V10W_Enable.icf`) | `0x40004400` once, at the same relative offset `0x0800737c`; `FUN_080072A4`'s bytes match the RA89R build apart from `bl` offsets; the RA89G analog config (`0x08010E0C`) also sets GPIOA mask `0xCC` |
| CPS (`cps_decompiled/`) | one `SerialPort` (`serialPort1`), i.e. one PC channel; no second port |
| USART3 | the AT module (`ra89r_bluetooth.md`); its TX path is bound to `0x40004800`, never to `0x40004400` |

## What is ruled out

| candidate | evidence |
|---|---|
| a GPS / modem / any AT device | no AT or any other string is sent or parsed on this UART; the only AT path is USART3 (`ra89r_bluetooth.md`) |
| a PMIC / IO-expander / companion chip | the pins are the keypad ADC ladder; no read or write of `DR`/`SR` exists |
| an external PA / antenna tuner | no register traffic; the PA path is `driver/pa.c` on the BK4829/BK4815 bus |
| a second console / debug port | the console is USART1 (`FUN_08020B48(0xE100)`); every debug string is printed there; USART2 has no TX call site |
| the CPS's second channel | the CPS opens a single serial port, and the stock's programming channel is USART1 |
| a hidden device reached through the handle | the handle `0x2000034c` has no reader other than the init; no literal points at it except the init pool and the scatter table |
| a bit-banged bus on those pins | `PA2`/`PA3` are AF/analog only; the bit-banged buses are the LCD, the RF 3-wire bus and the BK1080 I2C, all on other pins |

## Why it is there (inference, not evidence)

The most likely explanation is that `FUN_080072A4` is **template/vendor
boilerplate**: an "init all three UARTs" block lifted from a reference design or
from a sibling model of the same family (the codec knows UV8800/TH9000D too),
left in place after the feature that used USART2 was dropped or moved to the
keypad ADC.  That is an inference — the image only shows that the bring-up
exists and is never used.  A second, equally consistent reading is a
factory-test port that this production build never enters (the unreferenced UI
string `Test Mode` at `0x08026F1C` is *not* evidence for it: it has no code
cross-reference and is not tied to USART2).

## Open

1. **The original intent.**  Which sibling model or reference design used
   USART2, and for what, is not in this image.
2. **The `channel 2 / channel 14` board variant.**  `ra89r_keypad.md` notes an
   alternate ADC path (`FUN_0801BE90`) that scans channel 14 (`PC4`) instead of
   channel 2 (`PA2`) — on a board built that way `PA2` would be free of the
   keypad ladder, but the USART2 init is byte-identical in both stock images and
   still has no consumer, so the conclusion does not change.
3. **A scope check is not worth it.**  There is no TX call site to trigger and
   the pins are the keypad, so a capture on `PA2`/`PA3` would show the ladder,
   not a UART.  Nothing on the radio can distinguish this build from one with
   `FUN_080072A4` deleted, which is why no driver is offered.

## No driver

A transport driver would be **premature**: there is no protocol, no message set
and no owning feature to drive, and adding one would be dead code that also
contends for the keypad's `PA2`/`PA3`.  The deliverable for this feature is this
write-up.  If a future unit or sibling model is found whose USART2 *is* wired,
`FUN_080072A4` (above) is the complete bring-up to reproduce, and the transport
can then be written against a real peer.

## Reproduce

```sh
python3 tools/ra89r.py decode FIRMWARE_RA89R_20260203_V49.icf work/stock_v49_raw.bin
# the base is referenced once:
python3 - <<'EOF'
import struct
d=open('work/stock_v49_raw.bin','rb').read()
b=struct.pack('<I',0x40004400); offs=[]; i=0
while (j:=d.find(b,i))>=0: offs.append(hex(0x08004000+j)); i=j+1
print(offs)          # ['0x800737c']
EOF
```

In Ghidra (`stock_v49_raw.bin`, base `0x08004000`): decompile `0x080072A4`
(bring-up), `0x08005460` (its only caller), `0x08004E58` → `0x08010B4C` →
`0x08010CC0` (the pin takeover), and read the vector at `0x080040D8` (IRQ 38).
