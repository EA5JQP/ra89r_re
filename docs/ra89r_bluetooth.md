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
first command, and console `y` prints the PD0 state and listens for the module's
boot banner.  Radio validation of the power-on is the open step; see
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
`"0"`, `"5"`, `"6"`, `"7"`, `"8"` (mic) and `"0"`, `"4"`, `"8"`, `"16"`, `"23"`,
`"31"` (speaker) at `0x08024e64`ff.  The caller passes `level + 1`, with the
level read from the config struct (`+0x3b` mic, `+0x3c` speaker).

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

## On-radio result: the module does not answer (cause found: PD0 held in reset)

The driver was built for the target and run on the radio (console `y`).  The
UART is provably correct — after `bluetooth_init()` the USART3 registers read
`CR1 = 0x200c` (UE|TE|RE), `BRR = 0x01a1` (48 MHz / 115200), `CR3 = 0`, and
`GPIOB` shows PB10/PB11 in AF2 (`MODER` bit-pairs = 2, `AFR[1]` nibbles = 2),
with **PB11 idling high**.

What the module returned was nothing usable:

* `AT+GMR?` at 115200 → one byte `0x51 'Q'`, framing error (`SR` `FE|ORE`).
* The same at 9600, 38400, 57600, 19200, 4800, 76800 and 230400.
* The stock's own order (`AT+RST`, `AT+SLEEP=OFF`, `AT+BT=EMITTER`, `AT+GMR?`),
  one step at a time with ~0.8 s of listening each → **the same `0x51` with a
  framing error, every step.**

A real UART response differs per command and decodes cleanly at *some* rate; an
identical byte with a framing error at every rate and every command is an idle/
floating line, not data.  The module's TX line is not being driven.

The cause is the **reset line, not the UART**: the stock holds the module in reset
on **PD0** and only releases it when Bluetooth is enabled (see "The BT Switch and
the module reset line (PD0)").  The bring-up does set PD0 — `FUN_08013c74`
configures it as output and `FUN_08013c24` clears it — but this firmware never
drove it, so the module stayed in reset.  The earlier conclusion ("no BT
power/enable/reset pin") was wrong: PD0 was there, attributed to the debunked
"companion gauge" reset instead.

**The fix (built, radio validation pending).**  `bluetooth_init()` now calls
`bluetooth_power(true)`, which configures PD0 as an output and reproduces the
stock's reset pulse (low 10 ms, then high) after the UART is up, so the module
boots and can emit `+IM_READY`.  Console `y` prints the PD0 state (`GPIOD MODER`/
`ODR`), power-cycles the module, listens 1 s for the boot banner, then runs the
stock's command order and prints every raw byte and framing error.  If the module
now answers, this is the missing piece; if it is still silent, PD0 was not the
only issue and the hardware-level explanations (absent/unhealthy module, a
variant that does not speak this AT set) come back into play.  The write-up keeps
the latter only as the fallback it now is.

## Open

1. **Radio validation of the PD0 power-on.**  Run console `y` on the radio and
   check that PD0 is driven high (`GPIOD ODR` bit 0), that the module emits
   `+IM_READY` after the reset pulse, and that `AT+GMR?` then returns
   `+IM_VERSION:` and `AT+CONN_STATE?` answers.  Until that run, PD0 is the
   identified cause but the power-on itself is not observed on the device.
2. **The binary `RDTP` protocol.**  Frame layout, the meaning of the
   `fe fe ee ef` prefix, and the opcode list are open; the opcodes are only known
   from the two dispatch sites.
3. **The DMA bring-up.**  The stock's TX/RX DMA channels and the `0x420`-byte RX
   buffer are transcribed above but the driver's polled transport is a stand-in.
4. **The gain encoding.**  The mic/speaker gain tables are transcribed, but the
   two-byte entries are not interpreted (`"0"`, `"5"`, ... are what is sent; how
   the chip maps them to dB is not in this image).
5. **The BT setting byte.**  The mode is mirrored to the external flash at
   `[*0x08025ff8] + 9`, but the rest of the CPS BT settings (name, pairing list)
   were not mapped.
