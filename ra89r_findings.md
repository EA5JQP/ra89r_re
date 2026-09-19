# RETEVIS RA89R / RA89G — firmware reverse engineering findings

Target firmware: `FIRMWARE_RA89R_20260203_V49.icf`.
A second, newer sample of the same family is also in the repository:
`Ra89G_R_UpDataFile20260401_V52_10W_Enable.icf` (RA89G, V52, "10W enable") --
same container and baseline (`0x66`, 72 records from `0x08004000`), 146,064
bytes, 83% of bytes differing from the RA89R V49 image at equal offsets (they are
different builds, so compare by content, not by offset).  Useful as a
cross-check for any subsystem when the two models disagree.
Decoded with `ra89r.py` -> `work/FIRMWARE_RA89R_20260203_V49.bin`
(72 records, baseline `0x66`, 145,772 bytes, flash `0x08004000..0x0802796C`).
Analysis: `tools/ra89r_analyze.py` -> `work/analysis/{listing,functions,strings,
peripherals,datarefs}.txt|csv`.

**This document supersedes the earlier revision**, which was written from images
produced by an older decoder. That decoder decrypted the payload bytes correctly
(71 of 72 records are byte-identical to the current decode) but laid the image
out wrongly, so every address in the earlier revision is shifted. See
"Address drift" below before reading any address from an older note.

## Hardware

| block | identification | evidence |
|---|---|---|
| MCU | **Puya PY32F403** (Cortex-M4F), 384 KB flash `0x08000000`, 64 KB SRAM `0x20000000`, `PY32F403xD` register map | vendor SDK + datasheet in `PY32F4xx_Firmware/`, `PY32F403_Datasheet_V1.8.pdf`; `FLASH_END = 0x0805FFFF` in `py32f403xD.h` |
| Bootloader | 16 KB at `0x08000000`, dumped separately (`bootloader.bin`), SP `0x20003190`, reset `0x08000145`, contains the `.icf` record validator at `0x08000BE0` | `ra89r.py` docstring; `bootloader.bin` |
| RF transceiver | **BK4815 / BK4829** (both supported by this build) | UI/debug strings `4815 Error`, `4829 Error` at `0x080270F4`ff; the four-byte register frame below matches the BK481x 3-wire write |
| External flash | **SPI NOR, Winbond-class (`0xEF`), device id `0x16`**, on SPI1 | firmware reads it itself: `0x08018C7C` sends `0x90` + 24-bit address and compares the reply with `0xEF16` |
| Display | **128x64 mono dot matrix, ST7565-family controller, bit-banged 4-wire bus** | see `ra89r_lcd.md` |
| Bluetooth | module on a UART, AT-command driven | AT command strings at `0x08022344..0x080225D0` and `0x08022820` |
| Models | **RA89R and RA89G** share this firmware | `RETEVIS RA89R`, `RETEVIS RA89G` at `0x080223E8`, `0x08019ED8`, `0x0802689C` |

## Address drift (read this before quoting any address)

The earlier revision's images (`~/Repos/ra_re/RA89R_decoded*.bin`, and the Ghidra
programs of the same name in the `~/Repos/h8_re` project) start the application
image at `0x08000000` instead of `0x08004000` and emit **one extra byte per
record**, so an address `A` in those images corresponds to

```
A_actual = A_old + 0x4000 - record_index(A_old)      record_index ~ (A_old - 0x08004000) / 0x800
```

Verified examples (payload found by content match):

| old note | actual | symbol |
|---|---|---|
| `0x0801C298` | `0x08020260` | RF filter-switch cluster |
| `0x080158E7` | `0x080198BC` | band/range table marker |
| `0x0800F250` | `0x08013232` | RF band setup |
| `0x0800D9A2` | `0x080119A2`+ | (see "GPIO/BSP helpers" below) |

Older analysis is therefore still useful for its *conclusions*, but addresses must
be re-derived from the current decode.

## `.icf` container (verified, authoritative: `ra89r.py`)

One CR-separated ASCII-hex record per line: `[6-byte header][payload][1 check]`.
Header bytes are XORed with a per-family baseline (`0x66` RA89R, `0x88` UV8800,
`0x90` TH9000D); payload **and the check byte** are XORed with a per-record key
`key = h[0]^h[1]^h[2]^h[3]^h[4]^h[5] ^ baseline`; `length = (h[0]<<8)|h[1]`,
`addr = ((h[2]<<16)|(h[3]<<8)|h[4]) * 0x100`; the record is valid iff
`(sum(h) + sum(payload) + check) mod 256 == 0`. The check byte is *computed*, not
brute-forced (the bootloader routine at `0x08000BE0` does the same sum).

