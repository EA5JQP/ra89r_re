# RA89R Bluetooth

**Status: mapped from the stock image; driver + host test complete; the missing
enable line is found — the module is held in reset on `PD0` until the stock's
"BT Switch" turns Bluetooth on (branch `driver/bluetooth`).**  The protocol work
is solid — every `AT+…` command string and every `+IM_*` event is taken
byte-for-byte from the image and checked by the host test (80 checks).  The
earlier runs failed for a hardware reason that is now identified: the stock holds
the Jieli module in reset on **`PD0`** (GPIOD bit 0) and only releases it when
Bluetooth is enabled, so a firmware that never drives PD0 sees the same idle byte
`0x51 'Q'` with a framing error at every rate and every command — an idle/
floating line, not a response.  `bluetooth_init()` now configures PD0 as an
output and releases the module (the stock's own low→high reset pulse) before the
first command, and console `y` prints the PD0 state and captures the module's
replies.  **VALIDATED on the radio:** with the buffered capture, the module
answers in full at 115200 8N1 — `AT+GMR?` returns
`+IM_VERSION:YBT100_FW_V01_02_012`, so it is a Jieli **YBT100**.  The earlier
"distorted waveform" was the bench dropping bytes, not the module.  See
"On-radio result" and "The BT Switch and the module reset line (PD0)".

The owner's teardown says the board carries a **Jieli** Bluetooth audio chip; the
firmware agrees and adds the detail: it is a Jieli **"AT" module**, the firmware
variant whose host interface is a line-based AT command set (the same command
family appears in Jieli's AC63/AC69 `CONFIG_APP_AT_CHAR_COM` documentation).  The
MCU talks to it on **USART3** with `AT+...\r\n` commands and parses `+OK`,
`+ERROR` and `+IM_*` event lines.

## Identity and wiring

| | |
|---|---|
| part | Jieli Bluetooth audio SoC, AT-command firmware (owner's teardown + the command set) |
| bus | **USART3**, base `0x40004800` |
| TX | **PB10** — GPIOB mask `0x400` |
| RX | **PB11** — GPIOB mask `0x800` |
| framing | **115200 8N1**, alternate function **2** |
| data path | DMA (`DMA1` at `0x40020000`): TX through one channel, RX into a `0x420`-byte buffer at `0x20003844` |

The pins are read out of the stock, not assumed.  `FUN_08013420` is the USART3
bring-up: it is entered from `FUN_08013298` only when the UART handle's base word
equals `DAT_0801341c`, which is **`0x40004800`**, and it configures GPIOB mask
**`0xC00`** as alternate function, mode AF, pull-up, speed high, **AF 2**
(`local_1c = 0xc00`, `local_18 = 2`, `local_14 = 1`, `local_10 = 2`,
`local_c = 2`, then `FUN_0801199c(GPIOB, ...)`).  `0xC00` is PB10|PB11, and the
vendor header gives `GPIO_AF2_USART3` for this part (`py32f403_hal_gpio_ex.h`).

The baud and the peripheral base are both in the handle initialiser
`FUN_08020bf4`: `*handle = 0x40004800`, `handle[1] = baud`.  Its only caller,
`FUN_08020be4`, is called from the board bring-up `FUN_0801d718` as
`FUN_08020be4(0x1c200)` — **115200**.  The same initialiser writes `handle[5] =
0xc`, which `FUN_0801321a` -> `FUN_0802037c` ORs into CR1: `0xc` is `RE|TE`, and
every other CR1/CR2/CR3 field stays 0, i.e. **8 data bits, no parity, 1 stop
bit, 16x oversampling**.  So USART3 is wired exactly like the console USART1 in
`driver/uart.c`, only on the other UART and the other pins.

## The bring-up

`FUN_0801d718` is the stock's board bring-up, and it is where the Bluetooth UART
is initialised, between the console and the panel:

```
FUN_0801d718:
  FUN_0801b054()          (empty stub)
  FUN_080241f8(0x32)      delay
  FUN_0801e720(1000)      TIM?
  FUN_0801e75c(0x6e09)    TIM?
  FUN_08020b48(0xe100)    USART1 = console, 57600
  FUN_08020be4(0x1c200)   USART3 = Bluetooth, 115200   <-- here
  FUN_08005460()          USART2 + keypad/ADC scan
  FUN_0800a8f4()          DAC (PA4/PA5)
  FUN_08013c74()          GPIO: LED, PC13/PC15, PD0   <-- PD0 = BT reset, left LOW
  FUN_0801370c()          RF bus PA12/PB12/PB13
  FUN_08013838()          PA12/PB8/PB12
  ...
```

`FUN_08013c74` configures PD0 as an output (GPIOD mask `1`) and its tail
`FUN_08013c24` clears every GPIOD pin (`BRR = 0xFFFF`), i.e. **PD0 low = the
module held in reset** right after boot.  The application only releases it when
Bluetooth is enabled (next section), which is why a firmware that never drives
PD0 never sees the module.

`FUN_08020be4(baud)` does two things:

* `FUN_08020bf4(baud)` — fills the handle at **`0x20003c50`** (base, baud, the
  `0xc` CR1 bits) and calls `FUN_0801321a`, which resets the UART, computes BRR
  (`FUN_0802037c`, from the handle's baud and the APB1 clock), and on a fresh
  handle calls `FUN_08013298` -> `FUN_08013420` (the PB10/PB11 AF2 setup plus
  the DMA channels);
* `FUN_08020bcc()` — `FUN_08013598(handle, 0x20003844, 0x420)`, i.e. arms the
  **receive DMA**: `0x420` (1056) bytes into the buffer at `0x20003844`, with
  the UART's error interrupt and DMAR/DMAT set.

The register map matches the vendor header: base `0x40004800`, `SR 0x00 DR 0x04
BRR 0x08 CR1 0x0c CR2 0x10 CR3 0x14`, GPIO `MODER/OTYPER/OSPEEDR/PUPDR/AFR` and
`RCC APB1ENR` bit 18 = `USART3EN`.

### Transmit

```
FUN_08022f68(str)
   -> strlen, store the length at state+0x48
   -> memcpy(str, state+0x15, len)          (a shadow copy in the state struct)
   -> FUN_08020c2c(str, len)
FUN_08020c2c(str, len)
   -> busy-wait on *0x2000001c == 0         (DMA-busy flag)
   -> memcpy(0x20003a49, str, len)          (the DMA source buffer)
   -> FUN_0801366c(0x20003c50, 0x20003a49, len)   (start the DMA transfer)
```

So a command goes out through USART3's TX DMA.  `FUN_0801366c` is the generic
DMA-start helper (its handle's first word is the peripheral base, `handle[0xf]`
the DMA channel), which is why it is shared with the console.

### Receive

The receive DMA lands in `0x20003844` and the **main loop** picks it up (this is
not an interrupt-driven parser):

```
FUN_08024448 (main loop)
  -> FUN_08020d14 -> FUN_08020c68          (frame-level dispatch)
        -> FUN_08022900(0x20003844, *(u16 *)0x2000001e)
              -> FUN_08022974(line, len)   (the response parser)
  -> FUN_0801a228 (the app loop)
        -> FUN_08022900(0x20003844, *(u16 *)0x2000001e)
```

`FUN_08022900` splits the buffer on `\r\n` and calls `FUN_08022974` once per
line.  `FUN_08020c68` first looks at a command byte inside the frame descriptor
`0x20009f9c` (`+0x12`); for the binary opcodes `0xE0`, `0xE2`–`0xE7`, `0xEB` it
calls a handler directly, and otherwise falls through to the text line parser.

## The BT Switch and the module reset line (PD0)

The UART bring-up alone does **not** power the module: the stock holds it in
reset on **PD0** (GPIOD, mask `1`) and only releases it when Bluetooth is turned
on.  The earlier "there is no BT power/enable pin" reading missed it because PD0
*is* in the bring-up, but as a reset the application drives rather than as a
static GPIO setup (the "companion gauge" note had also claimed PD0 — see below).

**The menu path.**  The string `BT Switch` sits at **`0x0802702C`**; it is the
first item of the `BT Menu` descriptor at **`0x080256AC`** (title `BT Menu` at
`0x080271E0`, then the item-label pointers), inside the menu-descriptor table
**`0x080256D4`** (the table `FUN_08008124` renders from).  The BT menu state
machine is **`FUN_0800C588`**; its first item (case 0) calls
**`FUN_08009660(value)`**.

**`FUN_08009660(value)`** is the on/off setter:

* it writes `value` to `config[0x38]` — the settings struct at `0x20009F28`,
  i.e. **codeplug settings byte 9 bit 0, the CPS "Bluetooth" bool** — and
  mirrors it to the external flash via `FUN_080193E4`;
* **off** (`value == 0`): queues `AT+BT_DISCN` (`FUN_08022664(0x22, …)`), clears
  the connection flags, waits 400 ms, then drives **PD0 LOW**.  The raw write is
  `GPIOD BRR = 1` — `FUN_08011B74(0x48000C00, 1, 0)` at `0x080096B2`–`0x080096B8`;
* **on** (`value != 0`): calls **`FUN_0801D69C`**, which drives PD0 low
  (`GPIOD BRR = 1`, `0x0801D69E`), does the module handshake, then drives PD0
  **HIGH** (`GPIOD BSRR = 1`, `0x0801D6DA`) — an active-low reset pulse.

So **PD0 high = module released/running; PD0 low = held in reset.**

`FUN_0801D69C` is the module bring-up proper, not a gauge reset: it reads a
settings byte (`0x08025FF3`), copies two tables, loads the eight paired-device
records from the external flash (`FUN_080106A4`: 8 × 16-byte names + 8 × 64-byte
records) and clears the ready flag — all Bluetooth state.  It is called from:

* `FUN_08009660` (the BT Switch on path, `0x0800968C`);
* the power/mode path `FUN_0801B058` (case `0x20`), **only when
  `config[0x38] != 0`** (`0x0801B0EE`);
* the resume path `FUN_080167F0`, again only when `config[0x38] != 0`
  (`0x08016886`).

The periodic check `FUN_080066CC` (under the time-slice flag `0x08` of
`FUN_0801E034`, which the main loop `FUN_08024448` calls at `0x080244A6`) reads
PD0 back (`FUN_08011B64(GPIOD, 1)` = `GPIOD->IDR & 1`) and re-pulses it via
`FUN_08006850` while `config[0x38] == 1` and the module has not reported ready —
a reset retry, and further proof the line is the module's reset.

**Is it on by default?  No.**  The stock drives PD0 high only when the codeplug
Bluetooth bool is set, and that bit defaults to **0** (`ra89r_codeplug.md`, byte 9
bit 0, "this radio" = 0), so the stock itself leaves the module in reset.  This is
also why a firmware that never touches PD0 sees exactly the observed idle line:
the module is simply not running.

**Not a gauge.**  `ra89r_battery.md` read PD0 as the "companion gauge" reset, and
`FUN_0801D69C` as the gauge's boot handshake.  That gauge was a dead end (the
PC14/PB2 bus is the BK1080 FM receiver — `ra89r_bk1080.md`), and the function it
names is this same Bluetooth bring-up, reached only from the BT paths above.

## The AT command set

`FUN_0802213c` is the command sender.  It indexes a command list held in a RAM
state struct at **`0x20009ade`** (`DAT_08022340`):

| offset | meaning |
|---|---|
| `+0x01` | the parameter value string for the parameterised commands |
| `+0x4a` | the command list (one byte per command, a `bt_cmd_t`) |
| `+0x54` | number of commands queued |
| `+0x55` | current command index |
| `+0x56` | error flag (set on `+ERROR` for commands that may not fail) |
| `+0x58` | response state (1 or 3, set per command) |

`FUN_0802213c` reads `state[state[0x55] + 0x4a]` and switches on it.  Indices
`0..9`, `0x0d..0x27` are fixed strings sent verbatim by `FUN_08022f68`; indices
`0x0a`, `0x0b`, `0x0c`, `0x19`, `0x24` are parameterised and go through
`FUN_08022770`.  The strings are the stock's own constants, `\r\n` included:

| idx | command | idx | command |
|---|---|---|---|
| 00 | `AT+GMR?\r\n` | 0x14 | `AT+BLE_SCANATCN=OFF\r\n` |
| 01 | `AT+BAUD=1\r\n` | 0x15 | `AT+BLE_SCAN=ON\r\n` |
| 02 | `AT+BAUD=2\r\n` | 0x16 | `AT+BLE_SCAN=OFF\r\n` |
| 03 | `AT+SLEEP=ON\r\n` | 0x17 | `AT+BLE_DISCN\r\n` |
| 04 | `AT+SLEEP=OFF\r\n` | 0x18 | `AT+BLE_CONN_LAST\r\n` |
| 05 | `AT+POWEROFF\r\n` | 0x19 | `AT+RING_CONN=<v>\r\n` *(param)* |
| 06 | `AT+RST\r\n` | 0x1a | `AT+BT_LOCAL?\r\n` |
| 07 | `AT+CONN_STATE?\r\n` | 0x1b | `AT+LAST_EAR?\r\n` |
| 08 | `AT+SPKGAIN?\r\n` | 0x1c | `AT+BT=EMITTER\r\n` |
| 09 | `AT+MICGAIN?\r\n` | 0x1d | `AT+BT=RECEIVER\r\n` |
| 0a | `AT+MICGAIN=<v>\r\n` *(param)* | 0x1e | `AT+BT_SCAN=ON\r\n` |
| 0b | `AT+SPKGAIN=<v>\r\n` *(param)* | 0x1f | `AT+BT_SCAN=OFF\r\n` |
| 0c | `AT+WRITE_NAME=<v>\r\n` *(param)* | 0x20 | `AT+BT_SCANATCN=ON\r\n` |
| 0d | `AT+BLE_LOCAL?\r\n` | 0x21 | `AT+BT_SCANATCN=OFF\r\n` |
| 0e | `AT+LAST_RING?\r\n` | 0x22 | `AT+BT_DISCN\r\n` |
| 0f | `AT+BLE_SLAVE=ON\r\n` | 0x23 | `AT+BT_CONN_LAST\r\n` |
| 10 | `AT+BLE_SLAVE=OFF\r\n` | 0x24 | `AT+EAR_CONN=<v>\r\n` *(param)* |
| 11 | `AT+BLE_MASTER=ON\r\n` | 0x25 | `AT+BT_PAIRCLR\r\n` |
| 12 | `AT+BLE_MASTER=OFF\r\n` | 0x26 | `AT+BT_CALL=ON\r\n` |
| 13 | `AT+BLE_SCANATCN=ON\r\n` | 0x27 | `AT+BT_CALL=OFF\r\n` |

`FUN_08022770(cmd)` builds the parameterised five.  The prefixes are at
`0x08022820`ff: `AT+WRITE_NAME=`, `AT+EAR_CONN=`, `AT+MICGAIN=`, `AT+SPKGAIN=`,
`AT+RING_CONN=`.  It appends the value from `state+1` and then `\r\n`
(`DAT_08022868 = 0x0a0d`).  The gain values are **not** ASCII built on the fly:
they come from small tables — `FUN_0802286c(level)` for the microphone and
`FUN_080226d8(level)` for the speaker — whose entries are the two-byte strings
**`"0"`, `"5"`, `"6"`, `"7"`, `"8"`, `"9"`** (mic, six entries) and `"0"`,
`"4"`, `"8"`, `"16"`, `"23"`, `"31"` (speaker, six entries) at
`0x08024e64`ff.  The literal pool confirms the index map
(`0x080228E8`..`0x080228FC` → `0x08024E64`..`0x08024E6E`, stride 2), so the mic
table is six entries, not five; the port's table was short one (`"9"`).  The
level is read from the config struct (`+0x3b` mic, `+0x3c` speaker) and passed
straight to the table, so codeplug byte 8's 1..5 range indexes entries 1..5.

## The response parser — `FUN_08022974`

The parser is a longest-match chain over the line's prefix, in this order (all
prefixes are the stock's own constants; `+OK` and `+ERROR` include the `\r\n`):

| line prefix | event | what the stock does |
|---|---|---|
| `RDTP` | binary frame | the binary control path (below) |
| `+IM_READY\r\n` | ready | set `state+9`, delay, `FUN_08007fd0()` (start pairing/scan) |
| `+OK\r\n` | ok | advance the command queue (`FUN_080225e0`) |
| `+ERROR\r\n` | error | advance for commands that may fail, else set `state+0x56` |
| `+IM_VERSION:` | version | `FUN_0800e1e8` stores `"BT VER V"` + the reported string |
| `+IM_BT_EMITTER` | emitter mode | advance |
| `+IM_NAME_EQUALLY` | name set | mark `state+5`, write the name setting, advance |
| `+IM_BT_RECEIVER` | receiver mode | advance |
| `+IM_BLE_MASTER` | BLE master | advance |
| `+IM_BLE_SLAVE` | BLE slave | advance |
| `+IM_CONN_STATE:` | connection state | `FUN_0800e1ac` stores the state bytes |
| `+IM_SCO_CONN` | audio link up | `state+10 = 1` |
| `+IM_CALL_CONED` | call connected | `state+10 = 1` |
| `+IM_CALL_DISCONED` | call ended | `state+10 = 0` |
| `+IM_SCO_DISCN` | audio link down | `state+10 = 0` |
| `+IM_EARDEV:` | earpiece found | `FUN_0800f9b0` adds it to the paired list |
| `+IM_BT_SCAN_STOP` | scan finished | `FUN_08019d3c()` |
| `+IM_BT_EAR_CONN` | earpiece connected | `FUN_0801ae8c()` |
| `+IM_BT_DISCN` | disconnected | `FUN_0801aee8()` |
| `+IM_EAR_PTT_KEYDOWN` | earpiece PTT pressed | `state+6 = 1` |
| `+IM_EAR_PTT_KEYUP` | earpiece PTT released | `state+6 = 0` |
| `+IM_BLE_LOCAL` | BLE address/name | copy the address, advance |
| `+IM_BT_LOCAL` | BT address/name | copy the address, advance |

Two details that are easy to miss.  A line beginning `++` has its first byte
stripped before matching (the module echoes `AT+...` as `+...`, and `++` is the
escape for a literal `+`).  And every received line is also echoed to the
**console** USART1 by `FUN_08020b90` before it is parsed — that is the only place
the BT traffic is visible, and it is the natural hook for a scope-free
bring-up.

`+OK` does not always mean "advance": `FUN_08022974` advances when the queue has
exactly one command left, or when the current command is one of `MICGAIN`,
`SPKGAIN`, `BT_SCANATCN=ON`, `BT_CONN_LAST` (indices `0x0a`, `0x0b`, `0x20`,
`0x23`).  Those are the commands whose real answer is the `+IM_...` event that
follows, not the `+OK`.  `+ERROR` likewise advances only for `BT=EMITTER`,
`BT=RECEIVER`, `BLE_MASTER=ON`, `BLE_SLAVE=ON` (indices `0x1c`, `0x1d`, `0x11`,
`0x0f`); otherwise it latches `state+0x56 = 1`.

## What the stock uses it for

The command names say most of it, and the trigger functions confirm the wiring:

| feature | function(s) | commands queued |
|---|---|---|
| turn the radio's BT on/off, pick emitter/receiver | `FUN_08009660` | `AT+BT_DISCN` on off; `FUN_0801d69c` + state on; **PD0 low on off, low→high reset pulse on on** |
| BT call audio on/off | `FUN_08007540`, `FUN_080075a0`, `FUN_0801a774`, `FUN_08006234`, `FUN_080177a8` | `AT+BT_CALL=ON` / `AT+BT_CALL=OFF` |
| pair / scan / reconnect | `FUN_08007fd0` (on `+IM_READY`), `FUN_0800c588` | `AT+BT=EMITTER|RECEIVER`, `AT+WRITE_NAME=`, `AT+BLE_LOCAL?`, `AT+BT_SCANATCN=ON`, `AT+BT_CONN_LAST`, `AT+BT_SCAN=ON`, `AT+BT_PAIRCLR` |
| connect a known earpiece | `FUN_0800c588` (`FUN_08022664(0x24, record)`) | `AT+EAR_CONN=<addr>` |
| gains, name, ring | `FUN_0802213c` cases 0x0a/0x0b/0x0c/0x19 | `AT+MICGAIN=`, `AT+SPKGAIN=`, `AT+WRITE_NAME=`, `AT+RING_CONN=` |
| earpiece PTT keys | parser `+IM_EAR_PTT_KEYDOWN/UP` -> `state+6` | (event only) |

The mode and pairing state lives in a second RAM struct at **`0x200094e4`**
(`DAT_080096cc`, `DAT_0800c94c`, `DAT_08007644`, ...): `+0x9` is the ready flag,
`+0x6` the earpiece-PTT flag, `+0xd`/`+0xf` the connection flags, `+0x10` the
call flag, `+0x18` the paired-device records (13 bytes each), `+0x280` the record
index, and `+0x5f4` the state-machine step.  The BT *mode* itself is
`config[0x38]` in the settings struct at `0x20009f28`; `FUN_08009660` is its
setter and mirrors it into the external flash at `[*0x08025ff8] + 9`
(`FUN_080193e4`), i.e. the CPS's own BT setting.  `FUN_0800c588` is the
state machine.

The UI strings agree with all of this: `BT Earphone`, `BT Switch`, `Pairing`,
`No Pairing`, `Paired`, `Paired Dev`, `BT PTT`, `BT APP Talk`, `BT Menu`
(`0x08026d60`ff).

### The second, binary path — `RDTP`

Alongside the AT text there is a binary control protocol, and it is **not fully
decoded**:

* The MCU *builds and sends* it in `FUN_0801a828` when its argument is non-zero:
  it computes a checksum with `FUN_08015122` (a byte sum starting at offset 5),
  writes the four bytes at `0x0801a8d0` — `"RDTP"` — at the start of the frame,
  appends `0xFD`, and sends it through the same USART3 TX path.  Its callers are
  `FUN_08005f90`, `FUN_0801a08c`, `FUN_0801a1d4`, `FUN_0801a818`, and those build
  frames with opcodes `0xE1`, `0xE6`, `0xE8`, `0xE9`, `0xEA`, `0xEC`, `0xEE`.
* The MCU *dispatches* frames whose first four bytes are `"RDTP"` and whose next
  five bytes are `fe fe ee ef e0|e2|e3|e4|e5|e7|eb|ed` in `FUN_08022974`, to the
  same handlers `FUN_08020c68` uses for the opcodes `0xE0`, `0xE2`–`0xE7`,
  `0xEB`.

So the chip has both a line AT interface and a framed binary interface; the
binary one appears to carry the call/audio/PTT actions.  What the five-byte
`fe fe ee ef eX` prefix means, and whether those bytes are a fixed header or a
mask, is **open** (see below).  The driver only implements the AT half.

## What was ruled out

| candidate | evidence |
|---|---|
| I2C | the only two-wire bus on the board is `PC14`/`PB2`, and that is the **BK1080 FM receiver** (`FUN_08007034`/`FUN_08007158`, device id `0x80`, reg `0x0b` = RSSI) — see `ra89r_battery.md`.  `I2C1`/`I2C2` (`0x40005400`/`0x40005800`) are referenced nowhere in the image. |
| SPI | `SPI2`/`SPI3` (`0x40003800`/`0x40003c00`) are referenced nowhere; SPI1 is the external NOR flash. |
| USART1 | that is the 57600 Kenwood/programming port (`FUN_08020b48(0xe100)`), and it is where the BT lines are *echoed* (`FUN_08020b90`), not where the chip is. |
| USART2 | `FUN_080072a4` brings it up at 115200 on PA2/PA3, but nothing sends AT commands through it; the AT TX path is bound to the `0x40004800` handle. |
| a bit-banged bus | the AT path is a real UART with DMA and `BRR`; there is no bit-bang in it. |

## The driver

Two files on branch `driver/bluetooth`:

| file | what it is |
|---|---|
| `firmware/App/driver/bluetooth.h` | the command/event enums, the pure command-table and line-parser interface (device-header free) |
| `firmware/App/driver/bluetooth.c` | the parser, the parameterised builder, and the USART3 + GPIO bring-up |

The header keeps the parser and the command table free of any MCU include so
they can be checked on a PC; the hardware half lives in `bluetooth.c` under
`#ifndef BLUETOOTH_HOST_TEST`, exactly as `beeper.h`/`beeper.c` split the tone
math from the DAC.

What the driver does **not** do, deliberately:

* it does not implement the binary `RDTP` path — the evidence stops at the frame
  header and the opcode set;
* its transport is **polled**, not DMA.  The stock uses DMA for both directions;
  a polled USART3 is the smaller thing that can be got right without a radio, and
  the DMA bring-up is recorded above for whoever validates it.  This is the main
  thing to revisit on hardware.

### The bring-up test

The host test checks the half a radio cannot: that the exact bytes the stock
would put on the wire are the bytes the driver puts on the wire.

```sh
cd firmware && gcc -std=c11 -I App -I App/driver -DBLUETOOTH_HOST_TEST \
    tools/test_bluetooth.c App/driver/bluetooth.c -o /tmp/test_bluetooth \
    && /tmp/test_bluetooth
```

It replaces `bluetooth_hw_write()` with a recording stub and asserts: every one
of the 40 command strings byte for byte; the five parameterised builders; every
`+IM_*` / `+OK` / `+ERROR` line classifying to the right event; and the
CRLF-splitting receive path delivering whole lines to the callback.

The bench's timed capture logic has a separate host regression test:

```sh
cd firmware && gcc -std=c11 -I App -I App/driver \
    tools/test_bt_capture.c App/driver/bt_capture.c -o /tmp/test_bt_capture \
    && /tmp/test_bt_capture
```

It feeds a reply as bytes that become ready on separate polls, with idle polls
between them, and checks that the full sequence and accumulated error flags are
retained.

## On-radio result: VALIDATED — the module answers in full at 115200

**The AT link works.**  After the buffered capture fix, console `y` on the radio
reads complete, clean `+IM_*` lines with `FE` clear at 115200 8N1.  The module
identifies itself:

```
AT+GMR?  ->  +IM_VERSION:YBT100_FW_V01_02_012
```

so it is a **Jieli YBT100**, firmware **V01.02.012**.  The reset banner it emits
on PD0 low→high (with the leading `0x00` break byte) is:

```
+IM_BLE_SLAVE
+IM_BPHONE_DISCN
+IM_BT_EMITTER
+IM_BT_DISCN
+IM_READY
+IM_BT_SCAN_STOP
+IM_BT_SCAN_STOP
```

`+IM_BT_EMITTER` is the module's default mode; `+IM_READY` is the event the
stock's parser keys on to start pairing/scan.  `+IM_BPHONE_DISCN` is **not** in
the stock's prefix table (the parser has `+IM_BT_DISCN` but not the BPHONE
variant), so it classifies as `BT_EV_NONE`; the rest map to the expected events.

The UART registers are as documented: `CR1 = 0x200c` (UE|TE|RE),
`BRR = 0x01a1` (48 MHz / 115200), `CR3 = 0`, PB10/PB11 in AF2, PB11 idling high.

**PD0 is the module's reset, and releasing it is required.**  The stock holds the
module in reset on **PD0** and only releases it when Bluetooth is enabled (see
"The BT Switch and the module reset line (PD0)").  A firmware that never drives
PD0 leaves the line idle (`0x51`).  `bluetooth_power(true)` pulses PD0 low→high
and the module boots.

**The earlier failures were the diagnostic, not the module.**  The old `y` bench
checked USART3 every 10 ms and printed synchronously while receiving.  At 115200
a byte takes ~87 us, so a multi-byte reply was lost after the first byte (the
`SR=0x00f8` seen then had `FE` clear and `ORE` set — an overrun, not a framing
error).  The "distorted waveform / hardware limit" reading was an artifact of
that bench and is withdrawn.  The bit probe's `low 25 / high 51` was likewise
normal: the low run is the start bit, the high run is the two leading one-bits of
`'+'` (`0x2b`, LSB first), not a half-width start bit.

**The diagnostic capture fix (validated).**  `bt_listen` now continuously polls
USART3 for the requested interval, buffers bytes and accumulated `PE/FE/NE/ORE`
flags, prints only after capture ends, and feeds the bytes through the stock
parser so the event classification runs on real lines.  A host regression test
exercises a delayed multi-byte reply across idle polls.

## The BT menu items (stock reversal)

The stock's `BT Menu` descriptor (`0x080256AC`, title `BT Menu` at
`0x080271E0`) has nine items; `FUN_0800C588` dispatches each by an item **type**
(`DAT_0800c958[step - 1]`) to a handler.  The handlers write the codeplug
settings block and mirror it to the external flash (`FUN_080193E4`); the fields
are the ones `ra89r_codeplug.md` names.  The handler → field mapping is certain;
the menu-position → handler mapping is inferred (the type array lives in RAM):

| handler | writes | field |
|---|---|---|
| `FUN_08009660(value)` | `config[0x38]` → byte 9 bit 0 | Bluetooth on/off |
| `FUN_08019B4C(value)` | `config[0x3c]` → byte 8 bits 0–3 | speaker gain |
| `FUN_08019BDC(value)` | `config[0x3b]` → byte 8 bits 4–7 | mic gain |
| `FUN_08019BAC(value)` | `config[0x39]` → byte 9 bits 5–7 | byte 9 bit 5 speaker switch |
| `FUN_08019B7C(value)` | `config[0x3a]` → byte 9 bits 1–3 | PTT type (BT / local / both) |
| `FUN_08019B00(value)` | `config[0x3f]` → byte 7 bits 4–7 | byte 7 (the doc's hold time is bits 0–3; the handler's field is bits 4–7) |

Pairing (`FUN_0800C588` case 1) is a scan: when the module is ready it queues
`AT+BT_SCAN=ON` (index `0x1e`), and the module reports each found earpiece as
**`+IM_EARDEV:<addr>,<name>,<rssi>`** — observed on the radio as
`+IM_EARDEV:7BA245EBBE2C,Ear (stick),-68`.  The stock's `FUN_0800f9b0` splits
that on commas (scanning from offset `0xb`, the length of `+IM_EARDEV:`) into a
13-byte record (`state+0x18 + i*0xd`) and a 64-byte name (`state+0x80 +
i*0x40`); `AT+EAR_CONN=<addr>` takes the first field.  On the radio, selecting
the device connects it: the module answers `+IM_BT_EAR_CONN` and
`+IM_EAR_VOL=<n>`.  `FUN_08007FD0` (the `+IM_READY` path) instead does
scan-and-auto-connect with `AT+BT_SCANATCN=ON` and `AT+BT_CONN_LAST`.

The port's BT screen (`App/ui/bt.c`) now renders in the K1 menu's own layout
(three-row left column, inverted current item, value on the right,
index/count below) with six-character labels, and every item acts: BT Switch,
Pair, Hold, Scan, Volume, Mic and PTT set their fields and/or send the stock's
command; Paired and Info are read-outs.  Pairing uses the scan-and-auto-connect
path; the per-device pick still needs the binary `+IM_EARDEV` record captured on
the radio.

## Probing for a data (SPP / BLE-GATT) path -- result

The stock's AT set is audio/control only; there is no command in the image that
moves a data payload, and the framed `RDTP` path is call/PTT control, not a data
pipe.  Whether the module's firmware exposes a **serial (SPP)** or **BLE GATT**
data service is not in the image, so it was probed live with console `N` (which
sends an arbitrary AT line and prints the reply).  Result on the radio
(2026-10):

| sent | reply |
|---|---|
| `AT+HELP` | `+NOT_AT` |
| `AT+SPP`, `AT+SPP?` | `+NOT_AT` |
| `AT+BLE_GATT`, `AT+BLE_GATT?` | `+NOT_AT` |
| `AT+BLE_SERVICE` | `+NOT_AT` |
| `AT+BLE_ADV` | `+NOT_AT` |
| `AT+BT_LOCAL?` | `+IM_BT_LOCAL=11:C3:EF:CD:DF:5A,RETEVIS RA89R(BT)` then `+OK` |
| `AT+BLE_LOCAL?` | `+IM_BLE_LOCAL=10:C3:EF:CD:DF:5A,RETEVIS RA89R` |

So the module answers an unknown command with **`+NOT_AT`**, and its AT firmware
exposes **no SPP or GATT data command**.  It does have a BLE stack -- the
`AT+BLE_MASTER/SLAVE/SCAN` role commands work and `AT+BLE_LOCAL?` reports an
address -- so a phone can pair over BLE, but there is no AT-level way to move a
data payload.  Whether the BLE side carries a GATT data service is a question
for Jieli's own YBT100 documentation, not for this image.

The module identifies itself as **`RETEVIS RA89R(BT)`** at `11:C3:EF:CD:DF:5A`
(classic) and **`RETEVIS RA89R`** at `10:C3:EF:CD:DF:5A` (BLE).

## The BT audio path (stock behavior and port regression check)

When an earpiece connects, the stock:

- sends **`AT+BT_CALL=ON`** (`FUN_08007540`; `FUN_0801A774` when a call starts)
  to open the SCO audio link;
- sets the module's gains from the codeplug (`FUN_080075A0`:
  `AT+MICGAIN=<config[0x3b]>`, `AT+SPKGAIN=<config[0x3c]>`);
