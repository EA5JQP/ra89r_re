# RA89R documentation index

The write-ups for the Retevis RA89R / RA89G reverse engineering and for the
custom firmware.  Each hardware feature has its own file, holding the protocol
or register semantics, the evidence for every hardware claim, and — importantly
— what has already been ruled out and how, so a dead end is not re-derived.

Start with [`ra89r_findings.md`](ra89r_findings.md) if you are new here: it is the
cross-cutting write-up (hardware identification, address map, RF/band data, UI
strings, open points).  Return to it also for the **address drift** note — older
notes and quoted addresses use a shifted coordinate system.

| document | what it covers |
|---|---|
| [`ra89r_findings.md`](ra89r_findings.md) | hardware identification, flash/RAM address map, RF and band data, UI strings, open points. Addresses are for the **current** decode |
| [`ra89r_bootloader.md`](ra89r_bootloader.md) | the stock bootloader's flashing protocol, the frame format, the record validator, baud rates, and the `0x0805FFF0` update-mode request |
| [`ra89r_codeplug.md`](ra89r_codeplug.md) | the *contents* of the external flash's first 16 KB: 21-byte channel records, the two channel bitmaps, tone encoding, band ranges, the 32-byte settings block, calibration |
| [`ra89r_calibration.md`](ra89r_calibration.md) | the factory RF calibration window at `0x3000`: the stock's own address table, the pages, how the stock reads it into the RF chips, and why it must be preserved |
| [`ra89r_eeprom.md`](ra89r_eeprom.md) | the external SPI NOR flash ("EEPROM"): part and pins, SPI command set, what is actually on the chip, the write-validation test |
| [`ra89r_lcd.md`](ra89r_lcd.md) | the 128x64 panel: pin map, init sequence, addressing, fonts, port notes |
| [`ra89r_keypad.md`](ra89r_keypad.md) | the 20-button key matrix / ADC decode and the F4HWN `KEY_Code_e` mapping |
| [`ra89r_led.md`](ra89r_led.md) | the status LED (`PA13` red / `PA14` green, active high) and the backlight (`PA5`), including the pin searches that came up empty |
| [`ra89r_battery.md`](ra89r_battery.md) | the battery sense (ADC channel 9 / `PB1`), and the record of the earlier "companion gauge" dead end — that bus is the BK1080, see below |
| [`ra89r_bk1080.md`](ra89r_bk1080.md) | the BK1080 FM receiver on `PC14`/`PB2`: the I2C framing, the register map, the init block, the tune, and the RSSI/seek path |
| [`ra89r_beeper.md`](ra89r_beeper.md) | the beeper: TIM4 plus a tone generator, its pin is `PA4` (`DAC_OUT1`) |
| [`ra89r_bk4829.md`](ra89r_bk4829.md) | the RF transceiver on chip select `PB8`: identity (`0x4829`), boot sequence, differences from the UV-K1/K5V3 driver |
| [`ra89r_bk4815.md`](ra89r_bk4815.md) | the second transceiver, on `PB13`: identity (`0x4816`), its differently framed register access, boot sequence, 18-register table, the datasheet register map, the `0x71`/`0x72` frequency word, and its role as the > 134 MHz receive path |
| [`ra89r_rfpath.md`](ra89r_rfpath.md) | the RF path the two share: the bit-banged bus, boot bring-up order, what powers the RF section, the transmit chain |
| [`ra89r_rffeatures.md`](ra89r_rffeatures.md) | the stock's feature routines above the part: AF, AGC, the CTCSS/CDCSS/DTMF/scramble/VOX group, sleep/idle/mode-restore, by register |
| [`ra89r_noisereduction.md`](ra89r_noisereduction.md) | the noise reduction ("denoise"): the CPS's Noise Cancellation side-key, the compander (`0x28`) and the noise/SNR detectors (`0x63`/`0x65`) |
| [`ra89r_port.md`](ra89r_port.md) | the K1/F4HWN port itself: what the RA89R side provides, the fixes and missing modules, the board facts to re-point, and the order to do it in |
| [`firmware.md`](firmware.md) | the custom firmware project: layout, build, the console, and what is verified on the radio vs. still open |

## Conventions

* `ra89r_<feature>.md` is where a feature's own detail lives — including its
  dead ends.  `ra89r_findings.md` stays the cross-cutting write-up.
* Addresses are from the **current** decode unless the file says otherwise; see
  "Address drift" in `ra89r_findings.md`.
* Hardware claims come from the radio, not from static analysis alone.  A
  behaviour is only called *verified* once it was observed on the device; a
  register sequence read out of the disassembly is *located*, not validated.

## Related

* [`../tools/FLASHING.md`](../tools/FLASHING.md) — how to flash the radio.
* [`../tools/BUILDING.md`](../tools/BUILDING.md) — how to build the firmware and
  the host tools.
* [`../AGENTS.md`](../AGENTS.md) — repository layout, the branch workflow, and
  the contracts that are easy to get wrong.
