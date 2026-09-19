# RETEVIS RA89R — LCD / screen driver (reverse engineered)

All addresses are in the **correct** decode of
`FIRMWARE_RA89R_20260203_V49.icf` (see `ra89r.py`; image base `0x08004000`).
Addresses quoted from the old `ra89r_findings.md` are shifted — the mapping is
documented in `ra89r_findings.md` ("Address drift").

Status: **interface, init sequence, fonts and geometry are verified from the
firmware image** (every claim below is backed by disassembly of the listed
address + cross-checked against the PY32F403 register map in
`PY32F4xx_Firmware/`). Items marked *open* need hardware or a datasheet.

## 1. Summary

| | |
|---|---|
| Panel | **128 x 64 monochrome dot matrix** (8 pages x 128 columns) |
| Controller | **ST7565/ST7565R-compatible command set** (page/column addressing, latched into the panel's own RAM — the firmware keeps no framebuffer) |
| Bus | **4-wire, bit-banged**, MSB first, clock idle low, data sampled on the rising edge (mode 0-like) |
| Column origin | controller column = panel column **+ 4** (same offset the open-source UV-K1/K5V3 ST7565 driver uses) |
| Contrast | electronic volume `0x81 0x19` (25); booster `0xF8 0x01`; regulation ratio `0x25` |

## 2. Pin map (verified)

| signal | pin | evidence |
|---|---|---|
| SDA (data in) | **PB15** | `0x08015032`: `ldr r0,=0x48000400` (GPIOB) + mask `1<<15` |
| SCLK | **PA8** | `0x0801501C`: `mov.w r0,#0x48000000` (GPIOA) + mask `0x100` |
| A0 / DC (0 = command, 1 = data) | **PA10** | `0x08015086` (cmd path) clears `0x400`; `0x080150AC` (data path) sets `0x400` |
| CS (active low, held low for the 8 bits of one byte) | **PA11** | `0x08015008` clears `0x800`, `0x0801506C` sets it |
| RESET | **PA9** | `0x08014F44`: `0x200` driven `1 -> 0 -> 1` with 10 ms delays |

`FUN_08011B74(port, mask, value)` is the pin set/reset helper
(`value != 0` -> `port->BSRR = mask`, else `port->BRR = mask`; BSRR `+0x18`,
BRR `+0x28` per the PY32F403 header). Port bases: GPIOA `0x48000000`,
GPIOB `0x48000400`.

Note: SPI1 (`PB3/PB4/PB5` + `PA15` NSS, `0x08012A9C`) is **not** the display —
it is the external SPI NOR flash (see `ra89r_findings.md`).

## 3. Byte-level protocol

```
lcd_byte(b):                          ; 0x08015004
    CS = 0                            ; PA11 low
    for i in 0..7:                    ; MSB first
        SCLK = 0                      ; PA8
        SDA  = (b >> 7) & 1           ; PB15
        SCLK = 1                      ; rising edge latches the bit
        b <<= 1
    SCLK = 0
    CS = 1

lcd_cmd(b):  A0 = 0 (PA10); lcd_byte(b)     ; 0x08015080
lcd_data(b): A0 = 1 (PA10); lcd_byte(b)     ; 0x080150A6
```

## 4. Init sequence (verbatim, `0x08014F42`)

```
RESET: PA9 = 1; PA9 = 0; delay(10 ms); PA9 = 1; delay(10 ms)

cmd 0xE2            ; software reset
delay(10 ms)
cmd 0xA2            ; bias select 1/9
cmd 0xA1            ; SEG direction = reverse (0xA0 | 1)
cmd 0xC0            ; COM direction = normal
cmd 0xA6            ; inverse display = off
cmd 0xF8, cmd 0x01  ; booster ratio
cmd 0x2F            ; power circuit: VB=1 VR=1 VF=1
cmd 0x25            ; regulation ratio (V0 resistor ratio)
cmd 0x81, cmd 0x19  ; electronic volume (contrast) = 25
cmd 0xFF            ; \
cmd 0x64            ;  |
cmd 0x72            ;  |  not in the standard ST7565 command table,
cmd 0xB4            ;  |  see "open points"
cmd 0x90            ;  |
cmd 0x98            ;  |
cmd 0x70            ;  |
cmd 0xFE            ; /
cmd 0x40            ; display start line = 0
cmd 0xAF            ; display on
delay(10 ms)
```

The panel is never given a `0xAE` (display off) before the init; the driver
simply writes `0xAF` at the end. The last 8 bytes are not ST7565 commands
(a compatible-clone/extended command set is likely, e.g. NT7534-class — u8g2
notes such parts exist) — *open*: either replay them verbatim (what the RA89R
firmware does, so they are safe on this glass) or determine the real
controller from the panel marking.

## 5. Addressing and geometry

`0x0800F698` builds the 4-byte "set address" command packet from the caller's
coordinate pair:

```
page  = x  (clamped to 63)                 -> cmd[1] = 0xB0 | (x >> 3)
                                               cmd[0] = x & 7   (bit offset)
col   = y + 4 (clamped to 127)             -> cmd[2] = 0x10 | (col >> 4)
                                               cmd[3] = col & 0x0F
```

`0x0801C9A4(ctx)` sends `cmd[1], cmd[2], cmd[3]` in that order in command mode.
Then data bytes stream in with the controller auto-incrementing the column, so
the firmware never needs a framebuffer:

* the **page axis (64 px)** is the "row" coordinate, the **column axis (128 px)**
  is the "column" coordinate;
* each data byte is one **column** of 8 pixels; **bit 0 = topmost pixel**;
* a glyph is written page by page; e.g. the 8x16 font = 2 pages x 8 columns.

Geometry check: the "clear screen" call `0x08014C94` is
`fill_rect(row=0, col=0, rows=64, cols=128, pattern=0)` -> 8 pages x 128
columns.

## 6. Code map (display subsystem)

| address | role |
|---|---|
| `0x08015004` | `lcd_byte(b)` — bit-bang one byte (CS framing, MSB first) |
| `0x08015080` | `lcd_cmd(b)` — A0=0 + byte |
| `0x080150A6` | `lcd_data(b)` — A0=1 + byte |
| `0x08014F42` | `lcd_init()` — reset pulse + the sequence in section 4 |
| `0x08014C94` | `lcd_clear()` — fill whole panel with 0x00 |
| `0x08014F20` | `fill_rect(row, col, rows, cols)` wrapper |
| `0x080150BE` | `fill_box(ctx, rows, cols, pattern)` — pattern 0 -> 0x00, non-0 -> 0xFF |
| `0x0800F698` | `xy_to_page_col(x*, y*, cmd[4]*)` — see section 5 |
| `0x0801C9A4` | `lcd_set_addr(ctx)` — page/colHi/colLo triple |
| `0x08014E38` | `blit_glyph(..., data, height, ctx, width, ...)` — walks a bitmap page by page, one byte per column; keeps the current page in `ctx[1]` |
| `0x08014CBC` | glyph wrapper: picks 16-byte (8x16) or 5-byte (5x7) cell sizes |
| `0x08014D0E` | `draw_text(row, col, str, font_selector, ...)` — main text renderer |
| `0x08014CF4` | `draw_text_wrap(row, col, str)` convenience entry (no font selector) |
| `0x0800E9F0` | `font8x16_ptr(c)` -> `0x08026072 + (c-0x20)*16`, accepts `0x20..0x7A` |
| `0x0800EA18` | `font5x7_ptr(c)` -> `0x08026622 + (c-0x20)*5`, accepts `0x20..0x5A` |
| `0x0800EA40` | `cjk_glyph_ptr(s)` -> copies 32 bytes from `0x000D0000 + idx*32`, `idx = 94*(b0-0xA1) + (b1-0xA1)` (GB2312 16x16) — see open points |
| `0x08015080`/`0x080150A6` callers elsewhere | only the init and `0x0801C9A4` — i.e. everything else goes through `draw_text`/`blit_glyph`/`fill_box` |

Text renderer behaviour (`0x08014D0E`):

* font selector `0` = 8x16 (cell 8 wide, 16 tall, line pitch 16),
  `1` = 5x7 (cell 5 wide, 8 tall, line pitch 8);
* characters outside the font range are replaced by space (0x20);
* when the column would exceed `128 - cell_width` the cursor wraps to
  column 0 and the row advances (26 px for GB2312 text, 16/8 px otherwise);
* GB2312 double-byte text (`byte[0] > 0x80 and byte[1] > 0x80`, font 0 only)
  is drawn as a 16x16 glyph and advances 16 columns.

## 7. Fonts (verified by rendering)

**8x16, `0x08026072`**, 91 glyphs for ASCII `0x20..0x7A`, 16 bytes each:
`bytes[0..7]` = columns 0..7 of rows 0..7, `bytes[8..15]` = columns 0..7 of
rows 8..15. Bit 0 of each byte = topmost row of that page, bit 7 = bottom.
(Verified: 'M', '0', 'A', 'g' render correctly.)

**5x7, `0x08026622`**, 59 glyphs for ASCII `0x20..0x5A`, 5 bytes each: four
column bytes plus a trailing blank column (glyph cell 4 px wide, 7 px tall),
same bit order. (Verified: the uppercase set used by the channel display.)

**GB2312 16x16** — `0x0800EA40` computes `0x000D0000 + idx*32`. Nothing is
mapped at `0x000D0000` on this part (flash is `0x08000000..0x0805FFFF`, SRAM
`0x20000000..`), and a full GB2312 16x16 set (~282 KB) does not fit in the
146 KB application image. *Open*: see below.

## 8. What a driver for the port needs

1. **Replay the init sequence verbatim** (section 4) including the unknown
   trailing bytes; they were tuned for this glass.
2. **Addressing**: page `0xB0|(row>>3)`, column `0x10|((col+4)>>4)`,
   `(col+4)&0x0F`; keep the `+4` offset.
3. **Bus**: the panel tolerates a real SPI controller — the open-source
   UV-K1/K5V3 driver (`/home/gonzalo/Repos/uv-k1-k5v3-firmware-custom/App/driver/st7565.c`)
   drives an ST7565 panel over SPI1 with `CPOL=1`, `CPHA=2edge` (mode 3),
   MSB first, `DIV64`, plus a separate A0 GPIO, and uses the identical
   `column + 4` offset. On the RA89R the firmware bit-bangs instead
   (PB15/PA8/A0/CS/RST); either works — bit-banging is what the stock
   firmware does and needs no SPI pins.
4. **Fonts**: the stock 8x16/5x7 bitmaps are in this repo (addresses above)
   and can be lifted directly; the port's own UI fonts are a different size
   and are drawn through the port's own text code, so only the low-level
   panel driver has to be adapted.
5. **No framebuffer in the stock firmware** — the text/graphics layer streams
   to the panel. A port that wants a 1 KB shadow buffer
   (the UV-K1/K5V3 driver keeps `gFrameBuffer[7][128]` plus a
   `gStatusLine[128]`, i.e. all 8 pages) needs a `blit_full_screen` that walks 8 pages x 128 columns; the panel itself has
   no read-back path (the RA89R never reads from the display).

## 9. Open points (screen)

1. **Exact panel/controller part number.** Command set is ST7565-family, but
   `0xFF/0x64/0x72/0xB4/0x90/0x98/0x70/0xFE` are not standard ST7565 commands
   (0x64/0x72/0x70 = "display start line 36/50/48", 0xB4 = page 4 are legal;
   0x90/0x98 and 0xFE are not). Resolve by reading the LCD flex/marking or
   asking the vendor; until then replay the vendor sequence.
2. **CJK font source (`0x000D0000`)** — see section 7. Candidates:
   (a) the path is dead in this build (the RA89R UI shows only upper-case
   English strings), (b) the font is copied into that address by code that
   was not found, (c) a variant of the same firmware maps external memory
   there. Resolve by dumping the external SPI flash (Winbond-class, ID
   `0xEF16` read by the firmware itself) and looking for a 32-byte-stride
   glyph table, or by tracing `0x0800EA40` on hardware with the language set
   to Chinese.
3. **Contrast/backlight.** Contrast is fixed at `0x19` in this build (the
   UV-K1/K5V3 driver uses 31) and no menu item for it was found; whether the
   RA89R's backlight is PWM or a plain GPIO was not analysed.
4. **Display refresh triggers** — which UI events call `lcd_clear` /
   `draw_text` (i.e. whether the stock firmware redraws everything per
   update) was not traced; only the primitives were mapped.
