# Receive sensitivity: the port against the stock (2 m)

**Status: measured on the radio, 2026-11-08, 145.500 MHz only.  The port
(branch `driver/rx-stock-regs`) is as sensitive as the stock on both of its
receive chips; the audible difference is that the stock mutes noise and the
port plays it.**  UHF, the airband and 250-300 MHz are not measured here.

## Why this was measured

The complaint was "airband and VHF heard with a lot of noise, UHF and 250-300
only noise, the stock receives much better".  A register-level comparison with
the stock image (below) found two receive-entry steps the port did not do, and
the stock receives everything above 134 MHz on the BK4815 while the port
defaulted both VFOs to the BK4829.  All three were changed, and this page is
the measurement that followed.

## Setup

* tinySA (classic, `tinySA_v1.4-175-g1419a93`) LOW output, `sweep cw
  145500000`, `modulation nfm`, `modulation freq 1000` (its NFM deviation is
  not known; every run below used the same signal).
* 30 dB + 40 dB attenuators between the tinySA and the antenna port (levels
  below are at the radio: tinySA level - 70 dB).  The tinySA's LOW output spans
  -76..-6 dBm, so -146..-76 dBm at the radio.  With the 30 dB pad alone the
  weakest level is -106 dBm, which is above every receiver's limit here.
* Wide (25 kHz) channel, squelch 0 on the port, **monitor** on the stock.
* The speaker recorded by the laptop's microphone (`pw-record`, 16 kHz mono,
  3 s per step) and analysed with ffmpeg over 300-3400 Hz: **tone** = a 60 Hz
  band-pass at 1 kHz, **rest** = a 120 Hz band-reject at 1 kHz (noise +
  distortion), **SINAD** = tone - rest.  Peak levels were logged per step; a
  run that clipped (peak 0 dBFS) was discarded and repeated at lower mic gain.
* The port's chip was chosen per run with the console's `I` (TrVfoB) and the
  frequency set over CAT; the stock was set by hand.