The earlier revision described a "v3/v4" per-section key heuristic
(`key = h[4] ^ (h[3] ^ 0x66)`). That heuristic reproduces the true key for 71 of
this file's 72 records, which is why the old decodes looked mostly right, but it
is not the format: it fails on the short final record and does not generalise
(`ra89r.py` is validated on 616 records from nine stock files across three
radios). There is no "v3/v4" question any more — use `ra89r.py`.

## Address map of the current decode

### Display subsystem (full detail in `ra89r_lcd.md`)

| address | role |
|---|---|
| `0x08014F42` | `lcd_init()` — reset pulse + 21-byte ST7565-family init |
| `0x08014C94` | `lcd_clear()` (fills 64 rows x 128 columns with 0x00) |
| `0x08015004` / `0x08015080` / `0x080150A6` | bit-banged byte / command / data writers |
| `0x0801C9A4` | send page+column address triple |
| `0x0800F698` | (row, column) -> page/column packet, adds the panel's `+4` column offset |
| `0x08014E38` | glyph/data blitter (page-major, one byte per column) |
| `0x08014D0E` / `0x08014CF4` | text renderer (8x16 and 5x7 fonts, GB2312 path) |
| `0x08026072` | 8x16 font, ASCII `0x20..0x7A`, 16 bytes/glyph |
| `0x08026622` | 5x7 font, ASCII `0x20..0x5A`, 5 bytes/glyph |

LCD pins: SDA `PB15`, SCLK `PA8`, A0/DC `PA10`, CS `PA11`, RESET `PA9`.

### External SPI flash (SPI1: SCK `PB3`, MISO `PB4`, MOSI `PB5`, NSS `PA15`)

