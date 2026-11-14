# CHIRP / webchirp programming (`ra89r_fw`)

Programming the custom firmware's channels and settings from
[webchirp](https://github.com/jasiek/webchirp) or desktop CHIRP, using the
F4HWN **v6.2.0** CHIRP driver.  Branch `driver/chirp`; plan in
`docs/superpowers/plans/2026-11-03-chirp-webchirp.md`.

State: **validated on the radio (2026-11-14)**, merged into `develop`.  The
real CHIRP driver (local CHIRP `master` d99bb227 + webchirp's
`f4hwn_v6_2_0.py` through `ra89r_f4hwn.py`) downloads and uploads over the
console's cable at 115200; see "Radio run" below.  The PTY check
(`tools/webchirp/chirp_e2e.py`) still runs the same driver offline.

## What talks to what

```
webchirp (browser, Web Serial)  ── USB-serial cable ──  Kenwood jack, USART1 115200
  ra89r_f4hwn.py                                           App/main.c  byte router
   = f4hwn_v6_2_0.py, BAUD 115200                          App/k5prog.c  K5/K1 protocol
                                                           App/k1_view.c virtual EEPROM
                                                           settings.c / py25q16.c storage
```

* **Driver** (`tools/webchirp/ra89r_f4hwn.py`): a subclass of the published
  F4HWN v6.2.0 driver, `Retevis` / `RA89R (F4HWN port)`, `BAUD_RATE = 115200`
  (the stock driver opens 38400, and this radio's console/CAT/programming port
  runs at 115200).  Nothing else changes.  The driver is GPL-3.0-or-later like
  the CHIRP code it extends.
* **Protocol** (`App/k5prog.c`): the K5/K1 frame
  `AB CD len | XOR16(payload + crc16-xmodem) | DC BA`, hello `0x0514`/`0x052F`
  → `0x0515`, read `0x051B` → `0x051C`, write `0x051D` → `0x051E`, reset
  `0x05DD`.  Read/write must carry the hello's timestamp, as in the K1's
  `app/uart.c`.  Replies carry no CRC (`XOR16(FF FF)`), as the K1 sends them.
* **Transport** (`App/main.c`): USART1 is shared by the console and FT-817
  CAT.  A K5 frame starts with `0xAB`, which no typed key and no BCD CAT
  frame can start with, so the framer sees each byte first, when no CAT window
  is open.  A stray `0xAB` followed by anything else is dropped and the next
  byte goes back to the console.  While a session is live (`K5PROG_SESSION_MS`
  = 3 s after the last frame) console **text** is muted, so asynchronous prints
  cannot land inside a reply.  `gSerialConfigCountDown_500ms` is held, which
  makes the K1 end/refuse TX, as on the K1.  When the session ends the console
  prints a `[chirp] session ended: ...` summary.

## The K1 virtual EEPROM (`App/k1_view.c`)

The driver reads `0x0000-0xB18F` (in `0x80` blocks, so the last read ends at
`0xB200`) and writes `0x0000-0xA177`.  It writes `0xB000-` as well only when
"upload calibration" is ticked.

| virtual | content | where it really is |
|---|---|---|
| `0x0000-0x3FFF` | 1024 channel records x 16 B | K1 image, physical **`0x12000`** (`K1_IMAGE_CH_BASE`).  Same bit layout as the driver's `channel[]` (`codeplug_channel_pack`: freq, offset, rx/tx code, code types, mod/offset dir, power/bw/busy/txlock, step); byte 13's DTMF PTT-ID/decode bits are dropped |
| `0x4000-0x7FFF` | names x 16 B | K1 image, same address |
| `0x8000-0x880D` | attributes x 2 B (1024 + 7) | K1 image, same address.  Bit 7 (`exclude`, the K1's runtime scan-skip flag) is **cleared on write**: CHIRP leaves it as the slot held it, so a channel created in an erased slot arrived excluded, and upstream clears it at every boot, which this port does not (radio 2026-11-05: after an upload the memory scan sat on one channel).  Note that with the scan list on "ALL", a channel needs a scan list (`scanlists` in the channel's extra settings) to be scanned |
| `0x880E-0x886D` | 24 scan-list names x 4 B | K1 image, same address; now loaded into `gListName` at boot (it was never loaded before) |
| `0x9000-0x90DF` | 7 bands x 2 VFOs x 16 B | `cp_freq[band][vfo]` in the settings blob, the same order as upstream's `0x9000 + band*32 + vfo*16` |
| `0x90E0-0x90E7` | fox hunt | not built: `0xFF`, writes dropped |
| `0xA000-0xA177` | settings | packed from / parsed into `gEeprom` and the `gSetting_*` globals (below) |
| `0xA0C8-0xA0E7` | boot message lines | pass-through to the same physical address, which `ui/welcome.c` reads |
| `0xA148-0xA14F` | spectrum settings | pass-through to the same physical address, which `app/spectrum.c` reads/writes |
| `0xB000-0xB18F` | calibration | K1 image `0x10000`, **read-only**: writes are acknowledged and dropped |
| anything else | holes | read `0xFF`, writes dropped |

Writes to the flash-backed regions compare first and skip a block that is
already there.  A typical upload changes a handful of the ~280 blocks; each
changed block is a 4 KB sector read-modify-write.  Settings and VFO writes go
to RAM shadows.  At the `0x05DD` reset the shadows are parsed
(`k1_settings_unpack`, `codeplug_freq_restore`), the settings blob is saved,
and the MCU resets.  If an upload stops without a reset (cable pulled,
browser closed), the same commit-and-reset runs when the session times out.

**What can never be written.**  No virtual address maps onto physical
`0x0000-0x3FFF`, the stock codeplug that holds the factory calibration at
`0x3000-0x38AF`, and `storage_writable()` still refuses that range underneath.
The K1 calibration window at `0x10000` is read-only here for a second reason:
it holds the `RKT3` import marker at `0x100B8` (virtual `0xB0B8`, inside the
driver's `openGlitchThr`).  Overwriting the marker would re-run the one-time
import and erase the K1 image, channels included.

### Settings pack/unpack

`k1_settings_pack()` / `k1_settings_unpack()` produce and parse the F4HWN v6.2.0
`SETTINGS_SaveSettings()` / `SETTINGS_InitEEPROM()` byte layout, with the same
validation and defaults, under the same feature `#ifdef`s.  Deviations, each
for a reason:

* **No read of the old flash value to preserve bytes.**  The image is built
  from `0xFF`, and fields for features this port does not build get the K1's
  neutral value, never a stray `0xFF` the driver would index a list with:
  voice 0, NOAA 0, the DTMF-calling block `'*'`, `'#'`, 0, 10, codes
  `123`/`ABCD9`/`9DCBA`.  An empty DTMF up/down code is emitted as the K1's
  boot default `12345`/`54321`: the driver refuses an empty one (it crashed on
  it in the e2e run), and the port's `SettingsDefaults()` leaves them zeroed.
* **TOT**: the K1 refuses 0..4 and falls back to 11.  This port's default is
  3 (20 s), so the unpack accepts 0..179; otherwise a plain download/upload
  would silently change it.
* **Scan list MIX** (K1 `lists + 2`) is not built here; it loads as "all"
  (`lists + 1`).  The `0xA170` mix block reads as "every list, signature MIX".
* **gSquelchLevelOriginal** (a temporary squelch override) is reset to 10
  ("none") by an unpack.

### Settings blob v6

Several `gSetting_*` globals were never in the blob, so they were back to their
defaults at every boot: SetPwr/SetTot/SetEot/SetGui/SetMet/SetCtr/SetLck/
SetTmr/SetPtt, battery text, mic bar, backlight-on-TX/RX, live DTMF decoder,
audio profile, NFM and the dBm correction.  They now persist as their K1 bytes
in a 25-byte record appended to the blob's extra (`settings_extra_t.k1_globals`,
`EXTRA_VERSION` 6, `STORAGE_EXTRA_MAX` 336 → 384).  A v5 blob still loads and
keeps the defaults for those globals.  **Side effect:** changing one of these
in the radio's own menu now also survives a power cycle.

## Channel records moved to `0x12000` (RKT2 → RKT3)

Before this branch the K1 image kept channel records at physical
`0x9000 + ch*16`.  Records 256 and up therefore ran into `0xA000-0xA177`,
bytes the K1 code here still uses at those physical addresses (boot message
`0xA0C8`, spectrum `0xA148`, the FM clear at `0xA028`).  With the stock's 210
channels nothing collided, but a 1024-channel CHIRP upload would have.  The
stock dump (`stock_eeprom.bin`) has `0x4000-0x1FFFF` erased, so the records now
live at `0x12000-0x15FFF`, inside `K1_IMAGE_BASE..K1_IMAGE_END`.

`storage_import_k1()` migrates once: an image carrying `RKT2` gets the records
copied from `0x9000` to `0x12000`, except records 256-279, which overlapped the
settings bytes and cannot be told apart from them.  The marker is then
rewritten to `RKT3`.  Names, attributes and calibration stay where they were.
A fresh radio imports straight into the new layout.

**Downgrade caveat:** a build from before this branch knows only `RKT2`.  On an
`RKT3` radio it re-runs the import from the stock codeplug, which erases the K1
image's names and attributes and drops every edit made since (the stock
codeplug itself is untouched).

