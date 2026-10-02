# The RA89R calibration window

**Status: located, mapped at the page level, and preserved.  The byte-to-field
mapping inside each page is not settled; nothing here has been changed on the
radio.**  The window lives on the external SPI NOR flash (what the CPS calls the
EEPROM) at `0x3000`-`0x38AF`, inside the codeplug's first 16 KB.  The stock
firmware reads it at boot to align the RF; the port must never write it.

The decoder is `tools/ra89r_calib.py`; the page-level map below comes from the
stock firmware's own logical-address table and from the CPS decompilation in
`~/Repos/ra_re/cps_decompiled/_8890DTest/` (`AdjWin.cs`).

## Why it matters

This is the radio's factory RF alignment.  It is not a settings block and there
are no defaults for it.  If it is erased or overwritten, the fields read `0xFF`
and the stock radio misbehaves: wrong transmit power, receive/transmit off
frequency, wrong deviation, broken squelch and RSSI.  The `TYTDXC` signature at
`0x3FF0` and the window at `0x3000` are the two regions a foreign writer must
never assume it owns (`ra89r_codeplug.md`, "Writing").

The port's own state lives in the erased tail at `0x1FF000` (see
`ra89r_eeprom.md`), so nothing collides today.  `tools/ra89r_eeprom.py` can now
back the whole chip up and put it back (`backup` / `restore`, on branch
`driver/eeprom`), which is the recovery path if a write ever goes wrong.

## The stock's own address table

The stock firmware keeps the logical EEPROM address of every table in a table of
4-byte words in its own flash, at `0x08025FD0`-`0x08026060`.  The calibration
entries are:

| flash word | logical address | meaning |
|---|---|---|
| `0x08026040` | `0x3000` | calibration base (power) |
| `0x08026044` | `0x3000` | calibration base |
| `0x08026048` | `0x3000` | calibration base |
| `0x0802604c` | `0x3710` / `0x3860` | squelch / RSSI pages |
| `0x08026050` | `0x3000` | calibration base |
| `0x08026054` | `0x3000` / `0x34C0` | Rx tuning page |
| `0x08026058` | `0x3520` / `0x3000` | Tx modulation page |
| `0x0802605c` | `0x3870` | frequency-calibration page |
| `0x08026060` | `0x3880` | 48-byte settings/test block |

`tools/ra89r_calib.py --stock <image>` prints this table straight from the
stock image; it is the firmware's own view, so it does not depend on the CPS.

## The window, page by page

Boundaries are the page addresses above plus the CPS's per-page lengths
(`ConTableLength`).  Every byte is non-`0xFF` in the dump, i.e. the window is
programmed, not blank.

| logical | size | page | CPS fields |
|---|---|---|---|
| `0x3000` | 1216 | power | Freq + POW Low/Mid/Hig |
| `0x34C0` | 96 | Rx tuning | Freq + Adj |
| `0x3520` | 496 | Tx modulation | Freq + MOD trim/gain, MIC trim, limit |
| `0x3710` | 336 | squelch | Freq + W/N Sql1/Sql9 |
| `0x3860` | 16 | RSSI | Freq + RSSI1/RSSI9 |
| `0x3870` | 16 | frequency calibration | Freq + Fre Adj |
| `0x3880` | 32-48 | settings/test block | parsed by the stock at boot |

The field labels are the CPS's own (`AdjWin.cs` `DataGrid*Name`); they name what
each page tunes, not which byte is which field.

Observed in the dump:

* the power page opens with a long run of 3-byte groups `2d 77 b4`, then diverges
  (`0x3030` onward) — consistent with a per-frequency table of
  `{Low, Mid, High}` power bytes;
* the Rx-tuning page is a flat `0x50` (80) — a single tuning value repeated;
* the Tx-modulation page opens with 6-byte groups `82 82 78 78 78 64`.

## How the stock reads it

* **Per-index read — `FUN_08010550`.**  Takes a small struct whose `+3` is a
  1-based index and `+4` selects one of two tables, computes
  `addr = 0x3000 + table[i-1]` and `len = table[i] - table[i-1]`, then reads
  `len` bytes into the struct.  The tables are in the stock's flash at
  `0x080248E2` (table A) and `0x080248ED` (table B).  The index's meaning (band
  or frequency row) is not settled; the decoder prints the tables raw.
* **CPS export/test — `FUN_0801ADC4`.**  Reads `0x3000…0x3800` in `0x400`-byte
  chunks into RAM, processes each page, and builds a `0x55`/`0x87` serial packet
  — the path the alignment window uses to fetch the calibration from the radio.
* **Into the RF chips.**  The BK4815 boot writes registers `0x4c`, `0x55` and
  `0x62` from the RAM words at `0x2000449A`, `0x2000449C` and `0x2000449E`
  (`ra89r_bk4815.md`), and `0x2000449E` is filled from a parsed calibration byte
  at `0x0800DD2C`.  The other RAM consumers of `0x2000449A` are the BK4815
  bring-up (`FUN_08006A0C`) and several mode/tune routines.

## The CPS's page table

`AdjWin.cs` reads and writes the window in 512-byte chunks from `ConTetBegAdd =
12288` to `ConMaxStopAdd = 14464`, with a nine-entry page table:

```
ConTestTableAdd_CHG = { 12288, 12288, 13504, 13600, 12288, 12288, 14096, 14432, 14448 }
ConTableLength      = {   336,    16,     0,     0,  1216,    96,   496,     0,     1 }
```

The nine tabs are power, Rx tuning, Tx modulation, CTCSS modulation, signalling
modulation, Rx test, squelch, RSSI and frequency calibration.  Several tabs share
the `0x3000` base and index into it through the length offsets, which is why the
window is one concatenation rather than nine independent blocks.  The exact
row/column semantics of each page live in the CPS form's resources, not in its
code, so they are not recoverable from the decompilation alone.

## What is certain, and what is not

**Certain:** the window's location and that it is programmed; the stock's own
address table; the page base addresses; the `FUN_08010550` / `FUN_0801ADC4`
readers and the offset tables; the BK4815 RAM fields the calibration feeds; the
CPS page table and field labels.

**Not settled:** the byte-to-field mapping inside each page (which row is which
frequency, which column is which power step); the meaning of `FUN_08010550`'s
index; whether the CTCSS/signalling/Rx-test tables are separate pages or live
inside the `0x3000` block.  A future pass could settle these by driving the CPS's
alignment window against a radio and diffing the dump.

## Preserving it

* Never write `0x3000`-`0x38AF`, and never the whole `0x0`-`0x38FF` codeplug.
* Back up the whole 2 MB chip before any experiment:
  `tools/ra89r_eeprom.py backup eeprom.bin`, and restore with
  `tools/ra89r_eeprom.py restore eeprom.bin --yes --verify` (branch
  `driver/eeprom`; see `ra89r_eeprom.md`).
* Inspect it with `python3 tools/ra89r_calib.py work/ra89r_eeprom.bin --stock work/FIRMWARE_RA89R_20260203_V49.bin`.

## Open points

1. The byte-to-field mapping per page, and the row frequencies.
2. `FUN_08010550`'s index and the two offset tables.
3. Which calibration byte the noise-reduction enable bit lives in
   (`ra89r_noisereduction.md`).
4. The 48-byte block at `0x3880` and what the stock does with it.
