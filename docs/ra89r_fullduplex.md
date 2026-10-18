# Full duplex on the RA89R (two transceivers, one front end)

The goal was satellite-style **true full duplex**: receive continuously on one
VFO's transceiver while transmitting on the other VFO's transceiver, at the same
time — e.g. listen on the BK4815 while keying the BK4829.

**Finding: the port as it stands cannot, but the board can.** The stock firmware
has a cross-band repeater mode — the **`Aut Repeat`** menu item (the manual
lists options including **Bidir**) — so simultaneous receive on one band and
transmit on the other is a capability of this radio. The limit is the port's
single-domain band/path model, not the hardware. What the port offers today is
**cross-band half-duplex**; full duplex needs the port reworked to drive each
chip's path separately, the way the stock's repeater does.

## Status (2026-10-18)

What the work so far established, in one place:

- **RF duplex is real and was measured.** The 4815 keeps its RSSI/squelch while
  the 4829 transmits, and the stock's own test received 446 while transmitting
  145.5.  The receiving half is not the problem.
- **The uplink PA is the BK4829's only.** The 4815 does not radiate with the
  BK4815 transmit set (`FUN_080171d0`/V52 `FUN_080172bc`: `0x0C=0xF823`,
  `0x6D=0xE718` — dead code in the stock, see `docs/ra89r_bk4815.md`), even
  with the shared PA enabled — no carrier on 145.5.  Any transmitted signal
  comes from the 4829.
- **The receive audio path is the blocker, and its route is now settled.**  The
  BK4815's `EARO` is summed with the BK4829's at the amplifier input — **route 1,
  radio-verified 2026-10-18** with console `o`: the 4829's microphone amplitude
  follows the operator's voice (9 → 138) but not a downlink carrier (6–8), so
  the 4815's audio is not on the 4829's microphone input.  A 4829 transmission
  therefore does **not** relay the downlink, and the downlink's audibility while
  transmitting is the amplifier-node question (the 4829's `0x47` mute loads the
  node; `0xC3FE` powers it but adds the chip's own TX audio — the beep).
- **The stock's repeater is a relay, not a duplex transceiver.**  In BIDIR the
  microphone is disconnected; the transmit input comes from the other
  receiver's audio.  The operator cannot talk through it.  Whether the operator
  *hears* the downlink during the relay is unconfirmed (the stock's TX entry
  mutes the 4829; see "Still open"), and with route 1 the stock's relay audio
  route is itself unexplained.
- **The port can already do RF-duplex with live receive** (swap mode: 4829
  receiving, 4815 keyed) — but nothing is radiated, so it is not a usable
  duplex uplink yet.
- **The remaining unknown is the node's behaviour during a 4829 transmission.**
  Muting the 4829's `0x47` silences the 4815, and the one clean duplex PTT had
  `0x47 = 0x6142` with the AF DAC off; the beep (`0xC3FE`) is that same stage
  carrying the chip's own TX audio.  Which of the two routes above is in play
  decides whether that is good news or a relay.
- **V52 static follow-up (2026-10-18):** the arming routine is `FUN_080190CC`
  (called from the per-VFO reconfiguration `FUN_08016314` and the settings
  loader), the audio switch is a **per-chip** AF gate (BK4829 `0x47`,
  BK4815 `0x49`) with no cross-chip route, and the BK4815 transmit set is
  **dead code** in both builds.  Details and the resulting satellite setting:
  "V52 static follow-up", below.


## How the port models the two transceivers

- Two BK481x parts share one bit-banged bus (`driver/rf_bus.c`), each with its own
  chip select (`PB8` = BK4829, `PB13` = BK4815).
- `driver/rf_dual.c` decides which VFO each part is tuned to: the **selected** RX
  VFO's chip always holds it, and the other chip holds the other VFO **when the
  two VFOs name different chips** (one part cannot hold two frequencies).
- Per-VFO transceiver: `SETTINGS_GetVfoTransceiver(vfo)` returns
  `RF_XCVR_BK4829` / `RF_XCVR_BK4815` (the menu items **TrVfoA** / **TrVfoB**).
- **Transmit is always the BK4829** (`driver/tx.c`) — the only part with a
  validated transmit chain — at the selected TX VFO's frequency. A VFO whose
  receive transceiver is the BK4815 still transmits through the BK4829.
- Receive audio follows the selected VFO's chip (`rx_set_audio_source()`); the two
  chips' audio shares the BK4829's AF node.

## The test configuration (and a trap)

For a full-duplex test the **RX VFO must be the BK4815** and the **TX VFO the
BK4829**, on two different VFOs, e.g.:

- VFO A = BK4815 @ 145.9, squelch 0 (downlink — the one that must survive)
- VFO B = BK4829 @ 435.1 (uplink)
- cross-band so PTT transmits on VFO B

A run with the receive on the **BK4829** while transmitting is *not* a
full-duplex test: TX uses the BK4829 too, so it simply retunes the receiving
chip — ordinary half-duplex, and the receive "dying on PTT" there means nothing.

## The probe (throwaway; a console toggle, since removed)

`tx_start()` / `tx_stop()` and `pa_select_band()` were made to leave the BK4815
alone, in stages, to find what kills its receive. Same configuration each time:
key the BK4829 on 435.1 while the BK4815 receives 145.9.

| stage | what TX no longer did to the 4815 | RX 145.9 | TX 435.1 |
|---|---|---|---|
| 1 | T/R write (`0x0C`) and TX-mode retune (`0x70`) | **dead** | OK |
| 2 | 1 + band register (`0x75`) | **dead** | OK |
| 3 | 2 + `pa_band_path()` (PA0/PA1) and `pa_apply_chip_path()` | **survives** | **lost** |

So the BK4815's own T/R and band writes are **not** what kills it — the **shared
PA/front-end pins** are. Restore any of them and the transmit returns with the
receive dead; remove them and the receive lives with the transmit gone. One set
of pins, two mutually exclusive positions.

## Conclusion

The transmit and receive paths are not independent **in the port's current
model**: it drives one band/path state, so simultaneous TX and RX on two bands
is impossible as the port is written.  The board **can** do it — the 2026-10-17
stock test received on 446.00625 and transmitted on 145.5 **at the same time**
(see the radio test in the trace) — so the work is to give each transceiver its
own band/path state and its own receive chain, capturing the stock's register
recipe for the simultaneous state (J-Link watch recommended).  Until then the
port offers **cross-band half-duplex**: RX on one VFO's band, and on PTT it
switches the front end and transmits on the other.

## The stock's cross-band repeater (`Aut Repeat`)

The stock firmware implements the repeater the product page advertises.  The
string is `Aut Repeat` (0x08027154); the **manual** lists it as the repeater
option with settings that include **Bidir** (repeat both directions).  It is
**radio-verified as a true simultaneous cross-band repeater** (2026-10-17
test: relay with receive on one lane and transmit on the other at once) — see
the trace below for the OFF/UNIDIR/BIDIR setting, the per-lane machinery and
the runtime question of the relay mechanism.

## Why a cross-band repeater can do it and a single-path design cannot

A radio built for true cross-band *simultaneous* operation needs the hardware
to present both bands at once — a duplexer/diplexer (or two antennas) at the
antenna, with enough isolation between the high-power transmitter and the
sensitive receiver.  The 2026-10-17 stock-radio test (relay: receive `446`,
transmit `145.5`, at the same time) proves this board has that capability; the
port's own stage-3 probe (BK4815 RX surviving with the shared band/path pins
held in the receive position while TX was keyed) agrees.  Independently of
simultaneity, a single shared band/path state is the port's obstacle for *any*
per-lane behaviour — cross-band monitor, relay, full duplex — and the per-lane
shape to copy from the stock is documented in the trace below.

## Advantages of a per-VFO transceiver (over one shared transceiver)

- The two VFOs can sit on **different bands at once** (each chip keeps its own
  frequency), so switching VFOs does not retune or disturb the other.
- It enables dual-band **monitoring**: dual-watch and the scan can look at both
  VFOs across two chips rather than one.
- It is the foundation of any **cross-band** operation (RX on one band, TX on the
  other) — impossible with a single transceiver.
