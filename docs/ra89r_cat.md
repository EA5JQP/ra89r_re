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

**CAT and the console share USART1** (`ENABLE_CAT` in `App/k1_features.h`,
default on).  A CAT frame is a 5-byte burst; the console is typed with gaps, so
the auto-detect buffers bytes and either applies a well-formed frame or hands
anything else to the console — both features stay live in one image.  There is
no mode switch — frames are recognised on the wire by their shape.

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

## Automation additions (2026-10-22)

The bench work needed three things the original command set did not have:

| frame | direction | meaning |
|---|---|---|
| `00 00 00 00 02` (`SM`) | host -> radio, radio replies `00 00 00 <rssi> 02` | FT-817 S-meter read: the lane's raw RSSI clamped to 255 (9-bit `0x67` on the BK4829, 7-bit `0x44` on the BK4815) |
| `00 00 00 00 81` / `01 00 00 00 81` (`VS`) | host -> radio, reply `00` | FT-817 VFO select: 0 = A, 1 = B; switches `RX_VFO`/`TX_VFO` and reconfigures the channel |
| `00 00 00 00 E1` (`STATUS`, port extension) | host -> radio, radio replies `<status> <rssi_lo> <rssi_hi> <sql> E1` | the receive state the FT-817 has no read for: status bit 0 = squelch open, bit 1 = the audio lane is the BK4815, bits 7:2 = the scan mode; then the raw RSSI (16-bit, little-endian) and the squelch level (0-9) |

`STATUS` is a vendor command, not an FT-817 one: inventing an FT-817 reply that
a real client would misread is worse than a clearly separate opcode.

The console (same port, single letters) gained the port-specific controls that
have no FT-817 equivalent, so a host can drive a whole test matrix:

| key | action |
|---|---|
| `o` | toggle `SetScn` NORMAL <-> FAST (FAST BOTH and LISTEN were removed, `ra89r_scan.md`) |
| `7` | cycle the squelch level 0..9 (prints the marks that result) |
| `8` | switch the selected VFO A/B (what the physical EXIT does) |
| `9` / `I` | cycle `TrVfoA` / `TrVfoB` between BK4829 and BK4815 |
| `N` | arm a +/-500 kHz range around the current RX frequency (what F+5 does) |
| `E` | start / stop the scan on the armed range |
| `J` | the state + benchmark report (existing) |
| `S` | 0x67 samples against the live marks (existing) |