- drives **`PC13`** (`FUN_080177A8` raise, `FUN_08009C9C` lower -- GPIO mask
  `0x2000`), the audio-path/amplifier enable, gated on the BT bool and the
  connection state.

The first radio-tested port sequence (commit `0cc3de0`) sent only
`AT+BT_CALL=ON` on connect and `AT+BT_CALL=OFF` on disconnect; receive audio
from the radio was heard in the earpiece.  Commit `0bfe0cc` later added
`AT+MICGAIN` and `AT+SPKGAIN` sends at startup and immediately after
`AT+BT_CALL=ON`.  After the later `0x33` experiment was reverted and that
reverted firmware was flashed, the user still reported no BT receive audio.
The current diagnostic candidate therefore returns SCO setup to the earlier
CALL-only sequence and does not automatically write either module gain at boot
or connection.  The BT menu's explicit gain controls still send their selected
AT commands.  This isolates the gain writes as the remaining software change
after the known-working sequence; the candidate still needs radio validation.

The stock distinguishes a **BT link disconnect** from an **SCO/call disconnect**:
`FUN_08022974` clears its call-active flag for `+IM_SCO_DISCN` and
`+IM_CALL_DISCONED`; it does not send `AT+BT_CALL=OFF` there.  `FUN_080177A8`
can reopen the call when returning to RX if the earpiece is still linked.  The
port previously treated SCO/call-disconnect events as full audio shutdown and
  sent `AT+BT_CALL=OFF`. That matches the reported symptom after BT PTT (BT RX
  audio disappears and subsequent beeps are absent). The current candidate now
  keeps the BT link, avoids that OFF command, coalesces paired SCO/call-disconnect
  events, and reissues `AT+BT_CALL=ON` on the TX-to-RX transition. This fix is
  host-tested but still needs on-radio validation.

