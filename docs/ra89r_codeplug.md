# The RA89R codeplug — what is actually on the EEPROM

**Status: the channel half is decoded, implemented and in use; the settings half
is decoded here but not yet mapped into the port.**  The decoder the port runs
is `firmware/App/port_codeplug.c`; this file is the evidence behind it and the
map for the half that is still to come.  The chip itself — the part, the bus, the
command set, the journal, the blob area — is `ra89r_eeprom.md`.

Two sources, and everything below is one of them:

* `work/ra89r_eeprom.bin` — the 2 MB read of the fitted PY25Q16HB, i.e. the
  radio as it shipped;
* the CPS decompilation in `~/Repos/ra_re/cps_decompiled/_8890DTest/` — the
  programming software for this exact model.  It carries the layout twice over:
  as named constants (`Class1.cs`) and as the reader and writer of every table
  (`ChinfDetail.cs`, `Class2.cs`, `RadioSet.cs`, `DtmfMenu.cs`, `Tone2Menu.cs`,
  `Ton5Menu1.cs`, `通信簿.cs`, `RfRangWin.cs`, `AdjWin.cs`).  File:line citations
  below are into that tree.

The CPS's own container is worth knowing because it explains the offsets: the
EEPROM is handled as a text file of **32-byte rows** (`ConOneRowDatCt = 32`),
each row `<4 hex address><"20" = byte count><64 hex digits>` with a three-row
header (`Class1.cs:105,111,113`; `TongXun.cs:1186`).  Every table is addressed as
`array[FielBegRow + ADDR / 32]`, which is why the constants are multiples of 32
where they need to be — `ChinfBegAddr = 7936`, and `7936 / 32 = 248`, exact.

The CPS's normal read/write span is `0 … 12159` with one hole it never reads
(`IicAddress == 5824` jumps to `6560`, `TongXun.cs:1233`); the calibration window
is separate, at `12288 … 14495` (`AdjWin.cs:619,645`).

## The map

| offset | size | CPS name | contents |
|---|---|---|---|
| `0x0000` | 210 × 21 = 4410 | `ChInfAddr = 0`, `ConOneChDatCt = 21` | the channel records |
| `0x113A` | 6 | — | leftovers; the name table cuts a 211th record short |
| `0x1140` | 204 × 10 = 2040 | `ChInfNameAddr = 4416`, `ConOneChNameDatCt = 10` | channel-name continuation |
| `0x16C0`–`0x199F` | 864 | — | inside the CPS's never-read hole: the factory/template preset block (`TH-8600`, `TM-72A`, `VHF_UHF`) |
| `0x1F00` | 32 | `ChinfBegAddr = 7936` | channel-**used** bitmap |
| `0x1F20` | 32 | `ChSkipBegAddr = 7968` | scan-**allow** bitmap |
| `0x1F40` | 3 × 16 = 48 | `FreBandBegAddr = 8000` | band ranges (Rx and Tx, three bands) |
| `0x1F70` | 32 | `OpenRadioNameAddr = 8048` | the intro screen's two 16-byte lines: `RETEVIS` and `RA89R` |
| `0x1F90` | 4 | `ConFreCodeBegAddr = 8080` | the programming password, 8 hex digits; `0xFFFFFFFF` = none |
| `0x1F94` | 12 | — | not mapped by the CPS |
| `0x1FA0` | 16 | `ConRadioNameAddr = 8096` | the model name: `RA89_Plus` |
| `0x1FB0` | 96 | — | not mapped by the CPS |
| `0x2010` | 16 | `ConSacBegAddr = 8208` | the scan-range corners |
| `0x2020` | 32 | `ConSetBegAddr = 8224` | **the general settings block** |
| `0x2040` | 16 × 13 = 208 | `ConDtmfChBegAddr = 8256`, `ConOneDtmfChDatCt = 13` | DTMF channel codes, nibble-packed |
| `0x2110` | 16 | — | padding |
| `0x2120` | 64 | `ConDtmfSetAddr = 8480` | DTMF settings, own ID and ANI ids |
| `0x2160` | 16 × 11 = 176 | `Tone2BegAddr = 8544`, `ConOneTone2ChDatCt = 11` | 2-tone codes (`T-01`…) |
| `0x2220` | 32 | `Tone2RxAddr = 8736` | 2-tone receive tones and settings |
| `0x2260` | 16 × 32 = 512 | `Tone5TxBegAddr = 8800`, `ConOneTxTone5ChDatCt = 32` | 5-tone transmit |
| `0x2460` | 32 | `Tone5SetAddr = 9312` | 5-tone settings and ids |
| `0x2480` | 8 × 16 = 128 | `Tone5RxBegAddr = 9344`, `ConOneRxTone5ChDatCt = 16` | 5-tone receive |
| `0x2500` | 32 × 4 = 128 | `ConRadioBegAdd = 9472` | FM-broadcast preset frequencies |
| `0x2580` | 4 | `ConRadioEnableBegAdd = 9600` | which FM presets are used |
| `0x2584` | 4 | `ConFMVfoBegAdd = 9604` | the FM VFO's frequency |
| `0x2900` | 128 × 8 = 1024 | `BookNameAddr = 10496` | contacts: names (`BI6KSS`) |
| `0x2D00` | 128 × 5 = 640 | `BookBegAddr = 11520` | contacts: the 8-digit ids |
| `0x3000` | 2208 | `AdjWin.cs:619,645` | the calibration window |
| `0x3FF0` | 6 | — | `TYTDXC`: the codeplug signature |

