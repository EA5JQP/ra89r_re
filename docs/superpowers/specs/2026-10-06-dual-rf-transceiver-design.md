# RA89R dual-RF transceiver design

**Status:** Proposed design; awaiting user review. No implementation is authorized
by this document yet.

## Goal

Let each VFO choose its RF transceiver, and let both transceivers run at the same
time, each configured for its own VFO, so the radio can do satellite-style
full-duplex and cross-band repeat ("hear yourself on the repeater").

Success criteria:

1. A main-menu item selects the transceiver for VFO A and for VFO B.
2. Both transceivers can be active at once, each holding its VFO's frequency,
   band, modulation and T/R state independently.
3. With one VFO receiving and the other transmitting, the radio works
   full-duplex: audio received on one band is audible while the other band
   transmits, and on a second receiver the transmitted audio is the relayed or
   microphone signal as selected.

## Confirmed evidence (stock V49)

- The stock ships a **cross-band repeater**: the `Aut Repeat` setting with
  `OFF` / `UNIDIR` / `BIDIR` (`0x08027154`; options at `0x08024ed0`). A repeater
  needs RX and TX at once, so concurrent operation is a real hardware capability.
- The stock **configures both transceivers**. `FUN_08015eb4(channel, sel)`
  dispatches to `FUN_08015d88` (BK4815) or `FUN_08015e28` (BK4829); the selector
  is a call parameter, and the per-channel path
  `FUN_08016ee0(channel, flag)` picks the same pair. The chip is a runtime
  choice in the stock, not a build-time one.
- The shared bus already takes the chip select per call
  (`rf_bus_write/read(cs, ...)`, `rf_bus_assert/release(cs)`), so the transport
  is not the blocker.