| address | role |
|---|---|
| `0x20000D28` | SPI handle (SPI1) in RAM; `0x20000D84` = DMA1_CH1 handle used for it |
| `0x08012A9C` | clock + pin init for the SPI instance (dispatches on the handle's base) |
| `0x08018B6C` | write-enable (`0x06`) |
| `0x08018BB8` | 4 KB sector erase (`0x20` + 24-bit address) |
| `0x08018C7C` | read ID (`0x90` + 24-bit 0) -> `0xEF16` |
| `0x08018D38`, `0x08018E10` | page program (polled and DMA variants) |
| `0x0801D1D8` | SPI1/handle bring-up; ends by validating the flash ID |
| `0x0801CE90` | walks a 512-entry table (8 bytes/entry) and erases sectors past `0x1F8` |

### GPIO / BSP helpers (the earlier revision's names were guesses)

| actual | earlier name | role |
|---|---|---|
| `0x0801199C` (with a config struct) | "`FUN_0800d9ba`" | per-pin GPIO configuration from a descriptor (`mask`, mode, pull, speed, AF) |
| `0x08011884` | — | same, mask-only form (used for alternate functions) |
| `0x08011B74(port, mask, value)` | "`FUN_0800db9c`" | pin set/reset: `value ? port->BSRR = mask : port->BRR = mask` |
| `0x08011C48` / `0x08011C74` | — | NVIC enable / disable |
| `0x08011154`, `0x080111D8`, `0x080115FC` | — | peripheral/DMA init, deinit and clock helpers; they dispatch on the peripheral base held in the first word of a handle (`(base - DMA1_CH1)/0x14` = channel index), the same pattern the SPI-flash handle (`0x20000D28`) uses |
| `0x08024E80`-`0x08024F80` | — | mixed table of code/data pointers used by that layer |

Register offsets actually used (from the vendor header, not guessed):
GPIO `MODER 0x00`, `OTYPER 0x04`, `OSPEEDR 0x08`, `PUPDR 0x0C`, `IDR 0x10`,
`ODR 0x14`, `BSRR 0x18`, `LCKR 0x1C`, `AFRL 0x20`, `AFRH 0x24`, `BRR 0x28`.
(The earlier revision labelled `+0x0C` "MODER"; `+0x0C` is PUPDR and MODER is
`+0x00`.) SPI `CR1 0x00 SR 0x08 DR 0x0C`; DMA channel `CCR 0x00 CNDTR 0x04
CPAR 0x08 CMAR 0x0C`; USART `SR 0x00 DR 0x04 BRR 0x08 CR1 0x0C`.

### RF: filter switching and band data

`RF_FilterSwitch` cluster, **actual addresses**:

* `0x08020260` — `RF_FilterSwitch_VHF_UHF(state)`: `state->[0]` is a GPIO base;
  the routine reads its `ODR` (`+0x14`) bit 7 and bit 6, compares the state bytes
  `+0x45 == 0x21 '!'` and `+0x46 == 0x22 '"'`, and when the matching ODR bit is
  set calls `0x08020368` (from `0x08020280`) or `0x08020348` (from `0x0802029C`).
* `0x08020368` — `*(gpio + 0x0C) &= ~0xC0`, then `state[0x45] = 0x20`.
  `0x08020348` — `*(gpio + 0x0C) &= ~0x120`, `*(gpio + 0x14) &= ~1`, then
  `state[0x46] = 0x20`.
  Caveat: in the vendor register map `+0x0C` is `PUPDR` and `+0x00` is `MODER`,
  so the earlier revision's "clears MODER bits PB2/PB3/PB4" label is wrong even
  though the masks are the ones it quoted; whether those pins are really the
  filter-select lines (and which band is which) still needs a hardware check.
* **No static caller**: a full-image sweep of every 2-byte-aligned `bl`/`blx`
  and of all function-pointer tables finds no reference to `0x08020260`; the
  cluster is reached through a runtime-built or RAM-held pointer, or it is
  legacy for the other model in this build. (This confirms the earlier
  revision's item 4 — the earlier addresses were just shifted.)

Band/range table, **actual address `0x080198BC`** (10 Hz units):

| address | value | meaning |
|---|---|---|
| `0x080198BC` | `0x00A4CB80` | 108 MHz marker |
| `0x080198C0` | `0x20009BB8` | RAM pointer (runtime struct) |
| `0x080198C4` | 174 MHz | VHF max |
| `0x080198C8` | 400 MHz | UHF min |
| `0x080198CC` | 520 MHz | UHF max |
| `0x080198D0` / `0x080198D4` | 144 / 146 MHz | VHF refs |
| `0x080198D8` / `0x080198DC` | 430 / 440 MHz | UHF preset |
| `0x080198E0` | 148 MHz | UHF/VHF preset |
| `0x080198E4` | 420 MHz | preset |
| `0x080198E8` / `0x080198EC` / `0x080198F0` | 450 / 460 / 470 MHz | presets |
| `0x080198F4` | 136 MHz | VHF min |
| `0x080198F8` / `0x080198FC` | `0x20009C14` / `0x20009D98` | RAM pointers |

These match the CPS "Frequency Range" presets (see below), and no code path
compares a live frequency against them in-line — the ranges are CPS/EEPROM data,
so a band change is an EEPROM/`CPS` edit, not a flash patch.

### UI strings (useful for the port)

* `0x08022344..0x080225D0` — Bluetooth AT commands (`AT+GMR?`, `AT+BAUD=`,
  `AT+SLEEP=`, `AT+POWEROFF`, `AT+RST`, `AT+BLE_*`, `AT+BT_*`, `AT+SPKGAIN?`,
  `AT+MICGAIN?`, `AT+RING_CONN=`, ...), plus `RETEVIS RA89R` / `RETEVIS RA89G`.
* `0x08022820` — AT setup strings (`AT+WRITE_NAME=`, `AT+EAR_CONN=`,
  `AT+MICGAIN=`, `AT+SPKGAIN=`, `AT+RING_CONN=`).
* `0x08026B60..0x080271E3` — UI label pool, NUL-terminated variable-length
  entries: `CH.NAME`, `1750 TONE`, `FRE`, `CHINESE`, `ENGLISH`, `Main CH`,
  `Last CH`, bandwidths `100K/10K/50K/12.5K/2.5K/6.25K/25K/5K`, `LANG SEL`,
  `TX.SEL`, `CALL/KILL/FULL`, `SQL`, `DIS.NM`, `SCAN`, `WAKEN`, `CODE.ON`,
  `ALW.ON`, `STUN`, `CO`, `VFO`, `MENU`, `PTT`, `VOX LEV`, `MICLEV`,
  `N/W LOW`, `VOX SW`, `Spk Volume`, `Hold Time`, `Menu Time`, `BT Earphone`,
  `PTT Type`, `Led Type`, `Fre Reverse`, `Delete`, `PC Write`, `No Pairing`,
  `Pairing`, `Resetting`, `P1 Long`, `P2 Long`, `BT Switch`, `Tx Inh`, and the
  debug items `Radio/Char/BLE Ver/Carrier/Scrambler/4815 Error/4829 Error`.
* Display strings carry embedded control codes (e.g. `BT\tName`), so the UI
  layer interprets bytes, not pure text.
* `0x08011FE6..0x08012016` — incremental byte table `0x10..0x3F` followed by
  `"!\"#$%&'()*+,-./0123456789:;<=>???"` (character-index table for the
  earlier UI/code path).

## CPS (programming software) — band ranges are CPS/EEPROM data

From the earlier revision's CPS decompilation (sources `cps_decompiled/` in
`~/Repos/ra_re`, **not re-verified in this pass**):

* CPS EEPROM map: channels `7936`, channel names `4416`, band ranges `8000`,
  radio name `8048`, freq code `8080`, CTCSS/DCS `8208`, settings `8224`,
  DTMF `8256/8480`, 2-tone `8544+`, 5-tone `8800+`, contacts `10496/11520`.
* Band data: 3 bands x `{RxLo, RxHi, TxLo, TxHi}` in 10 Hz units at offset
  `8000` (`0xFFFFFFFF` = disabled). RA89R presets: `136-174 400-520`,
  `144-146 430-440`, `144-148 420-450`, `144-148 430-440`.
* The CPS rejects out-of-range frequencies ("BandOver") and clamps them to the
  nearest band edge, so 300 MHz (and the whole 174-400 MHz gap) never reaches
  the radio.
* The firmware's `0x080198BC` table mirrors the same preset list, i.e. it is the
  firmware-side default/fallback copy.

## Status of the earlier revision's claims

| # | earlier claim | now |
|---|---|---|
| 1 | `FUN_0800f250` = RF band-setup (writes `+0x45/+0x46`, MODER/ODR) | **structurally right, address shifted** -> `0x08013232`; register names corrected (`MODER +0x00`, `ODR +0x14`) |
| 2 | `'!'`/`'"'` state bytes only compared, never written | **confirmed**: the two select bodies store `0x20` into `+0x45`/`+0x46`; `0x21`/`0x22` are only ever compared (corrected addresses above) |
| 3 | band limits 136-174 / 400-520 confirmed as config data | **confirmed**, table is at `0x080198BC` |
| 4 | filter-switch cluster has no static callers | **re-confirmed** at `0x08020260` |
| 5 | `0x0800d9a2` is data, real helper `FUN_0800d9ba`/`FUN_0800db9c` | **renamed/corrected** -> `0x0801199C` / `0x08011B74` (see GPIO table) |
| 6 | `FUN_0801c6d0`/`FUN_080064f8` = DMA/streaming, `0x40020080`/`0x40020408` = DMA1_CH7/DMA2_CH1 | **consistent**: those are DMA register addresses; the DMA layer is now located at `0x08011154`/`0x080111D8`/`0x080115FC` and is used by the SPI-flash driver |
| 7 | `FUN_0800b08c` degenerate | unchanged region, not re-checked |
| 8 | "v3 vs v4 decode" story | **superseded** — the container format is exact (`ra89r.py`); only the *layout* of the old images was wrong |
| 9 | 300 MHz absent from any encoding | **confirmed** (no 300 MHz constant in the current decode) |

## Open points

1. **LCD controller part number** and the eight non-ST7565 init bytes
   (`0xFF 0x64 0x72 0xB4 0x90 0x98 0x70 0xFE`) — see `ra89r_lcd.md` §9.
2. **GB2312 font source** `0x000D0000 + idx*32` (referenced by `0x0800EA40`):
   nothing is mapped there (flash is `0x08000000..0x0805FFFF`, SRAM starts at
   `0x20000000`) and a full 16x16 GB2312 set does not fit in the image. Dump the
   external SPI flash and look for a 32-byte-stride glyph table, or trace the
   function on hardware with the language set to Chinese.
3. **SPI flash contents.** The firmware erases/programs sectors
   (`0x08018BB8`, `0x08018D38`); a dump of the chip (Winbond-class, id `0xEF16`)
   would answer (2) and probably hold voice prompts/config.
4. **RAM band/config structs** `0x20009BB8`, `0x20009C14`, `0x20009D98`
   (pointed at by the `0x080198BC` table) — contents and who fills them.
5. **Filter-switch callers**: nothing static references `0x08020260`; confirm on
   hardware whether it executes at all (breakpoint) or is dead code for RA89G.
6. **RF chip driver** is not yet mapped: the strings name BK4815/BK4829 and the
   UI has a "Carrier/Scrambler" debug page, but the 3-wire register layer was
   not identified in this pass (the `0x0800DCxx` region the earlier revision
   mentioned is EEPROM/config parsing, not RF).
7. **Keypad, audio (DAC/ADC), squelch, battery** — not analysed at all.
8. ~~Bootloader upload protocol~~ — **done**: see `ra89r_bootloader.md` (frames,
   commands, baud table, record handling) with `tools/ra89r_flash.py` and
   `ra89r.py mkicf` as the working host-side implementation.  Remaining unknowns
   are listed there (update-mode key combination, `E1`/`E3` semantics).

## Reproduce

```sh
python3 ra89r.py verify FIRMWARE_RA89R_20260203_V49.icf     # baseline 0x66, all 72 records valid
python3 ra89r.py mkicf my_firmware.bin my_firmware.icf # wrap an image for the bootloader
python3 ra89r.py decode FIRMWARE_RA89R_20260203_V49.icf work/FIRMWARE_RA89R_20260203_V49.bin
python3 tools/ra89r_analyze.py work/FIRMWARE_RA89R_20260203_V49.bin
```