- The chip per band can be chosen (`TrVfoA`/`TrVfoB`), e.g. to work around a
  part's weak band.
- With **one** transceiver only, both VFOs must share it: only one frequency is
  live at a time, no dual-band monitor, and no cross-band at all.

## Open / to settle

- The stock's `Aut Repeat` is **radio-verified as a simultaneous cross-band
  repeater** in BIDIR (2026-10-17 test, recorded in the trace).  The remaining
  question is the **mechanism**: capture with a J-Link watch during an active
  relay — the band-path pins (PA0/PA1), the per-chip `0x47`/`0x75` values on
  both lanes, the transmit lane's PA bias, and which chip each lane binds to —
  that register recipe is what the port's per-lane model must reproduce.
- Whether the port can adopt that per-chip path model directly, or whether some
  of it is board-specific wiring needing its own driver work.
- The probe above is evidence about **the port's model**; it also strengthens
  the board-capability reading (with `tools/rf_harness.py` it is repeatable).

## Side finding: the VFO could not be tuned

Setting this test up surfaced a separate defect, fixed on `develop`: a frequency
step was undone by the VFO reconfigure. Two faults, both only visible once
`TX_VFO != RX_VFO` (which is why tuning "worked before the cross-band change"):
`SETTINGS_SaveChannel()` stored a frequency channel at `gEeprom.TX_VFO` while
`SETTINGS_FetchChannelScanDisplayInfo()` read it at `gEeprom.RX_VFO`, and
`RADIO_ConfigureChannel()` re-read a frequency channel's own frequency on every
reconfigure — including the one that runs in the same pass as a step, before the
deferred save wrote it. See the commit "fix(vfo): stop a frequency step being
undone by the reconfigure".

See also: `docs/ra89r_rfpath.md` (the RF path), `docs/ra89r_bk4829.md`,
`docs/ra89r_bk4815.md`, `docs/ra89r_port.md`.

## Stock repeater trace (firmware `stock_v49_raw`, Ghidra, base 0x08004000)

Traced from the Ghidra program `stock_v49_raw` (the decoded stock V49 image,
flash `0x08004000..0x0802796C`, 1229 auto-analysed functions; project `ra89r_re`
at `~/ra89r_re`).  **Everything below is located from the disassembly — none of
it has run on the radio.**  Names are the auto-generated `FUN_/DAT_` ones so the
session's findings stay cross-checkable.

The repeater is real and its options are **OFF / UNIDIR / BIDIR** — a cross-band
repeater with a direction setting:

| what | address |
|---|---|
| `Aut Repeat` (menu label) | `0x08027154` |
| `OFF` / `UNIDIR` / `BIDIR` option strings | `0x08026b90` / `0x08026cec` / `0x08026ce4` |
| page-1 label/value table (label + the three options) | `0x08024ed0` |
| page table indexed by the draw code | `0x08024ee0` (entry 1 → `0x08024ed0`, the `Aut Repeat` page) |
| draw dispatcher | `FUN_0801b2dc` — **item `0x24`** (`'$'`), label table `0x08024ee0`, page byte `settings+0x1c`, value index `0x2000007d` |
| value dispatcher | `FUN_0800fb58` — item `0x24` case reads `settings+0x10` |
| **the mode byte** | **`0x20009f38` (`settings+0x10`): 0 = OFF, 1 = UNIDIR, 2 = BIDIR** |
| menu edit path (writes the mode) | `FUN_0800fe18` |
| settings save/load (persists it) | `FUN_08020d6c` |

### Arming (FUN_08018fe0)

Runs at the end of every per-VFO reconfiguration (`FUN_08016228` →
`FUN_08018fe0`) — i.e. the stock re-evaluates the repeater state after each
re-tune.  With page `settings+0x1c == 1` and mode (`settings+0x10`) != OFF it
compares the two VFO objects (`0x20009C14` vs `0x20009D98`, via
`FUN_0800e7e2(obj, 0/1)`) and:

- **mode == 2 (BIDIR)** → repeat on unconditionally.
- **mode == 1 (UNIDIR)** → repeat on only when the saved page context
  (`0x20000310` via `DAT_080190a0`) equals the current one.
- otherwise → disable: writes `0x20000302 = 0` **and drives GPIOA bit 0** (a
  band-path pin, `FUN_08011b74(0x48000000, 1, 0)` = `BRR = 1`).

The repeat-enabled flag is the app-level byte **`0x20000302`** (sibling of the
per-VFO chip selector `0x20000303`).

### Globals resolved (RAM)