### The CALL=ON storm (on-radio capture, `U`)

A raw USART3 capture after the earpiece linked showed an endless cycle:

```
+IM_BT_EAR_CONN  +OK  +IM_SCO_CONN  +IM_EAR_VOL=127
+IM_SCO_DISCN  +OK  +IM_SCO_CONN  +IM_SCO_DISCN  +OK  ...
```

The `+OK` after each `+IM_SCO_DISCN` is the module acknowledging the port's own
`AT+BT_CALL=ON`.  Root cause: `bt_service_tick()` re-sent `CALL=ON` on every
10 ms slice while the radio sat in RX, so each SCO teardown was immediately
answered with a fresh call open.  The stock never does this -- `FUN_080177A8`
reopens CALL only on the receive T/R transition.  The port now resumes only from
`bt_set_radio_tx_active(false)` (the `tx_stop()` path); an idle SCO teardown
leaves `s_call_resume_pending` set until the next T/R transition.  Host
regression: `test_bt_service.c`, "idle-RX SCO loss does not trigger a CALL=ON
storm".  **Radio re-test pending** to confirm the module keeps SCO up once the
port stops spamming CALL=ON.

The same capture contains **no** `+IM_EAR_PTT_KEYDOWN` / `+IM_EAR_PTT_KEYUP`
(and no `+IM_EAR_SIDE_SINGLE1`) -- only the SCO churn above.  The headset's PTT
event source is therefore still unidentified; a clean capture taken *while the
headset button is pressed* is needed before changing the parser, and the churn
above must be removed first so it does not bury the real event.