## The channel records

`ChInfAddr = 0` and `ConOneChDatCt = 21`: a channel is **21 bytes starting at
`index * 21` from offset zero**.  The decoder is `ChinfDetail.cs:328
DisCurrChPro`, the encoder `ChinfDetail.cs:593 ChgChStringPro` (the same logic
drives the grid in `ChInformtion.cs:440-499`).  Confirmed byte for byte against
the dump — record 0 is
`dc 36 dd 00 | dc 36 dd 00 | ff 0f | ff 0f | 40 | 00 | 04 | "CH-01 "`.

| offset | type | meaning |
|---|---|---|
| 0 | u32 LE | receive frequency, **10 Hz units** (14575000 = 145.7500 MHz) |
| 4 | u32 LE | transmit frequency, same units; equal to Rx on a simplex channel |
| 8 | u16 LE | receive tone word (see "Tones") |
| 10 | u16 LE | transmit tone word |
| 12 | u8 | flags A |
| 13 | u8 | flags B |
| 14 | u8 | flags C |
| 15 | 6 bytes | the channel name's first six bytes, space padded |

### Flags A (byte 12)

Decoder `ChinfDetail.cs:410-428`, encoder `ChinfDetail.cs:651-674`.

| bits | control | field | values |
|---|---|---|---|
| 0 | `checkBox4` | Tx inhibit | 0/1 |
| 1 | `checkBox3` | frequency reverse | 0/1 |
| 2–3 | `comboBox10` | busy lock (`BusyKind_EngCh`, `Class1.cs:524`) | 0 off, 1 sub-tone, 2 carrier |
| 4–5 | `comboBox5` | bandwidth (`WideNar_EngCh`, `Class1.cs:488`) | 0 wide, 1 mid, 2 narrow |
| 6–7 | `comboBox3` | power (`TxPower_EngCh`, `Class1.cs:464`) | 0 high, 1 mid, 2 low |

This radio's record 0 has `0x40`: power = mid.  Every other record is `0x00`
(wide, high power).

### Flags B (byte 13)

Decoder `ChinfDetail.cs:429-441`, encoder `ChinfDetail.cs:675-702`.

| bits | control | field | values |
|---|---|---|---|
| 0 | `checkBox5` | GPS / call id (`ChinfDetail.cs:1701`; shown only when GPS is enabled) | 0/1 |
| 1 | `checkBox2` | talk around | 0/1 |
| 2 | — | "the name is set": the CPS writes 1 when its 16-byte name field is not all spaces (`ChinfDetail.cs:687-690`).  Its decoder ignores the bit | |
| 3–4 | `comboBox6` | optional signalling (`OptSig_EngCh`, `Class1.cs:536`) | 0 off, 1 DTMF, 2 2-tone, 3 5-tone |
| 5–6 | `comboBox7` | squelch kind (`Spkind_EngCh`, `Class1.cs:458`).  The control is hidden (`ChinfDetail.cs:246`) and the decoder never reads it back | 0 SQ, 1 CT, 2 tone, … |
| 7 | — | never written, never read | unknown |