## Safety notes (radio 2026-11-05)

* **Console remote PTT** was the single byte `+`.  USART1 carries the console,
  CAT and these frames, so any stray 0x2B (line noise, a connection at the
  wrong baud) could key the transmitter for up to 30 s.  It is now `+TX` then
  Enter within 3 s, and is refused while a session is live and for 10 s after.
  During a session the K1's `SerialConfigInProgress()` already refuses every
  TX source.
* **TX permission is not channel data.**  On this firmware the radio-wide F
  Lock band plan decides it.  The per-channel TX lock (`txLock` in CHIRP, the
  K1's `TX_LOCK`) only means "obey the band plan": `RADIO_CheckTx()` refuses
  when `TX_freq_check() != 0 && TX_LOCK`, so it never blocks an in-plan
  frequency.  With the default plan (`DEF`: 137-174, 174-350, 400-600 MHz)
  a marine channel with `txLock = ON` **can still transmit** (156-162 MHz is
  in plan); `CE` (144-146, 430-440) or `DISABLE ALL` is what stops it.  An upload carries F Lock in `0xA150` and
  round-trips it unchanged.  To make the radio receive-only, set F Lock to
  `DISABLE ALL`.

## Checks

* `firmware/tools/test_k5prog.c`: frames built byte for byte like the
  driver's `_send_command`, replies parsed like `_receive_reply`.  Covers
  hello/read/write/short-write/reset, a stale timestamp, a bad CRC, the
  console/CAT bytes it must not claim, and a stalled frame.
* `firmware/tools/test_k1_view.c`: download shape; `pack(unpack(pack)) ==
  pack`; an edited upload (channels 300 and 1023, a name, a list name, a VFO,
  squelch, scan resume, SetPwr, battery text, boot message) survives a
  simulated reboot, and a second download is byte-identical.  It also checks
  that calibration and hole writes are dropped, that the stock codeplug and
  the K1 calibration bytes are unchanged, and the RKT2 → RKT3 move.
* `tools/webchirp/chirp_e2e.py`: the unmodified CHIRP v6.2.0 driver (CHIRP
  `0b4076d9`, webchirp's pin) through `ra89r_f4hwn.py`, over a PTY to
  `firmware/tools/k5prog_sim.c`.  Download, set a memory (NFM, CTCSS 67.0),
  delete one, rename one, set the squelch, upload, download again: the image
  matches over `0x0000-0xA177` and the simulator reports the stock codeplug
  and calibration unchanged.  Run 2026-11-03: all pass; download 0.4 s and
  upload 0.1 s on the PTY (the radio's bit-banged flash will be slower).

## Radio run (2026-11-14)

Image: `driver/rxsplt-save` @ `783a989` (this branch + the blob-v7 RxSplt
save + develop's TX-power fix), flashed over a radio that already ran the
RKT3 layout.  Driven from a script around the CHIRP driver on `/dev/ttyUSB0`:

* **Download**: 7.2-7.5 s, firmware `RA89R F4HWN 0.3`; the 4 channels and
  names matched the radio; every setting parsed (the K1-only ones -- DTMF,
  key actions, backlight -- read the K1 defaults, never set on this radio).
* **Upload**: cleared the 4 channels and wrote 68 (PMR1-16, NFM; 52 marine
  channels, FM, duplex ones as coast RX with a -4.6 MHz offset), scan lists
  1/2 named `PMR`/`MAR`: **10.1 s**, then the radio's own reset.
* **Download again**: all 68 read back field for field (frequency, name,
  mode, duplex/offset, power, scan list, TX lock); list names kept; nothing
  beyond them used.
* **TX lockout**: `+TX` 2.3 s after a download put **no carrier** on a tinySA
  (-117 dBm, the floor; the refusal text is muted with the session), and
  keyed normally 18 s after (-82 dBm on the tinySA, then `-` released).
* **Boot race**: the first download 4 s after the upload's reset failed with
  `Bad response header` -- CHIRP's hello met the boot banner (`boot...`).  A
  retry once the radio is up works; a user just retries.
* **LOW power**: PMR2 at "LOW 3 = 250 mW" measured ~45 uW: the F4HWN LOW
  ladder divides the page's Low byte (44) to TXP 13, where the PA barely
  conducts.  The LOW/MID ladder is not calibrated for this radio (open,
  `docs/ra89r_rfpath.md`).

## Bench gate


On the radio, with webchirp running locally with the RA89R entry
(`tools/webchirp/README.md`):

1. The first boot of this build migrates RKT2 → RKT3; check the stock's
   channels are all still there.
2. Download; edit a channel, a name, a VFO and a setting; upload.  The radio
   restarts and shows them.
3. Download again; the image equals what was uploaded.
4. Re-dump the calibration window (`tools/ra89r_calib.py`) and compare it
   with `calib.bin`.
5. The console still works after the session, and CAT (Look4Sat) still works.

Results: 1, 2 (channels, names, list names, deletes; a VFO and a menu
setting were not edited on the radio), 3 and the console part of 5 passed
(2026-11-14, above).  Upload time on the real flash: 10.1 s for 68 channels;
115200 needs no smaller block.  **Still open:** 4 -- this firmware has no
full flash dump any more (`E` is now the scan-start automation and `e` prints
64 bytes, so `tools/ra89r_eeprom.py dump` fails with "no dump header"); the
PTY check shows CHIRP's writes never reach `0x3000`-`0x38AF`, but the radio
has not been re-dumped.  CAT (Look4Sat) after a session is untested.