### Headset PTT and the module gains (second capture, after the storm fix)

A later capture (PTT via BT working) shows the headset button itself:

```
+IM_BT_EAR_CONN +OK +IM_SCO_CONN +IM_EAR_VOL=127 +IM_SCO_DISCN
+IM_EAR_SIDE_SINGLE1 ... +IM_EAR_SIDE_SINGLE0 +IM_EAR_SIDE_SINGLE1 +OK
+IM_SCO_CONN +IM_SCO_DISCN +IM_EAR_SIDE_SINGLE0 +IM_EAR_SIDE_SINGLE1 ...
```

So the headset PTT is **`+IM_EAR_SIDE_SINGLE1`** (the module also emits
`+IM_EAR_SIDE_SINGLE0`; the port matches only `SINGLE1` and toggles, which is
what keys TX, so `SINGLE0` is left ignored), and there is still **no**
`+IM_EAR_PTT_KEYDOWN/UP`.

On earpiece connect the stock sets the module's gains (`FUN_0801AE8C` ->
`FUN_080075A0(0)`): `AT+MICGAIN=<config[0x3b]>`, `AT+SPKGAIN=<config[0x3c]>`
from codeplug **byte 8**, then opens the call.  The port had sent only
`AT+BT_CALL=ON`, leaving the module at its power-on gains.  It now reads byte 8
into `gEeprom.BT_MicGain`/`BT_SpkGain` (`codeplug_shared_settings`), seeds
`bt_set_gain_levels()` at boot, and sends `AT+MICGAIN`/`AT+SPKGAIN` before
`AT+BT_CALL=ON` on `+IM_BT_EAR_CONN`, matching the stock order.  The port's mic
table was also short one entry (`"9"`), now fixed.  **Radio validation pending:**
whether this is what makes the headset mic reach the transmitted signal; the
radio-mic mute/analog route is still unidentified, so do not assume the gain
alone fixes it.