| address | meaning (inferred from use) |
|---|---|
| `0x20000302` | **repeater active** — set/cleared by `FUN_08018fe0`, read by ~30 sites |
| `0x20000303` | per-VFO chip binding (already documented); also the *lane* selector fed to the per-lane squelch evaluator |
| `0x20000310` | settings-page context (also the menu-open flag checked at `FUN_0800fb58`'s head) |
| `0x20000314` | app-state byte gating `FUN_08016228` / `FUN_0801bde8` |
| `0x20000324` | **active lane byte** (0/1) — written by `FUN_08016998`, selects the lane table in the squelch walkers |
| `0x20000326`/`0x20000327` | two counters cleared by the lane tick `FUN_08004c84` |
| `0x20000336` | the 0..8 AGC-squelch step counter (`FUN_080052b8`) |
| `0x20009c14` / `0x20009d98` | the two VFO channel objects (lane A / lane B) |
| `0x20009f28` | the settings struct (`+0x10` Aut Repeat; `+0x07..0x0a` = the four **side-key function assignments** P1/P2 Short/Long — see below; `+0x11` level, `+0x1c` page, `+0x31` unknown) |
| `0x20009f80` | key-event struct (`[3]` = key id, `[4..9]` qualifiers) — read by `FUN_0801a228` |
| `0x20003ddc` | scratch object with a flag at `+2`, used by the timer pair `FUN_0801e558`/`FUN_0801e75c` |

### The per-lane RF stack (the shape the port lacks)

This is the stock's answer to "two chips, one bus" — everything below is
parameterised by a lane, not global:

- **Register writes are chip-specific — resolved to the pins**:
  `FUN_080220a0(value, reg)` → `FUN_08021ff4` selects **PB8 (BK4829)** and
  masks the register `&0x7f` (the BK4819-family 16-bit frame);
  `FUN_08022082(word, reg)` → `FUN_08021f78` selects **PB13 (BK4815)** and sends
  the register as `(reg & 0x7f) << 1` (the BK4815's "differently framed" access,
  clock/data on PA12/PB12).  Both are plain wrappers over the same bit-bang
  sender `FUN_08017d6c`, differ only in select pin and register shift.  Corollary
  confirmations: `FUN_08013790` (band register `0x75`) uses the PB13 helper;
  the readers pair the same way (`FUN_08018060` = BK4815 bus,
  `FUN_080180f0` = BK4829 bus).
- **Per-lane squelch**: `FUN_080051e4(vfo)` (10-step counter at `vfo+0x20`,
  escalating to `FUN_08004c84` after 9) → `FUN_0801d420` →
  `FUN_08005440(laneByte, *0x20000303)` → `FUN_080052b8` (lane 1: reads `0x63`
  noise, `0x67` RSSI `&0x1ff`, `0x65`, walks AGC `0x13 = 8-count|0x3b0` between
  0xcf/0xb4, counter `0x20000336`, prints `"RSSI (R67): %d"` to the console) or
  `FUN_08005218` (lane 0: reads `0x44`/`0x43` of the *other* accessor,
  `FUN_08018060` vs `FUN_080180f0`).
- **Per-lane tick state machine** `FUN_08004c84(vfo)`: lane-activate
  (`vfo[0x1e] = 1`, per-band AF mute `0x3f` = 0x800/0x7fe, `FUN_080164b0`), or
  lane RX-off (`FUN_0801d458`: chip reg pair `0x42`/`0x5a` or mute `0x3f`,
  **green LED PA14 off**, `FUN_0801cc84` audio restore), then dispatch on
  `vfo[0x7d]/[0x28]/[0x29]` → `FUN_08016228` (full per-VFO register config:
  `0x47` = 0x6042/0x6142/0x6740 band/GPIO variants, `0x13` squelch,
  `0x30` = 0xbff1, scramble bit on `0x31`) or re-arm (`FUN_080161d8`).
- **Lane switch** `FUN_08016998(lane)`: writes `0x20000324`, re-steers the lane
  object with `FUN_0800d684(VFO_obj, lane, 1)` + `FUN_0801cc84`.
- **Transmit chain bring-up** `FUN_0801bde8(vfo)`: PA bias from the *power
  table* `FUN_080201cc` (two-pass gain selector at `vfo+0x92` — the port's
  missing "TX power from the codeplug" half), `FUN_08018a88(compare)`, chip
  GPIO via `FUN_080137d4(3, …)`, **PA1** = BSRR bit 1, and the **BK4815 band
  register `0x75`** via `FUN_08013790(3)`.
- **Chip-specific teardown** `FUN_0801d798`: writes `0x70 = 0` to one chip(*)
  or clears bit `0x0200` of `0x75` on the other — each lane ends with its own
  MSK/tail register pair, then drops **PC13** (audio path) via `FUN_08009c9c`
  (`GPIOC BRR = 0x2000`).
- **App dispatcher** `FUN_0801a228` has a dedicated repeater UX (mode `7`): the
  keys dispatch the **side-key functions** (`settings+0x07..0x0a`) that fire
  while the repeat is on (e.g. an assigned Power Select re-runs the lane
  teardown + relaunch), reconfigure the *active* lane only via
  `FUN_08013f28(lane_obj)` (`FUN_080166f8`), and clearing the mode byte also
  clears `0x20000302`.
- **The state machine**: `FUN_0800c550(lane obj, mode)` →
  `FUN_08016a2c(obj, mode)` is the stock's enter-state routine — arms the lane
  (`FUN_0800b380`), sends the entry signalling codes (`FUN_0801a5e4`, from
  `obj+0x79/0x7b/0x7c`), configures the **TX chain on that same lane**
  (`FUN_0801830c`, with the PA pulse/bias ramp `FUN_0801be90(1)` on enter and
  `(0)` on exit), runs that state's own key/audio loop
  `FUN_0801a228(obj, mode)`, then tears down (`FUN_0800bfc0`/`FUN_0800bf80`,
  `FUN_0801be90(0)`).
- The **normal automatic lane tick** (`FUN_08004aa4` → `FUN_08005bd0(7,1)`,
  the `FUN_08004c38` driver) is gated **off** while `0x20000302 == 1` — the
  repeater runs on its own tick path.  `FUN_08004aa4`'s other branch
  (`settings+0x31`, an 8-second window with `FUN_0801e75c(8000)` and the
  scratch object `0x20003ddc`) turned out to be **not the repeater**: it is the
  external-flash sample/playback engine (`FUN_080049f8` reads 8-byte rows of a
  table at flash offset `0x40000` through the stock's own SPI fast-read
  `FUN_08018c0c`, chip-select GPIOA bit 15, which the repeat never touches).

- The `settings+0x07..0x0a` bytes are the four **side-key assignments**
  ("P1 Short", "P2 Short", "P1 Long", "P2 Long", item `0x2b..0x2e`, label table
  `0x080254a8` page 1) with function values: None, VOX, Dual Wait (sic), Scan,
  Monitor, 1750 TONE, Power Select, Alarming, Temp Monitor, FM Radio, Talk
  Around, Fre Reverse (sic) — no repeater toggle among them.  `FUN_0801a228`'s
  repeat-mode key checks (`settings+7/8/9/10 == 5/6/7…`) are those side-key
  functions *working while the repeat is on* (e.g. Power Select re-runs the
  lane teardown + relaunch), not repeater configuration.

### The repeat engine in the main loop

`FUN_08024448` is the main app loop; **every one of its passes calls
`FUN_08006714`** — the alternator:

```c
if (0x20000302 == 0) return;             // gate = the repeater flag
GPIOA0_write(1);                          // PA0 (band-path pin)
if (0x20000324 == 0) { 0x20000324 = 1; FUN_0800c550(0x20009C14, 9); }
else                 { 0x20000324 = 0; FUN_0800c550(0x20009D98, 9); }
```

so while repeating the radio **alternates the monitored lane on each main-loop
pass**, re-entering the app state in `mode 9` ("cross-band repeat") on the
other lane's channel object.  Mode 9's loop ticks the monitored lane's squelch
(`FUN_08018710` → the counter chain) and **exits when that lane stops
receiving** (`obj+0x1c == 0 && obj+2 != 1`, which also clears `0x20000302`),
handing control back to the main loop, whose next pass starts the other lane's
turn.

**Verdict — revised by the radio test (2026-10-17).**  The static reading of
the code above is an **alternating scheduler**: enter mode 9 on one lane, run
while it receives, exit, flip.  But the radio test below shows the stock's
BIDIR repeat **does receive on one lane and transmit on the other at the same
time** during a carrier.  So the alternation seen statically is evidently the
lane-selection/idle machinery, and the actual relay keys the *other* lane's
transmitter while the monitored lane keeps receiving — the mechanism of that
(same-band-path pins held? per-chip `0x47`/`0x75` both set?) is exactly what a
J-Link run should now capture.  The static text stays as the located record;
the radio observation outranks it.

### Radio test (2026-10-17, stock firmware V52)

Setup: radio A transmits on `446.00625`; the stock radio has VFO A = 145.5,
VFO B = 446.00625, `Aut Repeat` = **BIDIR**; radio C listens on 145.5.
Result: radio C receives radio A's transmission **through the stock radio**,
and the stock is heard receiving (`446`) and transmitting (`145.5`)
**simultaneously** — a true cross-band repeat, continuous audio on radio C.
This is the first hardware proof that the board presents both bands at once
(receive on one lane while the other transmits); it outranks the static
alternator reading and restores the "the board can do it" premise of the port's
full-duplex goal.  Still to capture: the exact register/pin recipe of the
simultaneous state (band path pins, per-chip `0x47`/`0x75` values, PA bias on
the transmit lane, and which chip each lane uses here).

### What the repeat entry does to the audio registers (traced 2026-10-17)

Path: `FUN_0800c550(lane, 9)` → `FUN_08016a2c(lane, 9)` →
`FUN_08018ab8(lane)` → `FUN_08017306(lane, 1)` → **`FUN_08017280(lane)`**.  That
last routine is the lane's **transmit-audio setup**, and for the repeat it
writes the same microphone configuration as a normal transmission:

| write | value | meaning |
|---|---|---|
| `0x24` | `0` | (spur) |
| `0x47` | `0x6142` | AF output = normal (then overridden, below) |
| `0x37` | `0x9D1F` | mode restore |
| `0x47` | `0x6040` | via `FUN_08017628(0)` — **AF output muted** |
| `0x50` | `0x3B20` | **TX audio unmuted** |
| `0x70` | `0` | idle/config clear |
| `0x30` | `0xC1FE` | **TX DSP + mic ADC** (the normal TX value) |
| `0x40` | `old & 0xE000 \| 0x1000 \| gain<<4` | **mic gain** (`FUN_0801c564` → `FUN_0801c3a8`) |

and it also tunes the chip (`FUN_08017176` → `FUN_08017158`, the raw
`0x38`/`0x39` write) and the entry drives **PC13 low** (twice:
`FUN_08018ab8` and `FUN_0801830c`) while clearing the lane's RX flags
(`lane[0x1e]`, `lane[0x1c]`).  The BK4815's own entry set is different
(`FUN_080171d0`: `0x12=0x415C`, `0x0C=0xF823`, `0x16=0x3200`, `0x2B=0x803F`,
`0x6D=0xE718`, `0x73=0x8000`, `0x11=0x5800`).

**Consequence for satellite full duplex:** at register level the repeat entry
prepares the transmit lane for the **microphone** (mic ADC + TX unmute +
mic gain) and *mutes that chip's AF output* — there is no "route RX audio into
the modulator" write anywhere in the path.  The audible receive must therefore
come from the **other lane's chip**, still in its RX configuration
(`0x30 = 0xBFF1`, `0x47 = 0x6142` normal AF out) — i.e. the stock's duplex
audio split is exactly the one a satellite needs: one chip RX→speaker, the
other mic→TX.  The only remaining risk is board-level: whether the RX chip's
AF node is physically summed into the TX chip's mic input (if it were, the
downlink audio would leak into the uplink).  The stock test for that is to talk
into the stock's mic during a BIDIR relay and listen on the far radio; the
register trace says the firmware does not add the downlink to the uplink.

