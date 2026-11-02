# Airband reception (AM, 108.0-137.0 MHz)

**Status: VALIDATED on the radio (branch `airband`, 2026-10-22), receive-only.**
An FM-modulated... no: an **AM** carrier at 118.000 MHz gives a clean tone at
boot with no generator toggle, the demod registers read `0x31=0001`,
`0x42=6F5C`, `0x2A=7434`, and a range scan over 108-137 stops on the carrier
(119.900 MHz, the 1 kHz-step shoulder of 120.000) with the tone audible.  The port
demodulates the civil airband in **AM** and a range scan over it hits AM
channels; the band is the K1's own `BAND2_108MHz`, so nothing about the
tuning range changes.

## Why it needs a rule at all

The K1 app has `MODULATION_AM` and the menu can set it per VFO
(`app/menu.c`), and `RADIO_SetModulation(AM)` is a complete stock-compatible
sequence (below).  What it does *not* do is follow the frequency: a **range
scan carries the VFO's modulation**, so scanning 108-137 MHz demodulates FM
unless the menu had been set to AM first -- which then also applies to
145 MHz.  The band is AM by nature, so the port chooses the receive
demodulation by band:

* `FREQUENCY_IsAirband(f)` (`frequencies.h`, host-tested): 10800000 <= f <
  13700000 in 10 Hz units.  `13700000` is the start of `BAND3_137MHz`, so the
  top edge is the last airband channel (136.975 MHz), not 137.000.
* `RADIO_ApplyBandModulation()` (`radio.c`) runs at the top of
  `RADIO_SetupRegisters()`, i.e. on every channel change, knob step and scan
  hit: inside the band the RX VFO's modulation becomes AM (and the UI shows
  `AM`); leaving it undoes *the rule's own* choice back to FM, while a menu
  choice made outside the band is left alone.

Receive-only falls out of the K1's own rule: `RADIO_PrepareTX()` refuses TX for
any non-FM modulation, so an airband channel cannot key the PA.

## The AM sequence (`RADIO_SetModulation(MODULATION_AM)`)

```
0x31 |= 0x0001        AM demodulator enable
0x42  = 0x6F5C        demodulator config
0x2A  = 0x7434
0x54/0x55             AM audio profile (AUDIO_ApplyAMProfile, or 0x9009/0x31A9)
0x43                  AM filter, RADIO_GetAMFilterBandwidth() -> BK4819_FILTER_BW_AM
                      (wide reuses the wide RF preset: docs/ra89r_bk4829.md)
afDacGain = 0xF
0x3D  = 0x2AAB
AFC disabled          (AFM/AFC only makes sense for FM)
RADIO_SetupAGC(true, false)
```

The RSSI meter (`0x67`) and the receive gate are modulation-independent, so the
per-(lane, band) noise floor and the SQL ladder work unchanged in AM.

## Using it

* Menu: the VFO's `Modulation` can still be set by hand; outside the airband it
  is what the radio uses.
