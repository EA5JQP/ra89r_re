# RA89R documentation index

The write-ups for the Retevis RA89R / RA89G reverse engineering and for the
custom firmware.  Each hardware feature gets its own file, holding the protocol
or register semantics, the evidence for every hardware claim, and what has
already been ruled out and how, so a dead end is not re-derived.

Start with [`ra89r_findings.md`](ra89r_findings.md): it is the cross-cutting
write-up (hardware identification, address map, RF/band data, UI strings, open
points).  Return to it also for the **address drift** note — older notes and
quoted addresses use a shifted coordinate system.

| document | what it covers |
|---|---|
| [`ra89r_findings.md`](ra89r_findings.md) | hardware identification, flash/RAM address map, the `.icf` container, RF and band data, UI strings, open points |
| [`ra89r_bootloader.md`](ra89r_bootloader.md) | the stock bootloader's flashing protocol, the frame format, the record validator, baud rates, and the `0x0805FFF0` application-valid marker |
| [`ra89r_lcd.md`](ra89r_lcd.md) | the 128x64 panel: pin map, init sequence, addressing, fonts, port notes |
| [`firmware.md`](firmware.md) | the custom firmware project (bring-up stage): layout, build, the console, and what is verified vs. still open |

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