Observed values for the record: RX `0xBFF1` (and `0xBDF1` under VOX), TX
`0xC1FE`, the exit-chime/beep variants `0xC3FE`; AF mute `0x6042`/`0x6040`,
AF normal `0x6142`, `0x6740` (CTCSS/tone group); TX unmute `0x3B20`, muted
`0xBB20`; mic gain via `0x40` (`FUN_0801c3a8`, computed — not an immediate).

**Register 0x30 bit 9 is the AF DAC**, and it is the audio switch that matters
for duplex on the port: RX (`0xBFF1`) has it on, the normal TX word (`0xC1FE`)
has it off — so a transmitting BK4829 cuts the AF node that carries the
BK4815's audio to the speaker.  The stock's own `0xC3FE` (`FUN_0801a6d0`,
`0xC1FE | AF_DAC`) is "TX with the AF output stage alive", which is the value
the port's duplex TX uses (console `H` toggles it).

### Repeatability of the port's obstacle

The probe stages (above) match what the stock does in software: `FUN_08016228`
drives `0x47` (the chip GPIO/band register) per VFO, `FUN_0801bde8` sets the
band path pins (PA0/PA1) and `0x75` per lane, `FUN_0801d458`/`FUN_0801d798` tear
each lane down independently.  The stock keeps **per-lane state** (lane bytes
`0x20000324`/`0x20000303`, per-lane channel objects, per-lane register helpers),
which is exactly the capability the port's single-domain
`pa_band_path()`/`pa_apply_chip_path()` model lacks.

### Testing the duplex mode on the port (console `U`)

The port now has a console-driven duplex test (`firmware/App/driver/tx.c`,
`tx_set_duplex`; commands in `App/main.c`).  Setup, in order:

1. **Bind the chips**: menu **TrVfoA/TrVfoB** — the **receive** VFO to **BK4815**,
   the other VFO to **BK4829** (the only measured transmit chain).  `U` now
   forces this itself (and the transmit VFO), so no menu step is required — it
   restores whatever bindings it found when you send `U` again.
2. Set the receive VFO to the downlink and the other VFO to the uplink
   (e.g. RX 145.500, TX 446.00625 — the satellite split, either way round).
3. Select the downlink VFO (RX), then send **`U`** on the console
   (115200 8N1).  `U` refuses — with the reason — unless the two VFOs name
   different chips with RX on the BK4815 and TX on the BK4829.

While duplex is on, `U` again turns it off (TX returns to the receive VFO).
Two live toggles exist because the stock's recipe leaves two questions open
that the radio settles best:

- **`I`** — the transmit chip's AF output during a transmission:
  `1` = Normal AF (`0x6142`, **the default**) — required on this board: the
  BK4815's audio leaves through the BK4829's AF node, so muting the 4829 at TX
  entry silences the downlink even while its RSSI stays at the carrier level
  (measured 2026-10-17: `rssi4815=50`, speaker silent with `0x47=0x6040`);
  `0` = the stock's TX-entry mute (`0x6040`), `2` = the K1 mute (`0x6042`).
- **`H`** — the transmit chip's **AF DAC** (`0x30` bit 9): off = `0xC1FE`
  (default; the K1 TX chain), on = `0xC3FE`.  `0xC3FE` is the stock's **beep**
  word: with TX DSP running it loops the transmit audio to the speaker (a loud
  beep on PTT, heard 2026-10-17), so it is not a receive route.
- **`g`** — PC13 (the audio-path line) during TX: leave as RX had it (default),
  drive low (the stock's T/R state), or drive high.  `rx_poll` no longer touches
  the line while `tx_active()`, so this is the value the transmit side sets.
- **`O`** — the shared band-path pins: `0` = full transmit path (PA0/PA1
  written, default), `1` = leave them as the receive path left them (the
  transmit may lose power; it tells whether those pins desense the receiver).
- **`o`** — the **route probe** (needs `U` first): is the BK4815's downlink
  audio on the BK4829's microphone input?  It opens the 4815's own AF gate
  (`0x49 = 0x1A0F`; the app leaves it shut while the squelch is closed and the
  console blocks the poller), mutes the 4829's AF (so the speaker cannot be
  picked up acoustically), powers the 4829's mic path as the transmit entry does
  (`0x30 = 0xC1FE`, `0x50 = 0x3B20`, mic gain in `0x40`) **without the PA**, and
  samples the chip's VOX amplitude (register `0x64`, the stock's own reader).
  It first proves the reading with the operator's voice (and retries with the
  stock's VOX mic word if the transmit one does not respond), then compares the
  level with and without the operator keying radio A.  Verdict: an amplitude
  that follows the carrier means **route 2** (a BK4829 transmission relays the
  downlink on the air); no correlation means **route 1** (the downlink is only
  summed at the amplifier).  It neither keys nor radiates.
- **`7`** — the **guided receive diagnostic** (any configuration; release PTT
  first).  It opens the receive gates itself (the console blocks the app's
  poller; the 4815's AF is opened at maximum volume), measures a quiet RSSI
  baseline (6 s), asks for radio A keyed on the receiver's frequency (12 s),
  then asks you to listen and answer `1` = audio, `2` = noise, `3` = nothing.
  If the answer is not "audio" it walks four more listening steps: (4) **rewrite
  the 4815's frequency word** (a stale tuning word leaves it off-channel); (5)
  re-assert the whole receive state; (6) **mute the idle BK4829's AF** — the
  other VFO's demodulated noise; (7) **reopen that AF stage with the BK4829's
  own AF DAC off** (`0x30 = BDF1`), the candidate fix for the 4815 lane.  The
  answers separate "the 4815 was off-channel" from "a drifted gate" from "the
  idle chip's noise masks the downlink" (audio when muted) from "the 4815's
  audio needs the 4829's AF stage" (muting it silences everything) from "the
  noise is the 4815's own demod" (noise stays) from "the 4815's audio is absent"
  (silence even with the stage open and its DAC off).  Alongside the ear it
  prints the **4815's own level readouts** (`0x49` bits 14:07, the datasheet's
  read-only RX audio level, and `0x4A`/REG74) idle vs carrier; on this part they
  proved noisy and do not track the carrier, so they are informational and the
  ears decide.  Every register it reads is printed with the expected value,
  mismatches are called out as warnings (`0x6D` still `E718`, `0x30` not
  `BFF1`/`BDF1`, `0x47` AF muted, PC13 low), and the run ends with a
  `VERDICT:` line.  Timed (~3 minutes); ENTER starts it early.

Every 500 ms the console prints (and on PTT transitions):

```
[dup] tx=0 sql=1 rssi4815=212 4815 0x0C=A03 0x49=.. | 4829 0x47=6142 0x30=...
[dup] TX keyed (BK4829) -- BK4815 RX must continue
```

so the test can be judged without a debugger: **`rssi4815` must stay alive
(noise floor vs. a carrier on the downlink) while `tx=1`**, and the far radio
must hear the microphone.  Failure signatures: `rssi4815` collapsing at TX
points at the front-end/path pins (try `O`); the downlink going silent but
`rssi4815` alive points at the AF routing (try `I`); no modulation on the far
radio points at the BK4829 TX config (check the 0x30/0x50 read-backs).

### The automated self-test (console `E`)

`E` runs the whole local sequence without any setup discipline: it force-binds
the receive VFO to the **BK4815** and the other VFO to the **BK4829**, tunes the
receive chip to the RX VFO's frequency, measures the idle RSSI (5 samples over
1 s), then transmits a **2 s tone** on the TX VFO once per variant and samples
the receive lane's RSSI every 100 ms through it:

| variant | what it changes |
|---|---|
| `af=0 path=0 tr=0` | stock-like: TX-chip AF muted (`0x47=0x6040`), full band path |
| `af=1 path=0 tr=0` | TX-chip AF normal (`0x6142`) — the audio check |
| `af=0 path=1 tr=0` | PA0/PA1 left as the receive path had them |
| `af=0 path=0 tr=1` | additionally writes the BK4815 T/R (`0x0C=0x0203`) at TX entry |

