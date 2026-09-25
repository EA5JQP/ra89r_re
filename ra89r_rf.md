# RA89R RF transceivers — BK4829 and BK4815

**Status: both chips identified, their factory configuration extracted from the
stock image; no driver written yet.**  Two transceivers are fitted (confirmed from
board photos and from the firmware), and the stock configures both at boot.

## The two chips and the bus

They share one bit-banged bus and have **one chip select each**:

| | chip select | clock | data | write | read | id check |
|---|---|---|---|---|---|---|
| **BK4829** | `PB8` (`0x100`) | `PA12` | `PB12` | `FUN_080220A0(val, reg)` | `FUN_080180F0(reg)` | reg 0 reads **`0x4829`** |
| **BK4815** | `PB13` (`0x2000`) | `PA12` | `PB12` | `FUN_08022082(val, reg)` | `FUN_08018060(reg)` | reg 0 reads **`0x4816`** |

Evidence:

* `FUN_08009772` reads register 0 over the **PB8** primitive (`FUN_080180F0`) and
  compares the result with `0x4829`; `FUN_08009758` does the same over the
  **PB13** primitive (`FUN_08018060`) against `0x4816`.  The UI's error strings
  are `4815 Error` (`0x0802710C`) and `4829 Error` (`0x08027118`).
* Every primitive brackets its transfer with its own select line low → transfer →
  high (`FUN_08011B74(GPIOB, 0x100/0x2000, …)`).
* The bit layer is shared: `FUN_08017D6C` shifts a byte out over `PB12` with
  `PA12` as the clock, `FUN_08017FE4` shifts one in, and
  `FUN_0801DCF0`/`FUN_0801DCB8` *reconfigure* `PA12`'s direction rather than just
  flipping a level.
* The two parts frame differently: the BK4829 is addressed with `reg | 0x80`, the
  BK4815 with `(reg << 1) | 1` / `(reg << 1)`.
* Both are used heavily — ~57 call sites write the BK4829, ~33 the BK4815 — so
  both are real, fitted parts, not one part plus a footprint.

## The two boot initialisation routines

Both run early and are small enough to read linearly.  Each first re-tests its
chip and, on failure, calls `FUN_08015D14(0x0b)` instead of configuring it — that
is the "4815/4829 Error" path.

### BK4829 — `FUN_08006B78`, 39 writes

```
reg 00 = 0x8000    reg 2f = 0x9890    reg 48 = 0xb386
reg 00 = 0x0000    reg 3a = 0x9a7c    reg 49 = 0x2a32
reg 37 = 0x9d1f    reg 3e = 0x94c6    reg 4a = 0x5430
reg 13 = 0x03df    reg 3f = 0x07fe    reg 4d = 0xa015
reg 12 = 0x03db    reg 40 = 0x34f0    reg 4e = 0x6f10
reg 11 = 0x033a    reg 46 = 0x6050    reg 4f = 0x2a28
reg 10 = 0x0318                       reg 53 = 0x2028
reg 14 = 0x0210                       reg 73 = 0x6681
reg 19 = 0x1041                       reg 77 = 0x88ef
reg 1c = 0x0422                       reg 7b = 0x73dc
reg 1d = 0x2aab                       reg 7d = 0xe920
reg 1e = 0x4c58                       reg 7e = 0x303e
reg 1f = 0x165a                       reg 4c = 0xe520
reg 25 = 0x6dba    -- then FUN_0801BAF4() is called --
reg 28 = 0x0b40                       reg 48 = 0xb3b5   (rewritten)
reg 29 = 0xaa00                       reg 47 = 0x6042   (last write)
reg 2a = 0x6600
reg 2c = 0x0022
```

Order matters: the first two writes go to register 0 (`0x8000` then `0x0000`),
`FUN_0801BAF4` runs in the middle, and the last two writes (`0x48`, then `0x47`)
come after it.