Bit 2 is the one place the dump and the CPS disagree: every named channel in the
dump has byte 13 `= 0x00`.  Either the factory tool wrote this codeplug rather
than this CPS, or the bit is not what the CPS's writer implies.  Nothing in the
port depends on it.

### Flags C (byte 14)

| bits | control | field | values |
|---|---|---|---|
| 0–3 | `comboBox4` | step (`ConStep`, `Class1.cs:358`) | `2.5K 5K 6.25K 10K 12.5K 25K 50K 100K`, so 4 = 12.5 kHz |
| 4–5 | `comboBox8` | DTMF PTT id (`PttKind_EngCh`, `Class1.cs:500`) | 0 off, 1 begin, 2 end, 3 both |
| 6–7 | `comboBox9` | 5-tone PTT id (same list) | |

The dialog also carries a `checkBox1` ("scramble") that is set to false on load
and never encoded, and an "offset direction" list that is not in this record at
all — the offset is implied by the two frequencies.

### The record array, and the alignment trap

The records run from offset **0** to `0x1139` — 210 slots — and the name table
starts at `0x1140`.  The CPS offers 201 channels (`ChinfDetail.cs:257`) and fills
199 rows (`ChInformtion.cs:355`); its name writer refuses `i >= 204`
(`Class1.cs:1090`); the bitmaps could address 256.  So slots 201–209 exist in the
image and are outside what the CPS will edit.

An earlier reading of this image took the name to be the *first* six bytes of the
record and the frequencies to follow.  That reading is wrong, and the way to see
it is the factory filler: record 0 would then have no name and a 15-byte orphan
of `144.9750 / 144.9750 / no tone / 0x40 0x00 0x04` would sit in front of the
array.  With the CPS's layout the array starts at zero and *every* record —
programmed or filler — parses:

```
CH-01 144.9750   CH-02 145.7500   CH-03 430.3750   CH-04 438.6500
then 144.9750 / "CH-0  " to the end of the array
```

and the `0x40` in record 0's byte 12 becomes a real field (power = mid) rather
than an alignment artefact.

## The bitmaps

Two 32-byte rows sit immediately before the band table.  Bit order in both is
`bitmap[i >> 3] & (1 << (i & 7))` — `Class2.cs:32` and `Class2.cs:732` index the
byte as `i / 8` and test `1 << (i % 8)`.

| offset | CPS name | meaning |
|---|---|---|
| `0x1F00` (7936) | `ChinfBegAddr` | bit *i* set = channel *i* exists |
| `0x1F20` (7968) | `ChSkipBegAddr` | bit *i* set = channel *i* may be **scanned** |

`ChSkipBegAddr` is named "skip" but is an **allow** map: bit = 1 selects
`ScanSkip_EngCh[0] = "Allow"`, bit = 0 selects `"Skip"` (`Class2.cs:732`,
`ChinfDetail.cs:352-362`).  Reading it as its name suggests would invert every
channel's scan behaviour.

This radio has `0F 00 00 00 …` in both: four channels programmed, all four
scannable.  The enable bitmap and the records agreeing about how many channels
the radio has is the check that makes the map usable.

The port's bridge into the K1 is `MR_LoadChannelAttributesFromFlash` in
`misc.c`: `used` decides whether the channel exists at all (the K1's `0xFFFF`
marker), the allow bit becomes the K1's `exclude` flag, the K1's band comes from
the frequency, and every channel is put in the K1's "all lists" scan list (25)
because the stock has one channel set rather than 24 lists.

One inconsistency in the CPS worth preserving as a warning for a future writer:
`Class1.cs:1131-1140` (`SavOneChgDatPro`) writes the allow bit to **0** for a
channel it saves, while `Class1.cs:1145-1159` (`SavOneChgSkipSttPro`) writes the
same bit from the dialog.  The two disagree; the dialog is the one that wins in
normal use.