- The port's `BK4819_*` layer is bound to the BK4829 (`bk4819.c` `#define CS
  BK4829_CS_PIN`) and holds single-instance state (`gBK4819_GpioOutState`,
  `reg_30_cache`, `reg_47_cache`, the audio-path callback, the local gain
  settings). `bk4829.c` / `bk4815.c` likewise hard-code their selects.
- `VFO_Info_t` has no transceiver field; `pa.c` / `tx.c` / `rx.c` each hold one
  band/T-R/squelch/frequency state and read the current K1 VFO at call time.
- The BK4815's **receive is unproven**: its frequency tuning is a computed
  6-byte block at registers `0x70..0x75` (`FUN_0801703c`, floating-point math)
  that the port has never implemented, its per-mode RX config is `FUN_08016cec`,
  and the stock never meters it (all RSSI reads are BK4829 `0x63/0x67/0x65`).

## Open questions this design must resolve

These are investigation tasks, not assumptions; the design leaves room for any
answer and each has an explicit gate.

1. **RX/TX chip roles.** In the stock's `Aut Repeat`, which transceiver receives
   and which transmits, and is that fixed or frequency-derived? (Trace
   `Aut Repeat` and the `FUN_08015eb4` callers `FUN_0800c478`, `FUN_08015ecc`.)
2. **Audio routing between the two chips.** How does the stock move the received
   audio onto the transmit side? (Same trace; the repeater's audio path.)
3. **Where `Aut Repeat` is stored.** It is not in the CPS's 32-byte block, so it
   appears firmware-only. The port does not have to copy the stock's storage.
4. **BK4815 transmit.** The port's measured TX chain is BK4829-only. Until the
   BK4815 is shown to transmit, the TX side of any full-duplex arrangement must
   be the BK4829.

## Design

### Per-VFO transceiver selection

- Add a per-VFO transceiver value to the port-side settings. The user-facing
  choices are explicit: `RF_XCVR_BK4829` or `RF_XCVR_BK4815`. The old stored
  value `0` (formerly `AUTO`) and invalid values normalize to BK4829, preserving
  existing settings blobs without exposing a redundant menu choice.
- The field is the port's own setting, stored in the port's versioned settings
  blob, **not** in the read-only stock codeplug. It is a per-VFO value, so it
  survives reboot.
- Two main-menu items, `TrVfoA` and `TrVfoB`, are added to `MenuList[]` next to
  the other per-VFO items. Each names its VFO explicitly and writes that VFO's
  transceiver, independent of which VFO is selected on the main screen, so the
  operator does not have to switch VFOs to configure them. BK4829 is the
  default.

### Transceiver service layer

The blocker is that the K1 application speaks one API (`BK4819_*`) and assumes
one chip. Two options:

- **Option A (recommended) -- role-based, minimal refactor.** Keep the
  `BK4819_*`/BK4829 layer as the *primary* transceiver that the K1 application
  drives (RX and TX). Add a second, narrow BK4815 **receive** service
  (`driver/bk4815_rx.c`) that tunes and meters the BK4815 for the second VFO.
  A coordinator (`driver/rf_dual.c`) owns which VFO is primary and which is
  secondary, configures both, and sequences T/R. This matches the hardware
  evidence (BK4829 is the main/modem part; the BK4815's role above 134 MHz is
  the stock's second path) and is far less code than a dual-instance refactor.
- **Option B -- full per-VFO instances.** Refactor `BK4819_*` into an instance
  API so either chip can be any VFO's transceiver, including TX. Much larger,
  and blocked on proving BK4815 TX, which is currently unproven.

The design proceeds with Option A. Under it, "select the transceiver for each
VFO" means selecting which VFO is served by the primary (BK4829) and which by
the secondary (BK4815); a VFO set to `BK4815` is receive-capable but cannot be
the transmit VFO until BK4815 TX is proven.

**Transmit is cooperative, above 134 MHz.** The stock never transmits on the
BK4815 alone: on TX it runs the BK4829's modulator/tune (`FUN_08017280`) and PA
control (the BK4829 `0x33` band pin + the `PB14` bias PWM), and above 134 MHz it
also puts the BK4815 into its transmit state (`0x0C = 0x0203` via
`FUN_08016228`). So the BK4829 is the modulator and steers the PA; the BK4815 is
the RF/T-R path above 134 MHz. The port mirrors this: `tx_start` drives the
BK4829 and, above 134 MHz, sets the BK4815 to TX mode at the TX frequency
(`bk4815_set_frequency(freq, true)`) so it is not left on a receive frequency;
`tx_stop` asks the coordinator to put both back on their receive frequencies.

### Receive audio source

The BK4815 has its own AF path, register `0x49` (datasheet REG73: bit 15 is
"hard mute"; the boot config leaves `0x1A02`, unmuted). The stock switches the
audio source by the transceiver flag: `FUN_08015F48` mutes the current source
(`0x9A02` on the BK4815, or the BK4829's `0x47`) and `FUN_0801CCE8` restores it
(`0x1A02`). The port mirrors this: the receive audio follows the **selected RX
VFO's** transceiver — that chip's AF is opened by its own squelch
(`bk4815_set_af(true)` = `0x1A02`), the other stays muted. Squelch for the
BK4815 uses its `0x44` RSSI (measured: idle ~16, carrier ~41 → open `0x1C`,
close `0x14`); the stock squelches on the BK4829 even for the BK4815, so these
marks are the port's own.

### Dual-active operation

- `rf_dual` configures the primary for the primary VFO (frequency, band,
  modulation, squelch) and the secondary for the secondary VFO, using the
  BK4815's own tuning/config path (ported from `FUN_0801703c` / `FUN_08016cec`).
- Both receive simultaneously; each has its own squelch state. The two audio
  streams are mixed or selected by a user setting (which VFO is heard), since
  the MCU has one speaker path.
- Transmit is always on the primary (BK4829) for now.

### Full-duplex / repeater

- A repeater setting mirrors the stock's `Aut Repeat` semantics: `OFF`,
  `UNIDIR`, `BIDIR`. It is the port's own setting (the stock's storage is not
  copied).
- `UNIDIR`: receive on one VFO, relay that audio to transmit on the other.
  `BIDIR`: relay in both directions as the two VFOs' roles swap.
- The relay needs the received audio on the primary to reach the transmit path.
  The stock's routing is open question 2; until it is traced, the design assumes
  the MCU can route the secondary's demodulated audio into the primary's TX
  audio path (the same AF/mic path the beeper and DTMF already use). If the
  trace shows a hardware-only path the MCU cannot drive, the repeater is
  descoped and only dual-receive remains.

## Phasing

Each phase is independently testable and stays on `driver/dual-rf` until
validated on the radio.

1. **Per-VFO selection + BK4815 receive.** Port the BK4815 tune/RX config, add
   the per-VFO field, the menu item, and the `rf_dual` coordinator for
   dual-receive. Gate: the BK4815 receives a signal on a second VFO,
   independently of the BK4829.
2. **Full-duplex T/R.** One VFO receiving while the other transmits, with the
   audio relay. Gate: a second receiver hears the relayed signal while the
   radio still receives.
3. **Repeater modes.** `Aut Repeat` `OFF`/`UNIDIR`/`BIDIR`. Gate: the radio
   repeats a signal between bands, both directions for `BIDIR`.

If phase 1 shows the BK4815 cannot receive independently, the feature stops and
the finding is documented; per-VFO selection then has no second chip to select.

## Verification

- Host: the per-VFO field and menu (preview), the `rf_dual` role/route decisions
  and the repeater state machine (a focused host test, no hardware), and the
  BK4815 tune math if it can be expressed without floating point.
- Radio (each phase's gate above), on a second receiver where audio is involved.
- No phase merges to `develop` until its gate passes.

## Explicit non-goals

- Do not guess the BK4815's TX path or force it as a transmit transceiver.
- Do not copy the stock's `Aut Repeat` storage format; the port uses its own.
- Do not change the stock codeplug; the per-VFO setting lives in the port's blob.
