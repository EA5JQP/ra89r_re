# RA89R external EEPROM (SPI NOR flash)

**Status: reading works on the radio; writing is implemented but not validated.**
The driver is `App/driver/spi_flash.c` on branch `driver/eeprom` (console `e`
identifies the chip, `E` streams the whole chip as raw binary);
`tools/ra89r_eeprom.py` drives those commands from the host and
`tools/ra89r_eeprom_analyze.py` inspects the resulting image.

This chip is what the CPS calls the **EEPROM**: the codeplug (channels, channel
names, band ranges, radio name/model, CTCSS/DCS, DTMF and 2-tone/5-tone tables,
contacts) plus ~500 KB of something else — voice prompts or fonts.

## The chip and the bus

| what | value |
|---|---|
| part | **Puya PY25Q16HB**, SPI NOR, 16 Mbit = **2 MB** |
| pins | SPI1 **remap**: CS `PA15`, SCK `PB3`, MISO `PB4`, MOSI `PB5` |
| mode | SPI mode 0 — clock idles low, both sides sample on the rising edge |
| `0x90` | `0x8514` (manufacturer `0x85` = Puya, device `0x14`) |
| `0x9F` | `0x852015`, capacity byte `0x15` = 2 MB |

Evidence:

* a photo of the board names the part (`PY25Q16HB`);
* reading it over the console returns `0x90 -> 0x8514` and JEDEC `0x852015`;
* every stock flash routine brackets its transfer with
  `FUN_08011B74(0x48000000, 0x8000, 0/1)` — i.e. `PA15` is driven by hand as a
  plain GPIO chip select, not as the SPI peripheral's NSS;
* the datasheet's *default* SPI1 mapping is `PA4..PA7`, so `PA15/PB3/PB4/PB5` is
  the remap (the STM32F1-style arrangement).

**The stock's own id check is wrong for this board.** `FUN_08018BA0` returns
`FUN_08018C7C() == 0xEF16`, a *Winbond* 32 Mbit id, and the fitted Puya part
never matches it.  That constant is where the old write-up's "Winbond-class, id
`0xEF16`, 4 MB" came from — the firmware's expectation, not the hardware.
`ra89r_findings.md` has been corrected.

## Commands

SPI NOR standard, in the stock's own code (addresses are the stock V49 image):

| command | meaning | stock |
|---|---|---|
| `0x06` | write enable | `FUN_08018B6C` |
| `0x05` | read status register (bit 0 = busy) | `FUN_08018CD8`; `FUN_08018D24` polls it |
| `0x03` | read data, 24-bit address | this driver (the stock uses `0x0B`) |
| `0x0B` | fast read, 24-bit address + dummy byte | `FUN_08018C0C`, wrapped by `FUN_08018D10` |
| `0x02` | page program: write-enable, `0x02`, address, data, poll | `FUN_08018D80`, wrapped by `FUN_08018D38` (splits at 256-byte page boundaries) |
| `0x20` | 4 KB sector erase | `FUN_08018BB8` |
| `0x90` | manufacturer + device id | `FUN_08018C7C` |
| `0x9F` | JEDEC id (carries the capacity) | this driver |

## What is on the chip

A full read (`work/ra89r_eeprom.bin`, 2 MB) shows:

| range | contents |
|---|---|
| `0x00000`-`0x03FFF` | the codeplug: the CPS's flat settings image |
| `0x04000`-`0x1FFFF` | erased |
| `0x20000`-`0x28FFF` | the firmware's **journal** (table + up to 8 slots, below) |
| `0x40000`-`0x10FFFF` | index of 8-byte `{length, running offset}` entries at `0x40000`, then ~500 KB of blobs |
| `0x110000`-`0x1FFFFF` | **erased** — 960 KB (60 % of the chip is `0xFF`) |

### The journal at `0x20000`

`FUN_0801CE94` / `FUN_0801CEFC` implement a small log-structured store:

* **table** — a 4 KB sector at `0x20000`, 512 entries of 8 bytes:
  `{u32 address, 0xFF, u8 slot, 0xFF, u8 valid}`;
* **slots** — 4 KB each at `0x20000 + slot*0x1000`, `slot` in 1..8, i.e.
  `0x21000`..`0x28000`.  The dump has data in slots 1, 2, 6 and 7;
* `FUN_0801CE94` picks the next free slot by scanning for the highest `slot`
  below 8, and erases the table sector once the table passes index `0x1F7`;
* `FUN_0801CEFC` scans for entries with `valid == 0x11` and, for each, reads the
  slot, erases the sector at the entry's *logical* address, programs the slot
  content there, then writes `0x11` at `index*8 + 0x20007`.

**Observed**: five entries (indices 0-4) covering logical addresses `0x1000` and
`0x2000`, slots 1..5, every one with `valid == 0x00` — so the firmware would
ignore them.  Staged write, power loss, or a different meaning for byte 7: open.