One line per variant, with a verdict — e.g.:

```
[selftest] af=0 path=0 tr=0 | idle=41 | tx min=40 avg=42 | RX SURVIVED TX
[selftest] af=0 path=1 tr=0 | idle=41 | tx min=39 avg=41 | RX SURVIVED TX
[selftest] af=0 path=0 tr=1 | idle=41 | tx min=6  avg=9  | RX LOST DURING TX
```

Then it restores the bindings and duplex state.  The panel is frozen ~15 s
while it runs (the console blocks the app loop).  **Judge the carrier against
the layout's TX-only floor**: ~11 counts in the 145.5-TX layout and ~41 in the
446-TX layout (measured 2026-10-17, no carrier), so a weak carrier in the
446-TX layout can be mistaken for the port's own TX.  The verdict compares the
minimum during TX against idle (±4 counts).

**Radio result (2026-10-17, layout B: RX 446.00625 / TX 145.5, carrier
`idle=47`):** all four variants held `tx min=47` — the receive lane survives
the transmission, including the variant that writes the BK4815's T/R
(`0x0C = 0x0203`).  In layout A (RX 145.5 / TX 446) the same test with no
carrier showed the TX raising the receive floor 17 → 41, i.e. that layout
desenses the receive lane; the satellite configuration should be downlink 446 /
uplink 145.5 (the stock test's layout).  The far receiver is still the one
thing the radio cannot check itself: listen on the transmit frequency.

The BK4829 does not answer register reads reliably while transmitting (the
values do not match the writes and fluctuate between samples), so the console
prints the **written** transmit configuration during TX instead of read-backs.

**Radio progress (2026-10-17):** with `U` forced (RX 446.00625 on the 4815,
TX 145.5 on the 4829), the **first PTT after `U` was clean** — the downlink was
audible during the transmission and radio C heard the voice.  Subsequent PTTs
degraded to noise.  Two suspects are being separated with the console: the
4815's receive/demod setup (re-applied after each duplex TX and on each squelch
opening) and the 4829's own audio mixing into the shared node (console `z`: with
the bridge off the 4829 is hard-muted every poll, overriding the K1 app's
writes).

### Swap mode (console `y`): RX on the BK4829, TX on the BK4815

This mode was built on the reading that the stock's repeat entry programs the
**BK4815 for transmit** (`FUN_080171d0`, including `0x6D = 0xE718`, the
datasheet's transmit PA value).  **That reading is withdrawn** — see "The 4815
transmit set is dead code" below: the routine exists in both builds but its only
call site is the mode-0 branch of an audio dispatch whose sole caller passes 1,
so the shipped stock never uses it.  `y` nevertheless forces that arrangement on
the port, and it is the arrangement that would be needed for satellite mode:

- receive VFO → **BK4829** (its AF stays valid, so the downlink is audible),
- the other VFO → **BK4815**, transmit VFO = the other,
- the 4829 is **never written** during a transmission; the 4815 gets the stock's
  transmit set (`0x12=0x415C`/`0x0148`, `0x0C=0xF823`, `0x16=0x3200`,
  `0x2B=0x803F`, `0x6D=0xE718`, `0x73=0x8000`, `0x11=0x5800`, `0x49=0x1A02`).

The console prints `[swap]` lines: `tx`, the 4829's squelch/RSSI and its RX
registers, and the 4815's written transmit set.  `y` again restores the
bindings.  The open question this mode answers is whether the **microphone** is
wired to the 4815's modulator (the stock's repeat only relays the RX audio, so
it cannot tell us): if radio C hears the voice while the port keeps playing the
downlink, the satellite configuration works; if not, the board supports the
stock's relay but not a satellite uplink.

### V52 (RA89G 10 W) address map

The radio runs the **RA89G V52 "10 W enable"** build
(`docs/Ra89G_R_UpDataFile20260401_V52_10W_Enable.icf`, decoded to
`work/ra89g_v52.bin`, analyzed to `work/analysis_v52/`, and loaded into the
Ghidra project as `ra89g_v52.bin`).  The code is the same, but the shift is
**not uniform** (the image grew 292 bytes; some functions moved earlier), so use
these V52 addresses for a J-Link session, not V49 + a constant:

| role | V49 | **V52** |
|---|---|---|
| alternator (`FUN_08006714`) | `0x08006714` | **`0x08006714`** |
| lane handler | `0x080166f8` | **`0x080167e4`** |
| enter-state | `0x08016a2c` | **`0x08016b18`** |
| lane dispatch | `0x08016b1c` | **`0x08016c08`** |
| TX hub | `0x0801830c` | **`0x080183f8`** |
| TX bring-up | `0x0801bde8` | **`0x0801bed4`** |
| PC13 helper | `0x08009c9c` | **`0x08009ca4`** |
| lane activate | `0x08018ab8` | **`0x08018ba4`** |
| audio dispatch (only `(lane,1)` is ever called) | `0x08017306` | **`0x080173f2`** |
| 4829 TX-audio entry | `0x08017280` | **`0x0801736c`** |
| 4815 TX set (dead code, below) | `0x080171d0` | **`0x080172bc`** |
| per-VFO reconfiguration | `0x08016228` | **`0x08016314`** |
| **repeat arming** | `0x08018fe0` | **`0x080190cc`** |
| settings load/save (re-arms) | `0x08020d6c` | **`0x08020e58`** |
| AF restore applier | `0x0801cce8` | **`0x0801cdd4`** |
| AF mute applier | — | **`0x0801601c`** |
| 4815 lane RX config (port's `bk4815_enter_rx`) | `0x08016cec` | **`0x08016dd8`** |
| 4829 lane config (`0x47 = 0x6042`, `0x4a`) | `0x08016de8` | **`0x08016ed4`** |
| lane ticker | `0x08018710`/`0x0801862c` | **`0x08018718`** |
| app key/audio loop | `0x0801a228` | **`0x0801a314`** |
| `Aut Repeat` string | `0x08027154` | **`0x08027274`** |
| `BIDIR`/`UNIDIR` strings | `0x08026CE4`/`0x08026CEC` | **`0x08026E04`/`0x08026E0C`** |

RAM globals are the same in both builds: repeat flag `0x20000302`, lane byte
`0x20000324`, settings `0x20009F28` (Aut Repeat at `+0x10`), page context
`0x20000310`, and the band-derived chip binding `0x20000303` (1 = BK4829,
`<= 134.01 MHz`; 0 = BK4815).

### V52 static follow-up (2026-10-18): arming, AF gates, and the 4815 transmit set

Program `ra89g_v52.bin` in the Ghidra project (1230 functions).  Nothing below
ran on the radio: addresses and register values are located, the conditional
logic is the decompiler's.

**Arming is `FUN_080190CC`.**  Its two callers are the per-VFO reconfiguration
`FUN_08016314` (`bl` at `0x08016452`; the V49 `FUN_08016228` body, which sets
`vfo[0x1c] = 1` and then arms) and the settings loader `FUN_08020E58` (four
calls; the V49 `FUN_08020D6C`).  The body is the V49 `FUN_08018FE0` body with
the literals shifted: gate byte `0x20003cac + 0xf7` (V49 `DAT_08019090`),
settings mode `0x20009f28 + 0x10`, the lane objects `0x20009c14`/`0x20009d98`,
page context `0x20000310`, flag `0x20000302`, and PA0 written low through
`FUN_08011C48(0x48000000, 1, 0)`.  A J-Link breakpoint on `0x080190CC` therefore
catches every arm, and one on `0x08016452` catches the re-tune that triggers it.

**The AF switch is a per-chip gate pair, not a cross-chip route.**
`FUN_0801601C` is the *mute* applier (called from the lane tick `FUN_08004AA4`
at `0x08004AB8` and from `FUN_08005BEC`), `FUN_0801CDD4` is the *restore*
applier (called from the squelch-open path `FUN_08015700` at `0x08015722`), and
`FUN_0801D380` is the same idea for one lane object on the BK4829 side only (AF
normal `0x6740`/`0x6142` while the lane is active, `0x6042` when it is not; no
BK4815 branch).  The two appliers select on `0x20000303` (1 = BK4829 lane,
0 = BK4815 lane — `docs/ra89r_bk4815.md`):

| binding | BK4829 write | BK4815 write |
|---|---|---|
| 1 (BK4829 lane) | `0x47`: mute `0x6040`/`0x6042`, normal `0x6740`/`0x6142` | — |
| 0 (BK4815 lane) | — | `0x49` (= datasheet REG73): mute `0x9A02`, normal `0x1A02` |

`0x47` bits 11:8 are the AF-output select (`0` mute, `1` normal; `0x6740` is
select 7 — the K1's enum calls it `BK4819_AF_AM` — chosen when the VFO's
`[0x75]` flag is set, and then `0x13 = 0x3FF` and `0x4A = 0x5450` change too).
`0x49` bit 15 is the BK4815's output mute and the low nibble its volume; the
port's boot table already writes `0x49 = 0x1A02`.  **No register write anywhere
routes one chip's AF into the other**: on the BK4815 lane the per-VFO
configuration writes only its `0x0C = 0x0203`, the only AF *gate* the stock
touches for that lane is the BK4815's own `0x49`, and its RX/demod setup
(`0x16`, `0x2B`, `0x6D`, `0x73`, `0x70`, in `FUN_08016DD8`) is likewise all on
the BK4815.  The firmware's model is two independent AF sources at the
amplifier, so the 4815's audio reaches the speaker by one of two board routes,
and the image cannot choose between them:

1. **Summed at the amplifier input** with the BK4829's `EARO` (and the
   BK1080's outputs) — the `docs/ra89r_bk1080.md` reading.  The port's mute
   measurement then means a muted BK4829 output stage loads the summed node.
2. **Injected into the BK4829's microphone input**, the part's only other audio
   pin.  That is the path the stock's TX entry configures (mic ADC + TX DSP +
   `0x40` mic gain), it is exactly what a relay needs, and it would explain why
   the repeat entry programs the 4829's TX audio even when the 4829 is not the
   relay's receiver.

