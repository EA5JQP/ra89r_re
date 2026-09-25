# RA89R RF path and power

**Status: the bus, the boot bring-up order and the power picture are mapped from
the stock image; the driver work starts at the BK4829 (`ra89r_bk4829.md`).**

Two transceivers are fitted — a **BK4829** on chip select `PB8` and a **BK4815**
on `PB13` (`ra89r_bk4829.md`, `ra89r_bk4815.md`).  This file is the shared
physical layer, the order in which the stock brings them up, and what powers
them.

## The shared bus

| line | pin | direction |
|---|---|---|
| clock | `PA12` (GPIOA mask `0x1000`) | output, driven low between transfers |
| data | `PB12` (GPIOB mask `0x1000`) | **bidirectional** — output to send, released to read |
| select | `PB8` = BK4829, `PB13` = BK4815 | output, active low |

Both selects are driven by hand, not by a peripheral; every access brackets its
transfer with its own select low → transfer → high.

The bit layer is shared by both chips:

| address | role |
|---|---|
| `0x08017D6C` | shift a byte out over `PB12`, `PA12` as the clock, MSB first |
| `0x08017FE4` | shift 16 bits in |
| `0x0801DCF0`, `0x0801DCB8` | switch `PB12` between output and input (a pin reconfiguration, not just a level) |
| `0x08021FF4`, `0x08021F78` | the two write cores, differing only in which select they take |
| `0x080220A0`, `0x08022082` | the value-packing wrappers above them |
| `0x080180F0`, `0x08018060` | the two read paths |

The two parts frame differently, so the selects are not interchangeable: the
BK4829 is addressed with `reg | 0x80` (read) / `reg & 0x7f` (write), the BK4815
with `((reg & 0x7f) << 1) | 1` / `(reg & 0x7f) << 1`.

## The boot bring-up

`FUN_08016788` is the whole RF bring-up, and the order is deliberate:

```
FUN_080093EC()   -- selects the band/filter (FUN_08013790(3), FUN_080137D4(3, 0))
                    and drives PA1 low
FUN_08009C9C()   -- drives PC13 low
FUN_08006B78()   -- BK4829 configuration (39 writes)
FUN_08006A0C()   -- BK4815 configuration (33 writes + the 18-register table)
FUN_0800D434()
if (state[0x20000301] == 1) FUN_080139E4()
FUN_08007F90()
```

Each chip's own init re-tests it first and, if the identity check fails, calls
`FUN_08015D14(0x0b)` — which draws error string #11 from the table at
`0x08024e98`, i.e. the UI's `4815 Error` / `4829 Error` — instead of configuring
the chip.

## What powers what

A sweep of **every GPIO write in the stock image** (88 call sites resolved, with
their port, mask and level) leaves no candidate for an MCU-driven supply enable:
no pin is taken high as a rail gate.  The pins the stock actually drives are the
buses and the indicators:

| pin | role |
|---|---|
| `PA12`, `PB12`, `PB8`, `PB13` | the RF bus and its two selects |
| `PC14`, `PB2`, `PD0` | the companion gauge/charger chip (`ra89r_battery.md`) |
| `PA15`, `PB3`, `PB4`, `PB5` | the external SPI NOR flash (`ra89r_eeprom.md`) |
| `PB15`, `PA8`–`PA11` | the LCD |
| `PA0`, `PA1` | the TX/RX indicator field (see `ra89r_led.md`) and RF control |
| `PC13` | driven low around the RF and audio paths (`FUN_08009C9C`, 11 callers) |
| `PA13`, `PA14` | driven low from a few paths (the debug pins, reused) |

So the picture is:

* **`PA1` is not a lamp line** — it is driven low as part of the RF bring-up
  (`FUN_080093EC`) and toggles with the TX/RX indicator work, i.e. it is RF
  control, not a supply.  This is the same pin the LED search tested and found
  "does nothing visible" (`ra89r_led.md`).
* **`PC13`** is likewise a control line in the RF/audio paths, not a rail.
* **`PD0`** is pulsed low → high at boot (`FUN_0801D69C`) and driven low from a
  handful of other paths; it behaves as the companion chip's reset/handshake, so
  **charging and pack management are delegated to that chip**, with the MCU's
  only line to it being `PD0` plus the two-wire bus.
* The **MCU's own clock** is the other half of "power": the stock runs from the
  PLL its bootloader leaves running (`CR = 0x0040e583`, PLLON) while this firmware
  forces HSI at 8 MHz.  That is why every bit-banged bus here runs slower than
  the stock's.

This is as far as static reading goes without a decompiler pass over the
power-on/off and sleep paths; the Ghidra server had no program loaded for this
round, so the above is from the disassembly listing.

## The rest of the RF path

* **T/R**: `FUN_08016228` is the transmit/receive path and writes both chips.
* **Band/filter**: `FUN_080137D4` (with `FUN_08013790`) reads and rewrites
  register `0x33`; the RF bring-up calls it before configuring anything.
* **Squelch**: `FUN_080052B8` reads register `0x67` (RSSI) and makes the decision
  from it; the debug page shows it as `RSSI R67 %d`, with `0x65`/`0x63` read
  alongside.
* **TX power**: the UI carries `Power Select`, `Power 5W` and `Power 10W`
  (`0x08017900`ff) — the setting the RA89G V52 "10 W enable" build is named for.
  Which registers it lands in has not been traced.
* **Status LED**: the LED is an RF-chip indicator rather than an MCU pin
  (`ra89r_led.md`), so it comes with this bring-up.

## Open

1. **Which chip does what.**  Both are configured and used throughout; whether
   they are split by band, by TX/RX, or one is a second receiver is not
   established.
2. The power-on/off and sleep handling, and whether anything else gates the RF
   rails — needs a decompiler pass (Ghidra with the binary loaded).
3. The per-band/RX/TX routines, and where the TX power setting lands.
4. `FUN_0800D434` and `FUN_08007F90` (the other two calls in the bring-up) are
   not identified.