The missing menu key beep is not specific to the BT screen: `BT_ProcessKeys`
did not request a beep, and the shared K1 `AUDIO_PlayBeep()` returned early while
`gCurrentFunction` is RECEIVE or MONITOR (`App/audio.c`). The port now routes
normal-menu feedback through `AUDIO_PlayKeyBeep()` and requests the same optional
1 kHz beep for a non-held BT-menu key. This path honors the Beep setting and
does not repeat on held events. `preview_k1` exercises both menu paths in
RECEIVE using the host beeper.

The **destination is still unverified**: `AUDIO_PlayKeyBeep()` uses the existing
PA4/DAC beeper and temporarily enables the PC13 audio path, whose physical
destination has not been established. The BT link controls remain the stock
PC13 level and BK4829 `0x33` pin-2 output; neither host readback nor the current
radio report (audio heard through both endpoints) proves exclusive routing or
that a DAC beep reaches the BT headset. Do not claim linked beeps are headset-only
until the radio is checked, and do not add a speculative route write.

For transmit, the user confirms the stock silences the radio's own mic while in
BT mode, and ordinary port TX currently still uses the radio mic.  The earpiece
mic route is therefore still open.  The BK4829 mic ADC (`0x30` bit 2) experiment
was tried and reverted: it did not select the earpiece mic.