### BK4815 — `FUN_08006A0C`, 33 writes plus an 18-register table

```
FUN_08021F78(0x08024E40, reg 2, 0x24)   -- 36 bytes = 18 registers, see below
reg 70 = 0xa000    reg 4b = 0xf606    reg 6a = 0xcc31
reg 28 = 0x8820    reg 4c = <RAM>     reg 6b = 0x3415
reg 29 = 0x2050    reg 54 = 0xfc46    reg 6c = 0xe927
reg 2c = 0x8a2f    reg 55 = <RAM>     reg 6d = 0x6618
reg 2d = 0x1bc0    reg 58 = 0x0208    reg 7a = 0x46a3
reg 40 = 0x8000    reg 59 = 0xf7a1    reg 7b = 0x0002
reg 41 = 0xe000    reg 5e = 0x8028    reg 7c = 0xf3ac
reg 44 = 0x8000    reg 62 = <RAM>     reg 7d = 0x76b5
reg 45 = 0x67ff    reg 67 = 0xc31f    reg 7e = 0xfff5
reg 47 = 0x0a18    reg 68 = 0x4020    reg 7f = 0x3568
reg 48 = 0xa002                       reg 0c = 0x0a03
reg 49 = 0x1a02
```

Three of its writes take their value from RAM (`0x2000449A`, `0x2000449C`,
`0x2000449E`) — registers `0x4c`, `0x55` and `0x62`, i.e. the calibration-ish
fields — so the values depend on what the boot code loaded there, not on a
constant.

**The 36-byte table** at flash `0x08024E40`, written to registers 2..19 as
big-endian 16-bit words:

| reg | value | reg | value | reg | value |
|---|---|---|---|---|---|
| 2 | `0x6fc0` | 8 | `0xff33` | 14 | `0x5817` |
| 3 | `0x0e3d` | 9 | `0xc3fa` | 15 | `0x90a3` |
| 4 | `0xb041` | 10 | `0xa2a3` | 16 | `0x88f9` |
| 5 | `0xf770` | 11 | `0x8800` | 17 | `0x5800` |
| 6 | `0xf274` | 12 | `0x0603` | 18 | `0x415c` |
| 7 | `0x08f0` | 13 | `0x09fd` | 19 | `0x08a0` |

## Other configuration in the same image

`FUN_08006B78`/`FUN_08006A0C` are the *boot* configuration.  Band, mode and
RX/TX setup live in other routines, which are the next thing to extract:

* BK4829 side — `FUN_0801740C` (18 writes), `FUN_08017280` (7),
  `FUN_08016DE8` (6), `FUN_0801754A` (5), `FUN_08018A46` (4), `FUN_08016F7C`,
  `FUN_08017668`, `FUN_08009CC4` (4 each);
* BK4815 side — `FUN_080171D0` (10), `FUN_08005C34` (8), `FUN_08016CEC` (6),
  `FUN_0800B520` (6), `FUN_08006D10` (5);
* `FUN_080052B8` reads register `0x67` (RSSI) for the squelch decision, and
  `FUN_080137D4` reads and rewrites `0x33` (band/filter selection).

## Open points

1. **Which chip does what.**  Both are configured and used throughout; whether
   they are split by band, by TX/RX, or one is a second receiver is not
   established.  `FUN_08016228` (the T/R path) writes both.
2. The three RAM-sourced BK4815 registers (`0x4c`, `0x55`, `0x62`) — where the
   boot code gets those values (calibration from the external flash?).
3. The register *semantics* are not mapped — the sequences are extracted
   verbatim, not understood.  `bk4819.c`/`bk4829.c` in the UV-K1/K5V3 tree name
   many of these registers and are the place to cross-reference.
4. `FUN_0801BAF4` (called mid-way through the BK4829 init) is not identified.
5. No RF driver exists in this firmware yet; the status LED also depends on it
   (see `ra89r_led.md`).