## Tones

The tone words are not a single index.  `Class2.cs:158 Rx_TxChgToSubDataPro`
decodes and `Class2.cs:656 ChgSubToDataPro` encodes, and they agree.

| value (low 12 bits) | meaning |
|---|---|
| `0x0FFF`, and anything **above 2600** | no tone |
| 512 … 2600 | CTCSS, stored as **10 × Hz** (`885` = 88.5 Hz) |
| 0 … 511 | DCS, stored as the printed code read as an **octal** number (`0x13` = 19 = the code `023`) |

The thresholds are literal in the decoder: `n > 2600` → off, `n > 511` → CTCSS,
otherwise DCS (`Class2.cs:158-164`).

The high nibble carries the rest:

| word | bits | meaning |
|---|---|---|
| receive | 12–15 | the scrambler channel, 0–8 (`ScrChSet_EngCh`, `Class1.cs:542`); ≥ 9 is clamped to 0 (`ChinfDetail.cs:393-399,644-650`) |
| transmit | 12 | the signalling-monitor flag (`checkBox6`, `ChinfDetail.cs:400-404`) |
| transmit | 13 | always 0 |
| transmit | 14 | the **receive** code is DCS inverted (`...I`) — even though it lives in the transmit word |
| transmit | 15 | the transmit code is DCS inverted |

The CPS's decoder only tests bit 14 and applies it to both codes
(`ChinfDetail.cs:405-408`), which does not round-trip bit 15; the port follows
the encoder, which is the side that wrote the image
(`port_codeplug.c`, `cp_decode_tone`).

The tables the CPS offers are `Class1.cs:277 CtcTone[52]` (index 0 = `OFF`, then
62.5 … 254.1 Hz) and `Class1.cs:287 DcsTone[105]` (023 … 754), with
`Class1.cs:302 DcsToneAll[512]` as a UI alias list of every octal code.
The stored numbers are what matters: the port's `CTCSS_Options` and
`DCS_Options` hold the same ones.  The DCS lists agree index for index; the
CTCSS list differs by exactly one entry — the K1 has no 62.5 Hz — so a channel
using it decodes as "no tone" rather than as the wrong tone.

This radio uses no tones at all: every channel has `ff 0f ff 0f`.

## Channel names

The record's six bytes are the name's first half.  The 10-byte row at
`0x1140 + i * 10` is the **continuation**, name bytes 6 to 15: the CPS's encoder
builds one 31-byte string per channel, of which bytes 0–20 become the record and
bytes 21–30 become the name row (`Class1.cs:1063,1092-1117`).  Its reader
concatenates the two and overwrites the last six bytes with spaces
(`ChinfDetail.cs:483-485`), so a name is 16 bytes on the wire but only the first
ten survive a load — at most 10 ASCII characters or 5 GBK ones.  On this radio
every row is spaces, so the six bytes in the record are the whole name.
The port reads the record and appends whatever printable bytes the continuation
holds.

## The band ranges

`FreBandBegAddr = 8000`, written by `RfRangWin.cs:702-720`, read by
`Class2.cs:940-988`: **three entries of 16 bytes**, each four u32 LE in 10 Hz
units — `Rx lower, Rx upper, Tx lower, Tx upper`.  An entry whose first word is
`0xFFFFFFFF` is disabled (`RfRangWin.cs:663-672`).  This radio:

| entry | row | Rx | Tx |
|---|---|---|---|
| 0 | "150M" | 108.0000 – 174.0000 MHz | 144.0000 – 146.0000 MHz |
| 1 | "250M" | disabled | disabled |
| 2 | "450M" | 400.0000 – 520.0000 MHz | 430.0000 – 440.0000 MHz |

The receive side is wider than the transmit side on purpose: 108 MHz is the
receiver floor of the band, while transmitting is restricted to the amateur
segments.  The same 136/174/400/480 numbers appear again at 8208 as the *scan*
range corners.

## The general settings block

