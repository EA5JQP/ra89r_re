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
| `0x08017FE4` | shift 16 bits in, MSB first |
| `0x0801DCF0`, `0x0801DCB8` | switch `PB12` between output and input (a pin reconfiguration, not just a level) |
| `0x08021FF4`, `0x08021F78` | the two write cores, differing only in which select they take |
| `0x080220A0`, `0x08022082` | the value-packing wrappers above them |
| `0x080180F0`, `0x08018060` | the two read paths |

Underneath those sit four generic helpers that are worth naming, because "which
pin" is otherwise guesswork:

| address | role |
|---|---|
| `0x08011B74(port, mask, level)` | drive a pin — `level == 0` writes `BRR` (port + `0x28`), otherwise `BSRR` (port + `0x18`) |
| `0x08011B64(port, mask)` | read a pin from `IDR` (port + `0x10`) |
| `0x0801199C(port, cfg)` | reconfigure a pin from a small struct: `{mask, mode, pull, speed, af}` → `MODER`/`OTYPER`/`OSPEEDR`/`PUPDR`/`AFR` |
| `0x0802422A(n)` | busy-wait delay |

The port bases are the literals the stock loads: **`0x48000000` is GPIOA** and
the data/select port loaded everywhere else is **`0x48000400`, GPIOB**;
`0x48000800` is GPIOC and `0x48000C00` GPIOD.  The read path is therefore: after
the command byte, `FUN_0801DCF0(1)`/`FUN_0801DCB8(1)` puts `PB12` into
`MODER = input` (mode 0) for the sample window and `(0)` returns it to
`MODER = output`.  The two functions are separate copies of the same
`PB12` reconfiguration, one per chip's read path.

Bus timing, as the stock generates it: the clock **idles low**.  To write a bit,
`FUN_08017D6C` drives `PB12` to the bit, waits, raises `PA12`, waits, and drops
`PA12` again — so the data is set while the clock is low and the rising edge is
what latches it.  To read, `FUN_08017FE4` samples `PB12` while `PA12` is low and
*then* pulses `PA12` high, 16 times, MSB first, with a longer delay after the
last bit.  Every `BSRR`/`BRR` write is separated by a short delay loop, which is
what makes the bus safe to bit-bang from the firmware.

The two parts frame differently, so the selects are not interchangeable: the
BK4829 is addressed with `reg | 0x80` (read) / `reg & 0x7f` (write), the BK4815
with `((reg & 0x7f) << 1) | 1` / `(reg & 0x7f) << 1`.

## The boot bring-up

`FUN_08016788` is the whole RF bring-up, and the order is deliberate:

```
FUN_080093EC()   -- selects the band/filter (FUN_08013790(3), FUN_080137D4(3, 0))
                    and drives PA1 low
FUN_08009C9C()   -- drives PC13 low
FUN_08006B78()   -- BK4829 configuration (37 writes + FUN_0801BAF4's write to 0x7d + 2)
FUN_08006A0C()   -- BK4815 configuration (33 writes + the 18-register table)
FUN_0800D434()
if (state[0x20000301] == 1) FUN_080139E4()
FUN_08007F90()
```

Each chip's own init re-tests it first and, if the identity check fails, calls
`FUN_08015D14(0x0b)` and returns without configuring that part.

**What that error call actually shows is unresolved.**  `FUN_08015D14(code)`
renders one short string with `FUN_08014CF4(0x10, 0x1a, s)`, where
`s = *(u32 *)(table[index] + code * 4)`, `table` is the pointer array at
`0x08024E98`, and `index` is the byte at `0x20009F28 + 0x1c` — a runtime
configuration byte (written as `1` by `FUN_0800FE18`), whose entry 1 is a **RAM**
pointer (`0x20000258`), so the table it selects is built at run time, not in the
image.  The image does contain both candidate literals, `4815 Error`
(`0x0802710C`) and `4829 Error` (`0x08027118`, with a pointer in the pool at
`0x08027910`), and the two detects do use **different** call sites — but they pass
the **same** code `0x0b`, so the string cannot be chip-specific on the strength of
the call sites alone.  An earlier version of this write-up assigned one literal to
each chip; that claim is withdrawn, and the index-to-language-to-string mapping is
an open point rather than a finding.

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
* **`PD0`** is pulsed low → high at boot (`FUN_0801D69C`, which clears bit 0 of
  `GPIOD` at `0x48000C00`, does a handshake and sets it again) and driven low from
  a handful of other paths; it behaves as the companion chip's reset/handshake, so
  **charging and pack management are delegated to that chip**, with the MCU's
  only line to it being `PD0` plus the two-wire bus.
* The **MCU's own clock** is the other half of "power": the stock runs from the
  PLL its bootloader leaves running (`CR = 0x0040e583`, PLLON) while this firmware
  forces HSI at 8 MHz.  That is why every bit-banged bus here runs slower than
  the stock's.

A decompiler pass over the same image (Ghidra, program based at `0x08004000`)
re-derived each pin claim above from the decompiled functions rather than the
listing, and every one held.  What is still missing is the pass over the
power-on/off and sleep paths.

## The rest of the RF path

* **T/R**: `FUN_08016228` is the transmit/receive path and writes both chips.
* **Band/filter**: `FUN_080137D4` (with `FUN_08013790`) reads and rewrites
  register `0x33`; the RF bring-up calls it before configuring anything.
* **Squelch**: `FUN_080052B8` reads register `0x67` (RSSI) and makes the decision
  from it; the debug page shows it as `RSSI R67 %d`, with `0x65`/`0x63` read
  alongside.
* **TX power**: the UI carries `Power Select` (`0x08017900`), `Power 5W`
  (`0x08017910`) and `Power 10W` (`0x0801791C`) — the setting the RA89G V52
  "10 W enable" build is named for.  Which registers it lands in has not been
  traced.  The one register that *is* known to move with a build setting is the
  BK4829's `0x7d`, which the stock computes from a runtime field
  (`ra89r_bk4829.md`); whether that field is this power setting is a plausible
  but unconfirmed link.
* **Status LED**: the LED is an RF-chip indicator rather than an MCU pin
  (`ra89r_led.md`), so it comes with this bring-up.

## Open

1. **Which chip does what.**  Both are configured and used throughout; whether
   they are split by band, by TX/RX, or one is a second receiver is not
   established.
2. The power-on/off and sleep handling, and whether anything else gates the RF
   rails — the decompiler is now available, but these paths have not been walked.
3. The per-band/RX/TX routines, and where the TX power setting lands.
4. `FUN_0800D434` and `FUN_08007F90` (the other two calls in the bring-up) are
   not identified.
5. Which string `FUN_08015D14(0x0b)` actually renders, and what the byte at
   `0x20009F28 + 0x1c` selects (the language table it indexes is built in RAM).