Two more route tests/findings:

- The user toggled **PC13** high/low with console `C` and saw no change in mic
  behavior. PC13 is therefore not established as the mic-source selector; its
  stock `FUN_080177A8` behavior is tied to BT connection and the CPS speaker
  switch, but its destination remains unknown. The port mirrors that level
  policy from codeplug byte 9 bit 5; this is not claimed as the mic fix.
- The earlier port `g` test of BK4829 `0x33` pin 2 was **not stock-equivalent**.
  The stock's `FUN_080137D4(mask=4, value=4)` clears both output bit `0x0010`
  and its paired configuration bit `0x1000`, whereas the imported K1
  `BK4819_ToggleGpioOut()` changed only `0x0010`.  Thus the observed port
  `0x9040 -> 0x9050` did not reproduce the stock's `0x8040 -> 0x8050` transition.
  The attempted implementation also overwrote the whole `0x33` word in TX and
  broke TX; it was reverted. The port now uses a separate stock-semantic masked
  write for pin 2 and preserves it across TX/RX `0x33` writes. This restores the
  stock state transition but is still **not validated as a mic route** on the
  radio.

The stock's normal TX mic-gain writer is `FUN_0801C3A8` (`0x40`), whose value
comes from `FUN_08020190`; that path has no BT-connected or `config[0x3a]`
selection.  A separate UART binary-control mode can write a mic-gain override
at `0x2000A428` (`FUN_0801E1E0`, message type `0x20`), but there is not yet
evidence that this mode is entered for BT audio. The earpiece-mic route still
needs its control traced. The PC13/`0x33` restoration is for stock parity only,
not a claim that either line routes the earpiece mic.