`ConSetBegAddr = 8224`, **32 bytes**.  Read by `RadioSet.cs:2013 DisSetDataPro`,
written by `RadioSet.cs:2722 GetSetDataStringPro`; the option lists are built in
`RadioSet.cs:1943-2011` and the labels are `RadioSet.cs:304-322`.  Byte 2 of the
block (`0x2022`) is the CRC-free "preserved" byte, and bytes 5–6, 11, 17, 24–25
and 26–31 are either reserved or forced (see below), which is why the field map
is a nibble soup rather than a struct.

| offset | bits | field (label) | values | this radio |
|---|---|---|---|---|
| 0 | 0–3 | P1 short press | `KeyDefine_EngCh` | 6 |
| 0 | 4–7 | P2 short press | same | 9 |
| 1 | 0–3 | P1 long press | same | 3 |
| 1 | 4–7 | P2 long press | same | 5 |
| 2 | 0–1 | Rx light | `RxLight_EngCh` = Always.On / Code.On / OFF | 0 |
| 2 | 2 | LED type | `TxRxLed_EngCh` = All.Enable / All.Disable | 0 |
| 2 | 4–7 | pre-carrier time | 1..n | 0 |
| 3 | 0–3 | menu exit time | `Menu_Exit_Time_EngCh` = OFF,5S…60S | 2 (10 s) |
| 4 | 0–7 | selected channel | 0…199 | 0 |
| 5–6 | — | preserved on write | unknown | `00 00` |
| 7 | 0–3 | hold time (BT) | `BuleHoldTm_EngCh` = 4S…15S, Infinite | 0 |
| 7 | 4–7 | not read | unknown | |
| 8 | 0–3 | speaker gain | 1..5 | 2 |
| 8 | 4–7 | mic gain | 1..5 | 2 |
| 9 | 0 | Bluetooth | bool | 0 |
| 9 | 1–2 | Tx mic | `BulePttTpyeHoldTm_EngCh` = BT / local / both | 0 |
| 9 | 5 | speaker switch | `BoolKind_EngCh` = OFF / On | 0 |
| 10 | 0–2 | mic gain | 1..7 | 3 |
| 11 | — | preserved on write | unknown | `00` |
| 12 | 0–4 | squelch level | `SqlLevel_EngCh` = OFF,1…9 | 5 |
| 12 | 5–7 | sub-screen | `SubMainDis_EngCh` = OFF / Frequency / Voltage | 1 |
| 13 | 0 | Tx channel select | `PttTxKind_EngCh` = Main CH / Last CH | 0 |
| 13 | 3–4 | intro screen | `RadioKind_EngCh` = OFF / voltage / char string / startup logo | 2 |
| 13 | 5–6 | repeater tone | `CallKind_EngCh` = 1750 / 2100 / 1000 / 1450 | 0 |
| 13 | 7 | beep | `BoolKind_EngCh` | 1 |
| 14 | 0–4 | TOT | `TotKind_EngCh` = OFF,30s…270s | 0 |
| 14 | 5–7 | auto power off | `AutopowOffKind_EngCh` = OFF / 30min / 1h / 2h | 0 |
| 15 | 0 | inhibit setup | bool | 0 |
| 15 | 1 | inhibit initialise | bool | 0 |
| 15 | 2–4 | tail tone elimination | `EndTail_EngCh` = OFF / Frequency / 120 / 180 / 240 | 0 |
| 15 | 5 | language | `LanguageSel_EngCh` = Chinese / English | 0 |
| 15 | 6–7 | roger | `PttKind_EngCh1` = OFF / END | 0 |
| 16 | 0–2 | backlight mode | `LedKind_EngCh` = OFF, ON, 5S…30S | 1 |
| 16 | 4–5 | display mode | `ShowName_EngCh` = Frequency / Channel / Name | 2 (name) |
| 16 | 6–7 | scan resume | `ScanKind_EngCh` = To / Co / Se | 0 |
| 17 | — | forced to `0x01` on every save (`RadioSet.cs:2860`) | unknown | (dump `00`) |
| 18 | 4 | logo | `LogKind_EngCh` = Com logo / TYT logo | 1 |
| 18 | 6 | squelch-tail elimination | bool | 1 |
| 18 | 7 | boot password | bool | 0 |
| 19 | 0–3 | backlight brightness | `LIGlLevel_EngCh` = 1…7 | 6 |
| 19 | 4 | dual watch | `DoubleWatch_EngCh` = OFF / On | 1 |
| 19 | 5 | key lock type | `KeylockKind_EngCh` = OFF / Auto | 0 |
| 19 | 6 | radio monitor | bool | 1 |
| 19 | 7 | voice | bool | 1 |
| 20 | 0–2 | VOX level | 1..6 | 4 |
| 20 | 3–6 | VOX delay | `VOXDlyTm_EngCh` = 0.5S…5.0S | 3 |
| 20 | 7 | VOX switch | bool | 0 |
| 21 | 0–1 | key mode | `KeyMode_EngCh` = ALL / PTT / KEY / KEY&Side | 3 |
| 21 | 2–3 | battery save | `SaveMode_EngCh` = OFF / 1:1 / 1:2 / 1:4 | 2 |
| 21 | 4–5 | scan-Tx mode | `TxScanMod_EngCh` = Current / LastActive / Select | 0 |
| 21 | 6 | alarm mode | `AlarmMod_EngCh` = Remote / Local | 0 |
| 22 | 0 | weather switch | bool | 0 |
| 22 | 1 | alias | bool | 0 |
| 22 | 2 | send own id | bool | 0 |
| 22 | 4–7 | weather channel | `NooACh_EngCh` = NOAA-1…12 | 0 |
| 24–25 | — | preserved on write | unknown | `06 20` |
| 26–31 | — | forced to ASCII `"000000"` on every save (`RadioSet.cs:2931`) | unknown | `30 30 30 30 30 30` |