The separating test is one radio experiment, now implemented as console `o`
(needs `U` first): the speaker is muted (so it cannot be heard acoustically
either way), the 4829's microphone path is powered exactly as at transmit entry
but the PA is left off, and the chip's own VOX amplitude (`0x64`, the stock's
`FUN_08017618` read) is sampled with and without a carrier on the downlink.  An
amplitude that follows the carrier means the 4815's audio is in the 4829's mic
path (route 2: keying the 4829 would relay the downlink); no correlation means
route 1.

**Radio runs (2026-10-18).**  The first `o` run on 145.5 printed "route 1" but
was invalid: `rssi4815` went 10 → 33 on the carrier while the mic amplitude
stayed 7–10 — but when `o` is pressed the receive squelch is shut, so the app's
poller had the **BK4815's own AF gate shut** (`0x49 = 0x9A02`), and the console
blocks that poller for the probe's duration, so there was no 4815 audio on the
node at all.  The probe was changed to open the gate itself
(`0x49 = 0x1A0F`) and to require a **microphone control** before reporting a
route: the operator's voice must move `0x64` (it retries with the stock's VOX
word, `0x30 = 0xBDF1` + `0x31` bit 2, if the transmit word does not respond).

The second run is valid: control quiet `0x64` 9 → voice 138 on the TX mic word,
and through the carrier (`rssi4815` 30, 10 samples) the amplitude stayed 6–8
against 9 without it.  **The BK4815's audio is not on the BK4829's microphone
input** — route 1, measured in exactly the state a satellite uplink would use
(PC13 left as RX had it, PA off).  So a 4829 transmission does not carry the
downlink; the operator's microphone goes out alone.  What this does not cover:
the stock's own TX entry (`PC13` low, `0x37 = 0x9D1F`, `0x70 = 0`), which
matters to the stock's relay, not to our uplink; and the air-side end-to-end
check (key the port silent while radio A transmits — a far receiver on the
uplink must hear nothing).

**Satellite setting implied by the gates:** with route 1 the downlink is local
audio only, so the remaining question is whether it can be *heard* while the
4829 transmits.  Keep `0x47 = 0x6142` (unmute, so the summed node is not
loaded), then compare the two `0x30` words by ear with radio A transmitting and
the operator silent: `0xC1FE` (`H` off) carries no TX audio but the AF DAC is
off, and `0xC3FE` (`H` on) powers the node and adds the chip's own TX audio (the
beep).  The clean first PTT of 2026-10-17 says one of those combinations was
audible; the ear test says which, and whether the downlink rides under the
beep.  Either way the other remaining defect is not an audio-switch register: it
is what a 4829 transmit cycle does to the BK4815's receive/demod state (the
"clean first PTT, then noise" signature),
which `bk4815_enter_rx()` already tries to undo.

**Ear test, first run (2026-10-18, console `T`, layout A: RX 145.500 on the
4815 / TX 446.00625 on the 4829, beep state `0x47=0x6142` + `0x30=0xC3FE`).**
With radio A's carrier on the downlink (`rssi4815` idle 30–31), keying the
port collapsed the lane to `min 7 / max 16` for the length of the transmission
-- the carrier left the lane entirely, and the operator heard **the beep only**
(answer 2).  This re-confirms the 2026-10-17 layout-A desense (there: no
carrier, floor 17 → 41; here: a carrier, 31 → 7–16), now with the downlink
present: a UHF uplink blocks a VHF downlink lane.  It is **not** an audio-gate
verdict — the written transmit config was exactly the intended beep state
(`0x47=0x6142`, `0x30=0xC3FE`), so nothing in the AF switch explains the
silence.  The test's printed verdict ("the beep masks it, mix/level") misfired
because it weighed the 0x49 level meter, which does not track the carrier; it
now compares the TX RSSI minimum against idle (±4, the self-test's rule) and
warns when the split is the known-bad way round.  The run also shows the app's
own poller healthy afterwards: squelch opens at `rssi4815` 32 / closes at 10,
`0x6D` stayed `0x6618`, `0x75 = 0x0011`, and the tuning word (`0x71/0x72` =
`864e c4ec`) still decodes to 145.500 MHz — the demodulator set survives a
normal (non-duplex) TX cycle.  What remains: run `T` in **layout B** (downlink
446.00625 on the 4815, uplink 145.5 on the 4829), the split whose lane held
`47 → 47` in the self-test; a "beep only" there with the lane alive would be
the mix/level question, and answer 1 the first true duplex-audio pass.

**Ear test, second run (2026-10-18, console `T`, layout B: RX 446.00625 on the
4815 / TX 145.5 on the 4829, beep state; build predates the verdict fix).**
Idle `rssi4815` 46–49 with radio A keyed -- the self-test's 47 exactly.  During
the port's transmission the lane **flickered**: RSSI swung 12–49 (mean 31),
i.e. the carrier was seen at full level at moments and dropped to the noise
floor at others -- between layout A's collapse (31 → 7–16) and the self-test's
held `47 → 47`.  The ear answer was again **2 (beep only)**.  The self-test
(`E`) held `min=47` in this same band layout, and the one functional difference
is the transmit source: `E` keys a **tone** (`TX_SOURCE_TONE`), `T` keys the
**microphone** (`TX_SOURCE_MIC`, mic ADC + TX DSP + speech) -- so the
candidates are the mic/DSP chain itself, speech-peak PA current (supply sag
desensing the receiver in bursts), or the `T`-specific AF state (`0x47=0x6142`
+ AF DAC on).  Separating runs, all on the new build: (a) `E` again in layout B
(expect min 47); (b) `T` with radio A keyed continuously and the operator
**silent** -- a held lane points at speech-peak current, a flickering one at
the mic chain; (c) `T` with `H` off (no beep) to answer mix/level directly.
Post-run `[dup]` telemetry again healthy (`0x75=0x000a`, tuning word `893b
91b9` = 446.00625, squelch marks 10/48–51).

**Ear test, third run (2026-10-18, console `E`, layout B, radio A keyed).**
All four variants held the lane (`idle=47`, `tx min=47..48`, `RX SURVIVED TX`),
**and the operator heard the downlink perfectly under the transmission** --
"the TX is just a small beep" (the tone source's own DTMF beep).  That is the
first true duplex-audio pass: the summed-node route (route 1) carries the
downlink to the speaker while the BK4829 transmits, including the variant that
writes the BK4815's T/R (`af=0 path=0 tr=1`).  Combined with the second run,
the remaining defect is now exactly the **transmit source**: a **tone** TX
leaves the lane held and the downlink audible; a **microphone** TX
(`TX_SOURCE_MIC`: mic ADC + TX DSP + speech) makes the lane flicker (min 12 /
max 49) and the downlink inaudible.  Candidates, in order: speech-peak PA
current desensing the receiver in bursts (supply sag), the mic/DSP chain's own
noise, or the mic-entry register state.  Separating run: `T` with radio A keyed
continuously and the operator **silent** -- a held lane indicts speech-peak
current, a flickering one the mic chain's configuration (then `H` off answers
mix/level directly).

**Ear test, fourth run (2026-10-18, console `T`, layout B, mic source, beep
state, demod set applied by the test).**  The lane **held** through the
transmission (`idle 47`, `tx min 46 max 49`) where the third run's unset-demod
state flickered to 12 -- the mic-source TX does **not** desense the receive
lane; the earlier flicker was an artifact of the demodulator never having been
set (its unset state reads erratic RSSI too).  The ear answer was again **2**
(beep only), so with the lane held and every gate written open (`0x47=0x6142`,
`0x49` open, PC13 as RX left it) the remaining question is exactly the
mix/level one: the loud beep vs a modest carrier.  The test used to force the
beep state, which made `H` untestable through it; it now keys with whatever
`H`/`I` last set.  Decisive run: `H` then `T` -- the downlink audible without
the beep means masking; still silent with the lane held means the downlink
audio does not reach the summed node during a TX (the 0x30 dilemma: `0xC1FE`
may leave the node unpowered -- the last unknown).

**App-mode receive regression (2026-10-18, `'J'` captures, second pass).**
After a normal GUI boot (RX VFO on the BK4815), the first reception is correct
and a later one is noise.  Read against the write log, the captures revise the
first interpretation:

- The noisy-state capture (uptime 4246 ms, **84 writes total**) has **no
  `enter_rx` burst in its history** -- the squelch had never opened yet -- so
  its demod-set reading (`0x16 = 0x3200`, `0x2B = 0x0000`, `0x73 = 0x8001`) is
  the part's **power-on default state, not corruption** (the coincidence: the
  swap-TX set writes the same `0x16` word, but no port code writes the
  `0x2B`/`0x73` values, and the log shows no write happened).
