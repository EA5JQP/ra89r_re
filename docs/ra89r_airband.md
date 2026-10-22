# Airband reception (AM, 108.0-137.0 MHz)

**Status: implemented, receive-only, radio validation pending.**  The port
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
* `U` on the console (automation): sets the step to 25 kHz, tunes 118.000 MHz,
  gives the VFO its frequency channel and arms the range **108.000-136.975 MHz**
  (`gScanRangeStart/Stop`).  Start the scan with `E`.

8.33 kHz spacing is available in the K1's step table (`STEP_8_33kHz`) but is not
forced; 25 kHz is the default the console command uses.

## The transceiver

The stock's 134.0 MHz split (`docs/ra89r_rfpath.md`) puts 108-134 MHz on the
BK4829, and the port's per-VFO lane choice plus the scanner's candidate rule
follow it.  Above 134 MHz the stock hands the receive path to the BK4815, so
the top 3 MHz of the airband (134.0-137.0) is the one place a `FAST BOTH` sweep
would alternate chips; plain `FAST` stays on the selected VFO's chip and is the
mode to use there.

## Radio gates

* `U` then `E` with an AM carrier at 118.000 (tinySA `modulation am`): a hit,
  audio, and `AM` on the display.
* A scan crossing 137.0 returns to FM at 145.000 (no stuck AM).
* The menu's FM choice inside the band still gives AM (the band wins).
* No TX on an airband channel.
* `FAST BOTH` across 108-137 behaves as documented above.

## Open

* The AM sensitivity and the squelch ladder in AM are unmeasured (the RSSI gate
  is shared, but the AM demodulator's noise floor differs from FM's).
* No stock airband behaviour to compare against: the image has no AM UI string
  and no airband routine, so this is a port feature.