### The CPS map, verified

The old CPS decompilation gave a list of offsets; the dump confirms the second
half of it (details in `ra89r_findings.md`): band ranges at `8000` =
`108-174 / 144-146` and `400-520 / 430-440` (10 Hz units, `0xFFFFFFFF` =
disabled; note the `108` receiver floor, wider than the preset list), radio name
`RETEVIS` at `8048`, model `RA89_Plus` at `8096`, and the DTMF, 2-tone (`T-01`),
5-tone (`5T-01`) and contact (`BI6KSS`) tables.

The **channel** half does not match: `7936` holds `0f 00 00 00` repeated and the
name table at `4416` is all spaces.  What the image does contain is a compact
channel table of **21-byte records** (name 6 bytes, Rx u32, Tx u32 in 10 Hz
units, Rx tone u16, Tx tone u16, 3 flag bytes), whose first four entries are:

```
CH-01  145.7500 MHz   simplex      CH-03  438.6500 MHz   simplex
CH-02  430.3750 MHz   simplex      CH-04  144.9750 MHz   simplex
```

The table appears five times, `0x405` (= 49 x 21) apart, and again around
`0x2100e`.  Which copy the firmware treats as live is open.

## Writing: what is safe, and how to validate it

Nothing may touch the codeplug (`0x0`-`0x3FFF`), the journal
(`0x20000`-`0x28FFF`) or the blob area (`0x40000`-`0x10FFFF`).  The erased tail
`0x110000`-`0x1FFFFF` is 960 KB of untouched `0xFF`, and the firmware only ever
writes the ranges above, so a sector there is the natural scratch area.

**The write path is implemented; the on-radio test is what is left.**

`firmware/App/driver/spi_flash.c` (from branch `driver/eeprom`, now also on
`port`) has the write commands: write-enable (`0x06`) before every erase or
program, page program (`0x02`) that splits at 256-byte page boundaries, sector
erase (`0x20`) and a status-register poll (`0x05`, WIP) after each.  Nothing
erases on its own -- the caller erases first -- and that driver's read path is
the one already validated on the radio.

On `port` the two empty tail sectors are now in use, so the write test moved
down one:

| address | use |
|---|---|
| `0x1FE000` | scratch for the write test |
| `0x1FF000` | the port's settings blob (magic, version, size, checksum, `gEeprom`) |

The test (console **`6`**) erases `0x1FE000`, programs 256 bytes of
`0xA5 ^ i`, reads them back and reports the first address that differs; console
**`5`** saves the settings blob to `0x1FF000` and reads it back, and **`e`**
prints the identity (`0x90` / `0x9F`) plus a hexdump of the codeplug's first 64
bytes.  What neither can prove on its own is persistence across a power cycle:
that is the remaining step, and `5` twice around a power-off does it.

That exercises write-enable, program, read-back and erase without touching a byte
the radio uses.

## Already settled / dead ends

| claim | status |
|---|---|
| "Winbond-class `0xEF`, device id `0x16`, 4 MB" | **wrong** — that is the stock's expectation; the fitted part is a Puya PY25Q16HB, 2 MB |
| "the write test is pending" | **path implemented** — driver + console test + settings save are in; the power-cycle persistence check is what remains |
| the flash accessors are `FUN_08017FE4`/`FUN_08018060` | **wrong** — those are the BK4815/BK4829 bit-bang primitives; the flash is `0x0B`/`0x02`/`0x20` on SPI1 |
| the image is one flat codeplug | **no** — the codeplug is flat at `0x0`, but the firmware also keeps the journal at `0x20000` |
| the glyph table is at `0x000D0000 + idx*32` | **not seen** — `0xD0000` is not a 16x16 glyph table in this dump (see open point 3) |

## Open points

1. Which channel table is live: the `7936` region, or the compact 21-byte
   records.
2. The journal's `valid` byte: five entries with `0x00` where the code wants
   `0x11`.
3. The blob area: the index at `0x40000` and the ~500 KB it points at (voice
   prompts? fonts?).
4. Whether the CPS writes the whole 2 MB or only the first 128 KB — this decides
   how much of the chip a restore has to put back.
5. The write test above has not been run on the radio yet.

## Using it

```sh
python3 tools/ra89r_eeprom.py --port /dev/ttyUSB0 id
python3 tools/ra89r_eeprom.py --port /dev/ttyUSB0 dump work/ra89r_eeprom.bin
python3 tools/ra89r_eeprom_analyze.py work/ra89r_eeprom.bin
python3 tools/ra89r_eeprom_analyze.py work/ra89r_eeprom.bin --blocks
python3 tools/ra89r_eeprom_analyze.py work/ra89r_eeprom.bin --region 0x20000 128
```

The reader is validated on the radio — the dump's landmarks land exactly where
the CPS map says they should, which aliased data could not do.  The write path is
not, so the branch stays unmerged until the write test has run.