Two more traps for anyone writing this block: byte 19's nibbles are *also* used
by the tone editors as a "selected set" scratch field — DTMF writes its high
nibble (`DtmfMenu.cs:814-822`), 2-tone its low nibble (`Tone2Menu.cs:1040-1047`)
— and `DisSetDataPro` ignores the byte entirely; and several combos built in the
designer (`comboBox22` power 10 W/5 W, `comboBox14-18`) are never encoded at all
(`RadioSet.cs:1312-1317`).  The CPS defines no defaults for this block — it is
always read from the radio.  (The C# `catch` fallback literal at
`RadioSet.cs:2422-2428` is a foreign model's template, `"TYW-UV 9000…"`, not a
default a writer may use.)

## The other tables

* **DTMF** (`0x2040`): 16 records of 13 bytes; bytes 0–11 are the digits
  **nibble-packed two per byte** (`E` = `*`, `F` = `#`), byte 12 holds the digit
  count in bits 0–4 and the ANI/signalling kind in bits 5–6
  (`DtmfMenu.cs:438-480,764-793`).  The settings at `0x2120` hold the send timer,
  separate/group codes, the acknowledge and PTT kinds, the channel-used bitmap
  (bytes 7–8), the own id and two ANI ids (`DtmfMenu.cs:350-436,713-762`).
* **2-tone** (`0x2160`): 16 records of 11 bytes — two u16 tones and a 6-byte
  label with its length; the receive tones and settings are at `0x2220`
  (`Tone2Menu.cs:811-847,924-1030`).
* **5-tone** (`0x2260`): 16 transmit records of 32 bytes (nibble-packed sequence,
  length, a hidden 16-byte id, a 6-byte name) and 8 receive records of 16 bytes,
  with the settings and ids at `0x2460` (`Ton5Menu1.cs:899-1080,1216-1379`).
* **Contacts** (`0x2900`): 128 names of 8 bytes and 128 ids of 5 — four id bytes
  plus a digit count (`Class1.cs:1170-1216`, `通信簿.cs:202-241`).  This radio has
  one contact, `BI6KSS`, id `32598715`.
* **FM broadcast presets** (`0x2500`): 32 frequencies of 4 bytes, a used bitmap
  and the FM VFO's frequency (`RadioSYS.cs:300-477`).  All 32 are 90.4 MHz with
  only slot 0 marked used, and the FM VFO is 90.4 MHz.

## The calibration window

