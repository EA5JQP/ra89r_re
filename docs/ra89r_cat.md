# CAT (FT-817) on the USB-C serial port

The RA89R's **USB-C** port is a CH340 USB-serial cable wired to **USART1** (the
Kenwood jack / bootloader UART).  The firmware speaks **Yaesu FT-817 CAT** there
so a satellite tracker can Doppler-tune the radio.

The **wire protocol** — frames, BCD encoding, mode bytes, auto-detect, timing
and the host tool — is documented separately in
[`ra89r_carserial.md`](ra89r_carserial.md).  This file is the feature view:
why it exists, how it is built, and what is still open.

## Why FT-817 CAT

The intended controller is **Look4Sat**.  Its only radio control is its
`Ft817Controller` (`core/data/.../Ft817CatProtocol.kt`), an `IRadioController`
that speaks FT-817 CAT over **Bluetooth SPP** and drives the tracking service
(`setFrequency` / `setMode` / `PTT` / `CTCSS`).  It has **no USB-serial
support**, and the radio's Bluetooth is not usable for this, so the plan is to
add a USB-serial transport to Look4Sat later.  Until then
`tools/look4sat_cat.py` sends exactly the bytes that controller sends, over the
USB-C port.

## Implementation

- `App/cat.c` / `App/cat.h` — the **parser**, device-header free: a 5-byte
  sliding window that only accepts a frame when the command byte is known and
  the payload is well-formed for it.  Host-tested by `tools/test_cat.c` in
  `tools/check_all.sh`.
- `App/cat_radio.c` — the **actions**: tune the selected VFO, set modulation,
  key/release PTT through `driver/tx.c`, set CTCSS through `CTCSS_Options`.
- `App/main.c` — feeds every USART1 byte to `cat_parser_feed()`; a completed
  frame is applied by `cat_apply()`.
- `driver/uart.c` — USART1 receive is **interrupt-driven into a ring buffer**.
  The part has no RX FIFO and the main loop polls one byte per K1-app pass, so a
  5-byte CAT burst used to overrun; the interrupt buffers every byte.

**CAT-only under `ENABLE_CAT`** (`App/k1_features.h`, default on): the finished
firmware has no console, so with it on the serial port is CAT and nothing else.
Build with `ENABLE_CAT` off to keep the bring-up console.  There is no mode
switch — frames are recognised on the wire by their shape (auto-detect), so a
CAT session needs no command.

**Selected VFO:** set-frequency tunes the selected TX VFO (the K1's active one)
and mirrors it to the RX VFO; `READ` returns the RX VFO's frequency.

## Host tool

```sh
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 read
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 freq 145.500
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 freq 435.100 --mode FM
python3 tools/look4sat_cat.py --port /dev/ttyUSB1 sweep 435.600 435.700 5
```

Default baud 115200.  See `ra89r_carserial.md` for the frame details.

## Validation

Validated on the radio **2026-10-11**: `read` returns the frequency and mode,
`set-freq`/`set-mode` ACK `0x00`, and a 5-point sweep 435.600 → 435.700 MHz all
ACK (see `ra89r_carserial.md` for the transcript).

## Open / radio-gated

- **CTCSS** maps the requested tone to the nearest `CTCSS_Options` entry; worth
  checking against a receiver.
- **Baud** is fixed at the port's 115200 (`driver/uart.c`); change it there and
  pass `--baud` to the script if a controller needs another rate.
- **Look4Sat** still needs a USB-serial `IRadioController` (Android USB-OTG +
  CH340) before it can use this directly; that is a separate phone-side change.
