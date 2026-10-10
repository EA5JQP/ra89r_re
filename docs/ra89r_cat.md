# CAT (FT-817) over the serial port

The RA89R's **USB-C** port is a CH340 USB-serial adapter wired to **USART1** —
the same UART as the Kenwood programming jack and the stock bootloader
(`lsusb` shows a `1a86:7523` CH340; the bootloader answers `probe` on it).  This
feature makes the firmware speak **Yaesu FT-817 CAT** on that port, so a tracker
can Doppler-tune the radio.

## Why FT-817 CAT

The intended controller is **Look4Sat**.  Look4Sat's only CAT is its
`Ft817Controller` (`core/data/.../Ft817CatProtocol.kt`): a `IRadioController`
that speaks FT-817 CAT over **Bluetooth SPP** and drives the tracking service
(`setFrequency`/`setMode`/`PTT`/`CTCSS`).  It has **no USB-serial support**, and
the radio's Bluetooth is not usable for this, so the plan is to add a USB-serial
transport to Look4Sat later.  Until then, `tools/look4sat_cat.py` sends exactly
the bytes that controller sends, over the USB-C port.

## Protocol (from Look4Sat's `Ft817CatProtocol`)

5-byte frames: four payload bytes then a command byte.

| command | byte | payload | reply |
|---|---|---|---|
| set frequency | `0x01` | BCD(Hz/10), 4 bytes | ACK `0x00` |
| read freq+mode | `0x03` | `00 00 00 00` | 4 BCD + mode byte |
| set mode | `0x07` | mode, `00 00 00` | ACK `0x00` |
| PTT on | `0x08` | `00 00 00 00` | ACK `0x00` |
| PTT off | `0x88` | `00 00 00 00` | ACK `0x00` |
| CTCSS mode | `0x0A` | sub, `00 00 00` (`0x2A` on / `0x8A` off) | ACK `0x00` |
| CTCSS tone | `0x0B` | BCD(tone×10), 2 bytes, `00 00` | ACK `0x00` |

Mode bytes: `0x04` AM, `0x08` FM (the RA89R has no SSB/CW, so those map to FM).

## Implementation

- `App/cat.c` / `App/cat.h` — the **parser**, device-header free: a 5-byte
  sliding window that only accepts a frame when the command byte is known and
  the payload is well-formed for it.  Host-tested by `tools/test_cat.c`
  (`tools/check_all.sh`).
- `App/cat_radio.c` — the **actions**: tune the selected VFO, set modulation,
  key/release PTT through `driver/tx.c`, set CTCSS through `CTCSS_Options`.
- `App/main.c` — feeds every USART1 byte to `cat_parser_feed()`; a completed
  frame is applied by `cat_apply()` and does **not** reach the console.  Gated
  by `ENABLE_CAT` in `k1_features.h` (default on): the finished firmware has no
  console, so with it on the port is CAT-only; build with `ENABLE_CAT` off to
  keep the bring-up console.

**Auto-detect:** there is no mode switch.  CAT frames are recognised on the wire
by their shape (a known command byte and a valid payload), so console text never
becomes a frame and a CAT session needs no command.

**Selected VFO:** set-frequency tunes the selected TX VFO (the K1's active one)
and mirrors it to the RX VFO; `READ` returns the RX VFO's frequency.

## Host tool

```sh
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 freq 145.500
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 freq 435.100 --mode FM
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 read
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 ptt on
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 ctcss 88.5
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 sweep 435.600 435.700 5
```

The default baud is 9600 (the bootloader's rate, and a common FT-817 CAT rate).

## Open / radio-gated

- **Nothing here has run on the radio yet.**  The parser is host-tested; the
  actions, the auto-detect against a real CAT stream, the selected-VFO tune, PTT
  and CTCSS need the radio.
- **CTCSS** maps the requested tone to the nearest `CTCSS_Options` entry; the
  exact scale/behaviour is worth checking on the radio.
- **Baud:** the radio's USART1 baud must match the controller's.  It is fixed in
  `board_pins.h`/`driver/uart.c` (the console's 115200); the CAT tool's default
  9600 assumes it is changed to match — settle the number on the radio.
- **Look4Sat** still needs a USB-serial `IRadioController` (Android USB-OTG +
  CH340) before it can use this directly; that is a separate, phone-side change.