### Stock TX mic-source trace (Ghidra, stock V49)

The focused Ghidra pass does not find a BT mic selector in the normal RF path:

- `FUN_0801C3A8` writes BK4829 register `0x40`, packing the gain from
  `FUN_08020190(param)` into bits 4 onward while retaining the upper mode bits.
  Its caller is `FUN_0801C564`, which also updates the CTCSS/tail registers.
- `FUN_08020190` chooses between the channel's gain and the ordinary setting
  from fields in `param`; it does not read the BT-link flag (`config+0x38`), PTT
  mode (`config+0x3a`), PC13, or BK4829 `0x33`.
- `FUN_08017280` calls `FUN_0801C564` as part of the radio's RX setup, then
  writes the receive-mode register sequence including `0x30`. Its companion
  `FUN_080171D0` configures the other mode and does not write a mic-source
  selection. `FUN_0801C564` has no other direct caller in this Ghidra program.
- The BT transition path `FUN_08018AB8` calls `FUN_08017306(param, 1)`, which
  selects that same `FUN_08017280` RX sequence. The inspected `FUN_0801E1E0`
  binary-event branches do not add a BT-dependent `0x40` or `0x30` choice.

This narrows the unresolved route but does not prove that no indirect, analog,
or unrecognized binary-control path exists. No TX-mic register change is
justified by this static evidence; headset PTT's raw UART event and a controlled
over-air mic-source test are still required.

