# CAT (FT-817) serial protocol on the USB-C port

The RA89R's **USB-C** socket is a CH340 USB-serial cable wired to **USART1** —
the same UART as the Kenwood programming jack and the stock bootloader (`lsusb`
shows a `1a86:7523` CH340; the stock bootloader answers `probe` on it).  This
document is the **wire protocol** the firmware speaks on that port: Yaesu
**FT-817 CAT**, so a satellite tracker can Doppler-tune the radio.

The intended controller is **Look4Sat**, whose only CAT is its `Ft817Controller`
(`core/data/.../Ft817CatProtocol.kt`) over Bluetooth SPP; it has no USB-serial
transport yet.  `tools/look4sat_cat.py` sends exactly the bytes that controller
sends, over this port, today.  The feature's implementation notes and the
Look4Sat background are in `docs/ra89r_cat.md`.

## Link

- **USART1**, `PB6` TX / `PB7` RX, alternate function 2.
- **115200 8N1**, no flow control (`driver/uart.c`; the rate is the console's).
- A **frame is 5 bytes sent as one burst**.  The controller waits ~200 ms after
  a command before reading the reply (Look4Sat's `COMMAND_DELAY`).
- There is **no framing byte, no length field and no checksum**: the protocol is
  purely positional.  A receiver must know when it is at the start of a frame.

## Frame format

```
 P0   P1   P2   P3    C
└──── payload ────┘  command
```

Four payload bytes, then the **command byte** `C`.  For frequency and tone
commands the payload is a packed-BCD value, most-significant digit first;
everything else uses fixed byte values.

## Commands

| command | `C` | payload (`P0 P1 P2 P3`) | reply |
|---|---|---|---|
| set frequency | `0x01` | BCD(Hz ÷ 10), 4 bytes | ACK `0x00` |
| read freq + mode | `0x03` | `00 00 00 00` | 4 BCD + mode byte |
| set mode | `0x07` | `mode 00 00 00` | ACK `0x00` |
| PTT on | `0x08` | `00 00 00 00` | ACK `0x00` |
| PTT off | `0x88` | `00 00 00 00` | ACK `0x00` |
| CTCSS mode | `0x0A` | `sub 00 00 00` (`0x2A` on / `0x8A` off) | ACK `0x00` |
| CTCSS tone | `0x0B` | BCD(tone × 10), 2 bytes, `00 00` | ACK `0x00` |

The stock FT-817 has further commands (clarifier, `0x0F` PTT toggle, …); the
radio implements only the set above and **ignores unknown commands without a
reply**.

### Replies

- A **write** command (`0x01`, `0x07`, `0x08`, `0x88`, `0x0A`, `0x0B`) is
  answered with a single **ACK byte `0x00`**.
- **`READ` (`0x03`)** is answered with **5 bytes**: the current frequency as 4
  BCD bytes, then the mode byte.

## Frequency encoding

The value is the frequency in **10 Hz units** (`Hz ÷ 10`), zero-padded to 8
decimal digits, packed two digits per byte, most significant first.

| frequency | ×10 Hz value | digits | bytes |
|---|---|---|---|
| 145.500000 MHz | 14550000 | `14550000` | `14 55 00 00` |
| 435.100000 MHz | 43510000 | `43510000` | `43 51 00 00` |
| 435.700000 MHz | 43570000 | `43570000` | `43 57 00 00` |

So a `set frequency` to 145.500 MHz is the 5-byte frame `14 55 00 00 01`.

The radio accepts **10 – 1300 MHz** (`App/cat_radio.c`); a value outside that is
ignored.  (Ham satellite bands, 145 and 435 MHz, are well inside it.)

## Mode byte

| byte | mode | | byte | mode |
|---|---|---|---|---|
| `0x00` | LSB | | `0x04` | AM |
| `0x01` | USB | | `0x08` | FM |
| `0x02` | CW | | `0x0A` | DIG |
| `0x03` | CW-R | | `0x0C` | PKT |

The RA89R has only **FM** and **AM**; every other mode value is mapped to FM.

## CTCSS

`CTCSS mode` switches the encoder on (`P0 = 0x2A`) or off (`P0 = 0x8A`).
`CTCSS tone` carries the tone in **0.1 Hz** as two BCD bytes, e.g. 88.5 Hz →
`0885` → `08 85`.  The radio snaps the tone to the nearest entry of its
`CTCSS_Options` table.

## Auto-detect (no mode switch)

The port carries CAT directly — there is no "enter CAT mode" command.  The
firmware feeds every received byte to a 5-byte sliding window and accepts it as
a frame only when the **last byte is a known command** *and* the four payload
bytes have the shape that command uses:

| command | accepted payload |
|---|---|
| `0x01` set frequency | all four bytes valid BCD (each nibble ≤ 9) |
| `0x03` / `0x08` / `0x88` | all four bytes `0x00` |
| `0x07` set mode | `P1..P3 = 0` and `P0 ≤ 0x0C` |
| `0x0A` CTCSS mode | `P1..P3 = 0` and `P0 ∈ {0x2A, 0x8A}` |
| `0x0B` CTCSS tone | `P2..P3 = 0` and `P0,P1` valid BCD |

Because the command bytes are non-printable and the payloads are tightly
constrained, keyboard/console bytes cannot masquerade as a frame.  The parser is
`App/cat.c` / `App/cat.h`, host-tested by `tools/test_cat.c` (part of
`tools/check_all.sh`).

## Why receive is interrupt-driven

The PY32 USART has a **single-byte receive register and no FIFO**.  A 5-byte
frame sent back to back at 115200 arrives over ~435 µs, which is shorter than a
main-loop pass, so polling the register dropped the middle bytes and the frame
never matched.  `driver/uart.c` therefore receives in `USART1_IRQHandler` into a
256-byte ring buffer that the main loop drains — no bytes are lost regardless of
loop pace.  (This is why the earlier "it echoed the first byte but never
answered" symptom appeared.)

## What the radio does with each command

- **set frequency** — tunes the **selected TX VFO** (the K1's active VFO) and
  mirrors the value to the RX VFO, then applies the offset, squelch/output-power
  config and chip registers, and redraws the display.
- **read** — replies with the **RX VFO** frequency and mode.
- **set mode** — sets FM or AM.
- **PTT** — keys / unkeys the transmitter through `driver/tx.c` (microphone
  source).
- **CTCSS mode / tone** — sets the TX CTCSS code type and the nearest tone.
- Anything that does not parse is dropped silently (no reply).

## Host tool

`tools/look4sat_cat.py` is a standing-in driver that produces this traffic
(needs `pyserial`, like `tools/ra89r_flash.py`):

```sh
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 read
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 freq 145.500
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 freq 435.100 --mode FM
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 ptt on
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 ctcss 88.5
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 sweep 435.600 435.700 5
```

The default baud is **115200** (the radio's USART1 rate).  To prove the link on
its own, without the script:

```sh
python3 - <<'EOF'
import serial, time
s = serial.Serial('/dev/ttyUSB1', 115200, timeout=0.5)
s.reset_input_buffer()
for b in (0, 0, 0, 0, 3):          # FT-817 READ
    s.write(bytes([b])); s.flush(); time.sleep(0.005)
time.sleep(0.2)
print("reply:", repr(s.read(16)))  # 4 BCD + mode
s.close()
EOF
```

## Look4Sat

Look4Sat's `Ft817Controller` / `Ft817CatProtocol` build precisely these frames,
but connect over **Bluetooth SPP**, not serial.  Using this port from Look4Sat
is a phone-side change — a USB-OTG serial transport behind the same
`IRadioController` — which is out of scope for the radio; until then
`tools/look4sat_cat.py` stands in.

## Validation

Validated on the radio **2026-10-11** (firmware `driver/usbc`):

```
read: 446.000000 MHz FM
set-freq  ack=0x00 ok          (145.500 MHz)
set-mode  ack=0x00 ok
set-freq  ack=0x00 ok          (435.100 MHz, FM)
sweep 435.600 → 435.700, 5 steps: all ok
```

## Open

- The baud is fixed at **115200** in `driver/uart.c` / `board_pins.h`; change it
  there (and `--baud`) if a controller needs another rate.
- CTCSS tone snapping to `CTCSS_Options` is worth confirming against a receiver.
- `ENABLE_CAT` (`App/k1_features.h`, default on) makes the serial port
  **CAT-only** — the finished firmware has no console.  Build with it off to get
  the bring-up console back; the two do not share the port.