The absolute levels carry about +/-2-3 dB (tinySA output accuracy at its
bottom end, the pads' real values).  The comparisons between rows do not: the
signal chain was identical.

## Results (145.500 MHz, SINAD in dB)

| at the radio | -136 | -131 | -126 | -121 | -116 | -111 | -106 | -96 | -86 dBm |
|---|---|---|---|---|---|---|---|---|---|
| **stock** (its BK4815, > 134 MHz) | muted | muted | muted | muted | **13.0** | 13.1 | 17.4 | 19.0 | 19.1 |
| port, BK4815 | -12.8 | -11.0 | -5.7 | 2.7 | **15.6** | 23.7 | 24.7 | 25.4 | 22.4 |
| port, BK4829 | -12.8 | -11.2 | -3.7 | 4.2 | **12.2** | 16.0 | 23.1 | 22.6 | 21.7 |
| port BK4829 `0x67` | 113 | 119 | 119 | 128 | 135 | 147 | 154 | 174 | 194 |

(BK4829 `0x67` floor with no carrier: 116.  "muted": the stock's speaker
was quieter than the room, -60..-66 dBFS, no tone and no hiss.)

* **12 dB SINAD: stock ~-116 dBm, port BK4815 ~-117.4 dBm, port BK4829
  ~-116 dBm** (interpolated between the -121 and -116 rows).  The port is not
  less sensitive than the stock on either chip.
* **The BK4815 quiets sooner** (24 dB by -111 dBm; the BK4829 needs -106 dBm),
  which is what Auto's "BK4815 above 134 MHz" choice relies on.
* **The stock mutes below about -118 dBm even with monitor on**, and is still
  partly attenuated at -116 dBm (its tone 7 dB under its strong-signal level).
  With no carrier it is silent (-48 dBFS, the room); the port at squelch 0
  plays the hiss (-15..-17 dBFS on the BK4815 at the same volume).
* **The stock's audio is more distorted**: its 3rd harmonic is about -18 dBc
  against about -27 dBc on the port, which caps its SINAD at ~19 dB.  That is
  the audio chain, not reception.

So on a weak or absent signal the stock sounds quiet and the port sounds
noisy, although the port demodulates at least as well: the difference is the
noise muting.

## Outside noise on 2 m: the laptop charger

Before the charger of the laptop next to the radio was unplugged, the
BK4829's `0x67` floor was 140-155 across 138-152 MHz (125 at 170 MHz), about
17 dB above the documented 112 at 145.5 MHz, and both chips needed about
-100 dBm for usable audio.  Unplugged, 145.5 MHz read 116-121.  Ruled out on
the way: the channel bandwidth (`0x43`), the tinySA (output off, analyzer
mode, parked at 300 MHz), the BK4815's frequency and powered `0x0C` words, the
backlight, and the radio's state (a power cycle changed nothing).  A broad
hump around 144.0 MHz (149 / 158 / 150 at 143.9 / 144.0 / 144.1) remained
with the charger out; it is not the MCU clock (48 MHz x 3 would be narrow, and
432.000 MHz = 48 x 9 is clean at 55-59).  Its source is not identified.

When judging reception by ear, the radio's surroundings matter as much as the
firmware: a switching supply near the radio raised the 2 m floor by ~17 dB.

## What the firmware change was (branch `driver/rx-stock-regs`)

From a diff of every constant BK4829/BK4815 write in the stock image against
the port's:

* **BK4815 PLL calibration after a tune** -- the stock's receive entry
  `FUN_08016CEC` calls `FUN_0801BF76` right after the tune: `0x0C = 0x0A03`,
  then `FUN_0801BF54`: `0x0B = 0x0800, 0x8800, 0x9800` (the datasheet's "PLL
  calibration 1->0->1").  The port never wrote `0x0B` after boot.  Now
  `bk4815_pll_calibrate()`, run by `bk4815_receive_entry()`; the `0x0C` word in
  force is restored from a write shadow.
* **BK4829 VCO calibration after a retune** -- the stock's receive entry ends
  in `FUN_08018A46(0)`: `0x37 = 0x9D1F, 0x30 = 0x0200, 0x30 = 0xBDF1`.  The
  K1's retune writes only `0x38`/`0x39`.  Now `BK4819_CalibrateVco()`
  (`0x0200`, `0xBDF1`, then the running word back), from
  `RADIO_SetupRegisters()` and `rx_set_frequency()`.
* **Auto chip choice** -- TrVfoA/B "Auto" (the default): BK4829 at or below
  134.000 MHz, BK4815 above, the stock's `FUN_08006360` rule.

There is no measurement of the old build under these conditions, so the gain
from the two calibrations alone is **not** quantified; the table above is the
new build.

## Bench pitfalls met on the way

* A single `+` keyed TX on builds before 2026-11-05: stray bytes on opening
  the console port did exactly that on an old build, into the tinySA through
  the 30 dB pad.  Flash current builds before bench work.
* `n` and `J` are not read-only: `n` re-sends the BK4815's boot table and
  retunes it to 145.75 MHz (the coordinator then believes it is still on the
  VFO's frequency), `J` ends with a scan benchmark that moves the VFO.  While a
  bench runs the console ignores input.
* The tinySA's frequency is `sweep cw <Hz>` (its `freq` did not move the
  output); its USB link hung once and needed a power cycle.
* Bluetooth connected routes the audio to the headset: the speaker is silent.

## Open

1. The stock's noise muting.  The BK4829 lane already gates on the chip's
   noise/glitch comparator; the BK4815 lane now has the stock's SNR +
   ex-band-noise verdict (`FUN_08005218`), validated at SQL 1 on the radio
   (branch `driver/rx-noise-squelch`, `ra89r_squelch.md`).  SQL 0 still plays
   the noise.
2. UHF, the airband and 250-300 MHz sensitivity, and the old build as a
   baseline for the calibration change.
3. The 144.0 MHz noise hump.
4. The BK4829 boot registers the port still takes from the K1 rather than the
   stock (`0x1C`/`0x1D`/`0x1F`/`0x25`/`0x3A`, `0x13 = 0x3FF` in receive,
   `0x37`): candidates for the BK4829's slower quieting, untested.

## UHF, and the band-code bug (2026-11-11)

**Setup:** the radio's audio output straight into a USB sound card on the
Kenwood jack (C-Media, captured at 30%; 100% clips on hiss), so no room noise:
27 dB SINAD on a strong tone, 2nd/3rd harmonic -32/-41 dBc.  tinySA **high**
output (-38..+13 dBm at the SMA) through 30 + 20 + 10 dB, so -98 dBm is the
weakest level at the radio -- about 20 dB above the 2 m 12 dB SINAD point, so
the UHF limit is **not** measured here.  446.000 MHz, NFM 1 kHz, SQL 0.

| at the radio | -98 | -93 | -88 | -83 | -78 | -68 | -58 dBm |
|---|---|---|---|---|---|---|---|
| BK4815 SINAD | 26.0 | 25.8 | 25.8 | 25.7 | 25.7 | 25.8 | 25.6 dB |
| BK4815 SNR (`0x44<13:8>`) | 25 | 26 | 26 | 26 | 26 | 26 | 26 |
| BK4829 SINAD, **band-code bug** | -11.9 | -11.9 | -11.6 | 0.0 | -0.3 | -0.2 | 19.4 dB |
| BK4829 SINAD, **fixed** | 19.1 | 13.7 | 16.4 | 20.0 | 19.6 | 18.1 | 17.7 dB |
| BK4829 `0x67`, fixed (floor 56) | 82 | 93 | 103 | 134 | 134 | 134 | 153 |

**The band code in the BK4815's `0x75` switches the shared front end for
both chips.**  The stock writes it from the channel being received on either
chip (`FUN_08016DE8` -> `FUN_08013790` on the BK4829 branch too).  The port
wrote it from the *BK4815's own VFO*: with VFO B on the BK4829 at 446 MHz and
VFO A on the BK4815 at 120.1 MHz, `0x75 = 0x0411` (the VHF code) and the
BK4829 read `0x67` 77 at -58 dBm; the right code (`0x040A`) gave 152-153.
Every BK4829 configuration register was identical in the two states, and
neither the K1's nor the stock's `0x30` calibration sequence changed it.
`rf_dual_refresh()` now takes the band code from the selected RX VFO's
frequency and compares it with the driver's shadow (`bk4815_band_code()`),
since `pa_select_band()` and the scanner write it too.  Any setup with the two
VFOs on different chips *and* different bands (airband on one, 2 m or 70 cm on
the other -- Auto's normal case) was hit.

**Still open:** the BK4829 lane's audio is about 30 dB quieter than the
BK4815 lane's at the same volume (tone -44.6 vs -14.8 dBFS on the card; ~19 dB
at 2 m through the mic), which also caps its measured SINAD at the card's
noise floor; and the UHF 12 dB SINAD points (needs ~20 dB more attenuation).

## The BK4829 lane's low audio: the AF gain never reached the driver (2026-11-11)

`bk4819.c` keeps its own copy of the two RX audio gains (`BK4819_SetRxAudioGains()`,
so the driver does not depend on the settings module), but nothing ever called
it: `0x48` was built from the driver's fresh-settings defaults, **Gain-2 = 11
(-20.5 dB)**, not the codeplug's `VOLUME_GAIN` of 58 (+3 dB; the stock's own
word is `0xB3B5`, Gain-2 59).  The four K1 call sites of
`BK4819_SetRxAudioGain()` (`radio.c`, `app/app.c`, `app/action.c`'s mute,
`ui/menu.c`'s live preview) now hand it the current values first.  Radio:
`0x48 = 0xB3AF`; at 145.500 MHz, -86 dBm, the BK4829 lane plays at -13.4 dBFS
and the BK4815 lane at -17.1 dBFS on the card (SINAD 22.7 / 26.1 dB, 2nd/3rd
harmonics -30/-41 and -29/-39 dBc) -- before, the BK4829 lane was ~30 dB
quieter.

**USB card set-up, learned the hard way:** the C-Media card's hardware **Auto
Gain Control** must be off (`amixer -c 1 sset 'Auto Gain Control' off`): on, it
rescaled a weak or absent input by tens of dB, so a half-seated Kenwood plug
still "recorded" and levels jumped between -9 and -62 dBFS.  With AGC off and
the PipeWire source at 70% (+13.4 dB hardware) a firmly seated plug gives the
numbers above.  The radio's speaker is disconnected while the Kenwood plug is
in.

## 250-350 MHz, and the band-code split moved from 280 to 230 MHz (2026-11-11)

**Setup:** tinySA LOW output (to 350 MHz) through 60 dB, so -136..-66 dBm at
the radio; USB sound card (AGC off, 70%); NFM 1 kHz; SQL 0.

**260.000 MHz is a birdie, not a dead band:** `0x67` reads 141 there with no
carrier (69-72 at +/-50 kHz, 98 at +10 kHz) -- 10 x 26 MHz, the transceivers'
reference -- and an unmodulated carrier quiets the FM receiver, so the channel
is silent even at SQL 0.  Weaker ones at 312.000 (x12, 96) and 338.000 (x13,
85).

**The band code was wrong from ~230 to 280 MHz.**  The port switched the
front end to the UHF code (`0x0A`) only from 280 MHz; the stock uses it for its
250 MHz band too.  BK4829 RSSI rise at -76 dBm with each code:

| MHz | 180 | 190 | 200 | 210 | 220 | 230 | 240 | 250 | 265 | 279 |
|---|---|---|---|---|---|---|---|---|---|---|
| VHF code (`0x11`) | 78 | 84 | 73 | 63 | 58 | 38 | 20 | 0 | 21 | 10 |
| UHF code (`0x0A`) | 5 | -1 | 1 | 12 | 16 | 37 | 67 | 77 | 79 | 70 |

The codes cross at 230 MHz, which is now the split
(`BK4815_BAND_CODE_SPLIT_DEFAULT`, one helper `bk4815_band_code_for()` used by
every writer; console `>` steps it for the bench).  TX keeps its own 280 MHz
split (`PA_BAND_SPLIT`, the transmit band pins).  230 MHz itself is ~20 dB
down on either code -- the edge of both front-end paths.

**12 dB SINAD points** (interpolated):

| MHz | BK4815 | BK4829 | note |
|---|---|---|---|
| 250 | -109 | -109.5 | was deaf (VHF code) |
| 265 | -111 | -111.5 | was -71 / -79 (VHF code) |
| 300 | -111 | -104 | |
| 340 | -115 | -101 | |
| 145.5, for reference | -117 | -116 | |

## Airband: the branch against `develop`, back to back (2026-11-12)

AM, 1 kHz (tinySA `modulation am`, depth unknown), BK4829 lane, SQL 0, USB card
(AGC off), 60 dB of pads, same set-up for both builds; `develop` is `1716bc4`
(2026-11-02, before the receive changes on `driver/rx-stock-regs` /
`driver/rx-noise-squelch`), remote PTT disabled for the bench.

| SINAD (dB) | 133.0, -96 dBm | 133.0, -86 dBm | 121.5, -96 dBm | 121.5, -86 dBm |
|---|---|---|---|---|
| `develop` | 2.1 / 2.5 | 12.1 / 12.3 | 4.4 / 4.6 | 13.8 / 13.9 |
| branch | 4.0 / 4.2 | 14.3 / 14.6 | 4.5 / 3.9 | 14.8 / 14.8 |

No regression; the branch is equal or 1-2 dB better (its audio is ~26 dB
louder, further above the card's own noise).  AM 10 dB SINAD is therefore
about -90 dBm at 121.5-133 MHz on both builds, ~24 dB behind FM at 145.5 MHz
(12 dB SINAD -114.5 dBm the same day); part of that is the generator's unknown
AM depth and, below 130 MHz, the front end's roll-off (`ra89r_airband.md`).

Bench lessons from this round: a laptop charger raised the airband floor too
(127 MHz: `0x67` 139 with it, 108 without); after every flash the radio's
volume knob has to be turned back up (the card then reads -60 dBFS hiss
instead of -13), and the sound card comes back from a USB reset with its AGC
on.

## RxSplt is saved (settings blob v7, 2026-11-12)

The hidden menu's `RxSplt` (the receive band-code split) is one byte appended
to the settings blob as v7 (`driver/rxsplt-save`, on top of `driver/chirp`'s
v6): 10 MHz units, 18..28; older blobs and out-of-range bytes keep 230 MHz.
**Validated on the radio:** set to 260 MHz and saved, power-cycled, came back
at 260 (then set back to 230).  The first attempt "failed" because the
console's `5` re-wrote the blob without refreshing the port's own fields --
fixed in the same branch, `5` now does the menu's full save.