- The later capture (uptime 7045 ms, 357 writes) reads the set **correct**:
  a reception had opened the squelch and the poller's hook applied the
  verbatim set -- **and that reception was still noise**.  A statically
  correct set therefore does not imply a working demodulator; the first
  reception works with the *same* set, so the difference is in the chip's
  dynamic state around the second one, not in the registers.
- The log's tail also exposed the poller writing `0x49` **unconditionally
  every 10 ms** (~97 writes/s; the stock's appliers run on transitions only).
  That spam is now change-driven (the bridged BK4829 `0x47` stays
  every-poll -- the K1 app writes that one behind our back), with the gate
  shadow invalidated after every receive-set application, because the
  restart pair can revert the part's AF gate to its unmuted boot value.
- The write log now records only writes that differ from the previous one;
  at ~100 Hz of gate spam the 24-entry ring held just 0.24 s and evicted the
  reception-time bursts it exists to catch.

Next reproduction: reflash, TX #1 (ok), TX #2 (noise), then `'J'` **while the
noise is happening** (radio A still keying, squelch open).  The log will
either catch a code path writing the TX-shaped set mid-reception, or show the
set applied-and-untouched with `0x44` reading the carrier -- the "configured
but ineffective" state, whose next lever is the nuclear re-assert (full boot
table + retune + set) once per squelch opening.

**Second-pass captures (same day): the hook is verified working.**  The write
log now reads as intended: an `enter_rx` burst at every squelch opening
(uptime 15487, 21181, 23041 ms), gate open → +15 ms shut (the settle
window) → +160 ms reopen; 51 writes over 15.6 s (the spam fix confirmed
live).  The noisy reception's own capture reads `squelch OPEN, RSSI 58`,
the demod set exactly `B200/403F/6618/81D0`, `0x49` unmuted, the SNR
indicator `0x43` fluctuating wildly between captures (5 ↔ 73) -- **the
"configured but ineffective" state: the chip sees the carrier, the set is
correct, and the audio is still noise.**  The log also caught a real bug
(fixed): the gate opened in the same pass that armed the settle window --
the pre-arming `settling` read 0 there, so each opening amplified the
transient once before the settle kicked in; the gates now recompute the
window after the set.

Still unanswered: **whose** noise -- the bridged BK4829's own demod summed
into the node, or the BK4815's.  The one-keystroke discriminator at the noise
is `'z'` (bridge off, the 4829 hard-muted every poll, the 4815's own gate
stays open): noise stops → the 4829's contribution; noise continues → the
BK4815's demodulator itself.  If the latter, the nuclear re-assert variant
goes in next.