`0x3000 … 0x389F`, read and written by the CPS's alignment window (`AdjWin.cs`)
in 512-byte chunks, with a per-page offset table (`AdjWin.cs:619`) and nine pages:
power, Rx tuning, Tx modulation, CTCSS modulation, signalling modulation, Rx
test, squelch, RSSI and frequency calibration.  The cell widths per page come
from the grid definitions (`AdjWin.cs:6333-6360,4543-4582`), and the row counts
are model-dependent.  The **semantics** of each cell — which row is which
frequency or which column is which power step — live in the form's resources,
not in code, so they are not recoverable from the decompilation alone.

The port does not read or write this, and must not: it is the radio's factory
alignment.  The RF layer runs on the values measured on this radio
(`ra89r_bk4829.md`) until the calibration is mapped.

## The signature

`0x3FF0` holds `TYTDXC`.  The stock application writes it — `0x080220C0` in the
stock image journals six bytes to logical address `0x3FF0` — and it is the one
field a foreign writer must preserve.  The port does.

## What the port maps, and what it does not

| stock | K1 | note |
|---|---|---|
| Rx/Tx frequency | `freq_config_RX/TX.Frequency` | 10 Hz units on both sides |
| the two frequencies | `TX_OFFSET_FREQUENCY` + direction | the K1 carries an offset, the stock carries both ends |
| tone words | `CodeType` + `Code` | table lookup by stored number; 62.5 Hz has no K1 entry |
| flags A bit 0 | `TX_LOCK` | |
| flags A bit 1 | `FrequencyReverse` | |
| flags A bits 2–3 | `BUSY_CHANNEL_LOCK` | non-zero → locked |
| flags A bits 4–5 | `CHANNEL_BANDWIDTH` | wide → wide, anything else → narrow |
| flags A bits 6–7 | `OUTPUT_POWER` | high/mid/low → `OUTPUT_POWER_HIGH/MID/LOW1` |
| flags B bit 1 | — | talk around has no per-channel K1 equivalent in this build |
| flags C bits 0–3 | `STEP_SETTING` | the lists are identical |
| flags B bits 3–4 | — | optional signalling is not ported |
| Rx tone bits 12–15 | — | `SCRAMBLING_TYPE` is forced to 0 with `ENABLE_FEAT_F4HWN` |
| the enable bitmap | `ChannelAttributes_t.__val == 0xFFFF` | "this radio has no such channel" |
| the allow bitmap | `ChannelAttributes_t.exclude` | inverted |
| band of the Rx frequency | `ChannelAttributes_t.band` | |

The settings block is **not** mapped yet: the port's own defaults and its blob
decide.  Reading the block into the K1's `EEPROM_Config_t` is the next piece of
this work.

## Writing

The port reads the stock's regions and writes none of them:

* channels and names: read-only, so the stock firmware and the CPS keep the
  codeplug they wrote.  `SETTINGS_SaveChannel`/`SaveChannelName` are inert and
  say why;
* channel attributes: `MR_SaveChannelAttributesToFlash` is refused, because the
  K1's address for it (`0x8000`) is the middle of this chip's channel records
  and writing there would shred the codeplug;
* the settings and calibration blocks: untouched;
* the port's own state (its menu settings, and the frequency channels the stock
  has no place for) lives in its own blob at `0x1FF000`, in the tail the dump
  shows erased from `0x10FE41`.

A write path that keeps both firmwares working is possible — the stock has a
journal for exactly this, and the CPS rewrites whole 512-byte chunks — but it
needs the journal's `valid` byte question settled (`ra89r_eeprom.md`) and the
radio in front of it.  The two regions a writer must never assume it owns are
the calibration window and the `TYTDXC` signature.

## Open points

1. Mapping the settings block (`0x2020`) into `EEPROM_Config_t`, including the
   fields the K1 has no analogue for (P1/P2 key definitions, Bluetooth, the
   weather channel).
2. The calibration window: the row/column semantics of each page.
3. Byte 13 bit 2, and why the dump's named channels have it clear.
4. `ConFreCodeBegAddr`'s neighbours at `0x1F94` and the unmapped `0x1FB0`
   block, and the 96 bytes at `0x1FB0`–`0x200F`.
5. What slots 201–209 are for, and whether the radio reads them.