## Open

1. **Port integration.** The service, the F+MENU `DISPLAY_BT` screen and the
   settings are in, and the link reaches `Ready` on the radio. The earlier
   CALL-only sequence receives radio audio in the earpiece; automatic gain
   writes remain disabled. The port now mirrors stock PC13 and BK4829 pin-2
   control, pending radio validation. Earpiece TX mic routing remains open
   (stock silences the radio mic in BT mode). Pairing per-device pick and
   paired-list persistence also remain open.
2. **The binary `RDTP` protocol.**  Frame layout, the meaning of the
   `fe fe ee ef` prefix, and the opcode list are open; the opcodes are only known
   from the two dispatch sites.
3. **The DMA bring-up.**  The stock's TX/RX DMA channels and the `0x420`-byte RX
   buffer are transcribed above.  The port receives by **interrupt** into a
   256-byte ring (`USART3_IRQHandler`) instead of DMA, because a polled drain
   from the main loop lost bytes (the main loop spends milliseconds in panel
   blits); the stock's DMA is the equivalent hardware path.
4. **The gain encoding.**  The mic/speaker gain tables are transcribed, but the
   two-byte entries are not interpreted (`"0"`, `"5"`, ... are what is sent; how
   the chip maps them to dB is not in this image).
5. **The BT setting byte.**  The mode is mirrored to the external flash at
   `[*0x08025ff8] + 9`, but the rest of the CPS BT settings (name, pairing list)
   were not mapped.