* `U` on the console (automation): sets the step to **8.33 kHz** (Europe's
  airband channel grid; the K1's `STEP_8_33kHz`, 833 in 10 Hz units), tunes
  118.000 MHz, gives the VFO its frequency channel and arms the range
  **108.000-136.975 MHz** (`gScanRangeStart/Stop`; 136.975 is the last 8.33 kHz
  channel below 137.000).  Start the scan with `E`.

25 kHz spacing is still available through the menu for the legacy grid; the
console command uses 8.33 kHz because that is the real airband spacing.

## The transceiver

The stock's 134.0 MHz split (`docs/ra89r_rfpath.md`) puts 108-134 MHz on the
BK4829, and the port's per-VFO lane choice plus the scanner's candidate rule
follow it.  Above 134 MHz the stock hands the receive path to the BK4815.  The
scanner's `FAST` probes the selected VFO's own chip (`FAST BOTH`, which would
have alternated chips over 134.0-137.0, was removed 2026-10-28,
`ra89r_scan.md`).

## Radio gates

* `U` then `E` with an AM carrier at 118.000 (tinySA `modulation am`): a hit,
  audio, and `AM` on the display.
* A scan crossing 137.0 returns to FM at 145.000 (no stuck AM).
* The menu's FM choice inside the band still gives AM (the band wins).
* No TX on an airband channel.

## The gate in AM, and the noise during a scan (2026-10-22)

Two gate defects showed up only here, both because the airband carrier is strong
(0x67 = 194) and its noise floor is the VHF one (96-103):

* the (BK4829, VHF) floor was seeded *from the carrier* and clamped to 170,
  giving a mark of 200 -- above the signal -- so the gate closed, the floor then
  fell, and the state flapped (0x47 alternating 6042/6142).  The seed ceilings
  are now just above the measured noise (4829 UHF 80, VHF 120; 4815 30), so a
  carrier present at first learn cannot produce a mark above itself;
* during a scan the audio turned to noise: a 0x67 read that collides with a
  probe transaction on the shared bit-banged bus returns 0, the gate took that
  as "closed", the LISTEN pause then started a probe batch, and the probes
  collided with the poller -- self-sustaining.  Zero reads are now ignored and
  the LISTEN sweep resumes only after 5 consecutive clean closed polls.

## What the radio run exposed and fixed

* **The demod was never programmed.**  `RADIO_ConfigureChannel` sets
  `VFO_Info_t::Modulation` after the receive setup, so the VFO field said AM
  while the chip still held the boot values (`0x31=0`, `0x42=6B5A`,
  `0x2A=6600`).  `RADIO_ApplyBandModulation()` now calls
  `RADIO_SetModulation()` itself when it changes the modulation.
* **No tone at boot, tone after toggling the generator.**  With a carrier
  present at boot the floor was seeded from the first samples -- the carrier --
  so the mark sat above the signal and the gate only opened once the carrier
  dropped (floor fell) and returned.  The floor now starts from a measured
  **prior per (lane, band, modulation)**: BK4829 FM 55 UHF / 100 VHF, **AM 63
  VHF** (the AM AGC profile changes the RSSI scale), BK4815 15.
* **Noise during a scan.**  A `0x67` read colliding with a probe on the shared
  bus returns 0; the gate took that as "closed", the LISTEN pause then started a
  probe batch and the collisions became self-sustaining -- heard as noise.  Zero
  reads are ignored and the LISTEN sweep resumes only after 5 clean closed polls.

## Reception quality sweep (2026-11-02)

**Setup:** BK4829 (VFO A), SQL 4. tinySA low output, CW, through a 60 dB pad
(levels below are at the radio). Tuned over CAT; RSSI is the median of 20
`0x67` samples from console `S`. The slope is 2 counts/dB everywhere (0.5 dB per
count), so the meter is linear over -136..-66 dBm.

| MHz | mode | floor, carrier off | -126 | -116 | -106 | -96 dBm | equivalent input floor |
|---|---|---|---|---|---|---|---|
| 108.1 | AM | 78 | 85 | 102 | 122 | 142 | -128 dBm |
| 113.0 | AM | 87 | 93 | 109 | 130 | 149 | -127 dBm |
| 118.0 | AM | 104 | 106 | 118 | 137 | 157 | -122.5 dBm |
| 121.5 | AM | 114.5 | 117 | 124.5 | 143 | 163 | -120 dBm |
| 127.0 | AM | 127 | 127 | 135 | 153 | 172 | -118.5 dBm |
| 133.0 | AM | 110 | 120 | 137 | 158 | 178 | -130 dBm |
| 137.5 | FM | 125 | 128.5 | 139 | 157 | 178 | -122.5 dBm |
| 145.5 | FM | 112 | 118 | 133 | 154 | 174 | -127 dBm |

"Equivalent input floor" is `-96 dBm - (RSSI(-96) - floor) / 2`.

**Findings:**
1. **The gain rolls off below ~130 MHz.** For the same input, 0x67 is lower
   than at 133 MHz by 3 dB at 127, 7.5 at 121.5, 10.5 at 118, 14.5 at 113 and
   18 dB at 108.1. 133 MHz AM reads the same as 137.5 MHz FM, so this is the
   RF path (the VHF front end is built for 136-174 MHz; the stock has no
   airband), not the AM AGC profile.
2. **The noise floor falls with the gain**, so the RSSI-referred floor stays
   around -127..-130 dBm at 108, 113 and 133 MHz. It is **5-10 dB worse at
   118-127 MHz** (-118.5..-122.5 dBm), the busiest part of the band, and at
   137.5 MHz. The carrier-off spread is small (±2 counts), so it reads as raised
   broadband noise rather than a single spur. Not identified.
3. **The squelch opens 10-20 dB above what the meter can see.**
   - A carrier 3-7 dB above the floor is measurable at -126 (108, 113, 133) and
     -116 dBm (118-127).
   - At SQL 4 the open mark is floor + 29 counts (14.5 dB), so a fresh carrier
     opens at about -106 dBm (108.1, 133) and about -96 dBm (113-127, where
     the floor tracker had absorbed the earlier steps).
   - With the gate closed, the floor tracker climbs onto a steady
     sub-threshold carrier: during each ramp the marks rose 10-15 counts. A weak
     *continuous* signal (ATIS, VOLMET) therefore raises its own threshold and
     never opens. That isn't airband-specific; 145.5 FM does the same.

AM audio quality was not assessed: nothing here measures demodulated SNR.

## Open

* The RSSI curve is measured now ("Reception quality sweep" above). Still open:
  AM audio SNR by ear or instrument; whether another front-end setting (console
  `F` / `B`) has less loss below 130 MHz; the source of the raised floor at
  118-127 MHz; and an AM squelch that doesn't learn a steady carrier into the
  floor.  Earlier rough notes: at -10 dBm the carrier reads 0x67 = 234 at 120.000 and ~120 at
  118.000 (the AM AGC profile), and SQL 4's 30-count margin rejects a -30 dBm
  carrier that is only 9 dB above the noise.  Weak-signal AM wants SQL 1-2.
* The AM noise floor varies with frequency inside the band (36-38 at 118-120,
  74-77 at 125-127.6 with the generator off, ~90 with an antenna at 120.1), so
  the coarse prior takes a second or two to settle after a large retune.  The
  floor now climbs one count every 8 closed polls (~12 counts/s) so it does
  reach an antenna's higher floor; a per-channel floor cache is still the
  natural follow-up.
* With an antenna, 40 s of sweeping 108-136.975 at SQL 4 found nothing and
  60 s at SQL 1 was interrupted, so real off-air activity is still unverified;
  SQL 1-2 is the setting for weak AM.
* No stock airband behaviour to compare against: the image has no AM UI string
  and no airband routine, so this is a port feature.