**Radio answer (2026-10-18): `'z'` -> silence, `'y'` -> clean audio.**  With
the bridge off the burst disappears *and the BK4815's audio disappears with
it* -- the BK4815's downlink only exists on the path through the BK4829's AF
stage.  Rebinding the receive VFO to the BK4829 (`'y'`, swap) receives 446
directly and sounds clean (RSSI 222).  So the RF is fine on both chips; what
fails on the BK4815 lane is the **audio level on the shared path** -- the
4815's output ends up drowned by the BK4829's idle noise, which is what the
ear reports as "noise".  The port's own code agrees: the stock's AF gate word
`0x49 = 0x1A02` runs the BK4815's output volume at **2 of 15**, and every
diagnostic in this port deliberately opens it at **`0x1A0F`** -- *"at maximum
volume, so a quiet-but-working downlink cannot be mistaken for noise"*.  The
normal receive path was using the quiet value.  `BK4815_AF_ON` is now
`0x1A0F` (the diagnostics' proven audible value); wiring the gate to the app's
volume setting is the proper follow-up.  Confirming test: with the new build,
the first and later receptions should sound identical; if a later reception is
still noise, the level theory is dead and the nuclear re-assert goes in.

**The 4815 transmit set is dead code** — already recorded in `docs/ra89r_bk4815.md`
("The stock never transmits through it") and `docs/ra89r_rfpath.md`; the
swap-mode premise above predated that correction.  `FUN_080173F2(lane, mode)`
dispatches
`mode == 0` to the BK4815 transmit set (`FUN_080172BC`: `0x12 = 0x415C`/`0x0148`,
`0x0C = 0xF823`, `0x16 = 0x3200`, `0x2B = 0x803F`, `0x6D = 0xE718`, `0x73 =
0x8000`, `0x11 = 0x5800`, `0x14` from a table) and any other mode to the BK4829
TX-audio entry (`FUN_0801736C`).  Its only caller, the lane activate
`FUN_08018BA4`, passes **1** (`0x08018BDA`; V49 `0x08018AEE` is the same call),
and a linear sweep of every `bl` in both images finds no other call site, so the
mode-0 branch is never taken.  The repeat entry (`FUN_08016B18` mode 9 →
`FUN_08018BA4` → `FUN_080173F2(lane, 1)` → `FUN_0801736C`) configures the
**BK4829** for transmit (`0x47 = 0x6040`, `0x30 = 0xC1FE`, `0x50 = 0x3B20`, mic
gain through `0x40`) and the shared PA (`FUN_0801BED4`: bias, PA1, band path, and
— only outside a repeat — the BK4815 band write, `if (0x20000302 == 0)
FUN_08013864(3)`).  The 4815 set proves the silicon can do it, not that the stock
does; the port's "the 4815 radiates nothing" measurement is consistent with the
firmware never using that path.

Still worth watching on the probe: the alternator makes each lane active in
turn, and the mute applier runs on the *active* lane only, so `0x47` on the
BK4829 moves with the alternation.  If it stays normal while the BK4815 lane
receives, the stock keeps its own audio path alive and `I = 1` is the right port
setting; if it goes to `0x6040` on the transmit lane, the stock's own speaker is
silent during its relay (a relay for the other radio only).

Session tooling, for whoever repeats this: `work/ghidra/` holds a copy of the
`ra89r_re` Ghidra project (the original under `~/` is read-only in this
workspace), the headless runs need
`JAVA_TOOL_OPTIONS=-Duser.home=$PWD/home`, and the helper scripts used above are
`work/ghidra/scripts/{Decomp,Callers2}.java`.

### The GUI-load burst and the duplex RX signature (2026-10-18, revised same day)

- **The burst when the GUI loads.**  At boot, `rf_dual_refresh()` tunes the
  BK4815's VFO right before the GUI comes up, and (on `edcbc9a`) the tune ran
  `bk4815_enter_rx()`; its writes spike the part's RSSI, the next 10 ms poll
  crossed the `0x1E` open mark, latched the squelch open and amplified the
  demodulator's restart transient -- the burst heard exactly when the GUI
  loads.  The same hook re-ran the set on every squelch opening and every
  duplex PTT release.
- **First fix, wrong (burst removed, audio broken).**  Dropping the stock's
  trailing `0x70 = 0` / `0x70 = 0xA000` pair from the set silenced the burst --
  and silently broke the set: with the pair left out, the four demodulator
  writes alone do **not** bring the demodulator up (radio 2026-10-18: `E`'s
  downlink audio vanished on that build while `rssi4815` stayed 47; with the
  pair, the same test was heard perfectly).  Register `0x70` is the operation
  control (`0xA000` receive / `0xE000` transmit) and the restart cycle is what
  makes the reconfig take; the stock ends `FUN_08016CEC` with it for a reason.
- **Second fix (current).**  The set is back verbatim, and the burst is
  prevented where it actually happened: the **settle window**.  Applying the
  set latches the squelch open but holds the three audio gates (4815 `0x49`,
  4829 `0x47`, PC13) shut for 150 ms, then the squelch judges the settled
  RSSI -- the transient is never amplified, and a real carrier opens ~150 ms
  late.  `bk4815_set_frequency()` still does not call the set (the boot tune
  must not burst into the first squelch opening); the squelch-open hook in
  `rx_poll()` applies it once, settled, and the TX teardown paths arm the same
  window (`rx_4815_set_applied()`).  The console paths that tune the 4815
  while the poller is blocked (`U`/`E`/`T`) apply it explicitly.

  *Located, not verified*: the burst mechanism and the pair's necessity fit
  the register semantics and the radio runs above, but the current build has
  not yet been heard on the radio.

### Root cause (2026-10-18): the K1 clears the BK4829's chip outputs behind the port's back

The "first reception fine, later ones noise" -- and the later "it happens when
we switch VFOs, and a frequency edit and edit-back fixes it, **also on
develop**" -- is a **develop bug**, not a chip or duplex fault:

- The BK4815's receive path rides the **BK4829's chip-output pins**: the
  stock's BK4815 branch sets the 4829's pin 4 (`FUN_080137D4(0x10,0x10)`, the
  VHF-LNA bit), and the port's `rf_dual_refresh()` mirrors that
  (`GPIO4_PIN32_VHF_LNA`), plus `RX_ENABLE` (`GPIO0`).
- The K1 app **clears those same outputs** in its own paths --
  `FUNCTION_PowerSave()` (`functions.c`), the RX-off helper (`radio.c`) and the
  sleep path (`app.c`) clear `RX_ENABLE`; the RF-setup path clears the pin-4
  bit -- and those run on **VFO switches, wake-ups and idle**.
- `rf_dual_refresh()` re-drives the pins **only when the roles change** (a
  different VFO-to-transceiver binding or a new frequency), which a plain VFO
  switch does not.  So switching VFOs leaves the BK4815 receiving with the
  path pins down: **RSSI still reads the carrier, but the audio is noise** --
  exactly the symptom chased all day.  Editing a VFO's frequency forces
  `rf_dual_apply()`, which re-drives the pins, "fixing" it.
- Evidence in the captures: the noisy state read `0x33 = 0x9004`
  (`RX_ENABLE` clear, pin 4 set), a good one `0x9044` (both set).  Note
  `0x40 >> Pin`: `GPIO0_PIN28_RX_ENABLE = 0` is the `0x40` bit,
  `GPIO4_PIN32_VHF_LNA = 4` the `0x04` bit.
- The earlier "corrupt codeplug VFO" reading was wrong: rewriting the VFO was
  the *trigger* for the re-apply that fixed it, not the cause.

**Fix (this branch):** `rx_poll()`'s BK4815 branch now re-asserts the two pins
on change (`BK4819_IsGpioOutSet()` first, so no extra bus write) and keeps the
part's band register `0x75` on the tune it is actually on -- `pa_select_band()`
writes that register from whatever frequency it is handed, and the scan probes
and the TX path hand it the BK4829's.  `rf_dual_apply()` also records the
frequency it tuned the BK4815 to (`rf_dual_bk4815_frequency()`), which is what
the band guard uses.  *Located, not verified*: the pin evidence is from the
register captures, but the fix has not yet been confirmed on the radio by a
VFO-switch test.

### Still open (revised 2026-10-17)

- **The receive-during-transmit RF side is now confirmed on the radio**
  (2026-10-17 self-test, layout B: RX 446.00625 / TX 145.5): `idle=47`,
  `tx min=47` in all four variants, including the one that writes the BK4815's
  T/R (`0x0C=0x0203`).  The remaining RF checks are external: the far receiver
  must hear the tone/voice on the transmit frequency, and layout A (RX 145.5 /
  TX 446) desenses the receive lane (floor 17 → 41), so the satellite
  configuration is downlink 446 / uplink 145.5.  The stock's own relay keying
  (mode-9 loop vs `FUN_08004c84` → `FUN_080161d8` on the state bytes
  `obj+0x28/0x29/0x7a/0xa0/0xa3`) is still unneeded for the port's
  PTT-driven duplex, but remains unexplained.
- Board-level audio: **route 1 is radio-verified** (2026-10-18, console `o`: the
  mic amplitude follows the operator's voice 9 → 138 but not the downlink
  carrier 6–8), in the state a satellite uplink uses (PC13 as RX, PA off).  What
  remains: (a) the ear test — with radio A transmitting, compare `H` off
  (`0xC1FE`) and `H` on (`0xC3FE`) for whether the downlink is audible during a
  real duplex transmission and how loud the TX audio is over it; (b) the air
  check — key the port silent while radio A transmits, a far receiver on the
  uplink must hear nothing (route 1 predicts it); (c) the stock's relay: its TX
  entry writes `0x47 = 0x6040`, and with route 1 the stock's relay audio route
  is unexplained — watch `0x47` across a BIDIR relay and listen to the stock's
  speaker; the audio-path line **PC13** (console `g`) is the remaining stock-side
  candidate.
- Duplex audio is **verified with a tone transmit** (2026-10-18, layout B:
  lane held `47 → 48`, downlink heard under the TX, all four variants).  The
  open defect is the **microphone** transmit: its TX makes the lane flicker
  (min 12 / max 49) and the downlink inaudible (see the ear-test runs above).
  The separating run is `T` with radio A keyed and the operator silent; `H`/
  `O`/`g` are the knobs, and console `7` classifies a post-TX RX state if one
  still misbehaves.
- The exact UNIDIR/BIDIR lane restriction over the page-context check in the
  arming routine (V52 `FUN_080190CC`, `0x20000310` against the lane object) —
  runtime.  V52 also disables the repeat when either lane's `[0x75]` flag is
  set, which the per-VFO config uses for the `0x6740` AF/demod variant.
- The unnamed settings byte `settings+0x31` and the flash-table playback
  branch in `FUN_08004aa4` (voice prompts?, the `0x40000` table region — check
  against `stock_eeprom.bin`/`after.bin`).
- Meaning of the state bytes `obj+0x28/0x29/0x7a/0x7d/0xa0/0xa3` in the
  per-VFO object (`0x20009C14`/`0x20009D98`) — the port will need them named
  for the per-lane model.
