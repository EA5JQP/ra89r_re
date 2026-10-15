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
is impossible as the port is written.  The **board** can do it — the stock's
`Aut Repeat` cross-band repeater does — so the work is to give each transceiver
its own band/path state (and its own receive chain) the way the stock's repeater
routine does.  Until then the port offers **cross-band half-duplex**: RX on one
VFO's band, and on PTT it switches the front end and transmits on the other.

## The stock's cross-band repeater (`Aut Repeat`)

The stock firmware implements the repeater the product page advertises.  The
string is `Aut Repeat` (0x08027154); it sits among the Bluetooth-app strings in
the image, but the **manual** lists it as the repeater option with settings that
include **Bidir** (repeat both directions).  So the radio genuinely does
cross-band repeat, which means its hardware can present both bands at once and
the port's single-domain band model is the obstacle, not the board.

To port it, the next step is to find the routine behind `Aut Repeat` in the
disassembly and see what it does that the port does not: which transceiver
receives and which transmits, how it drives the band/path pins
(`pa_band_path()` / `pa_apply_chip_path()`), and whether the receive chain stays
live during the repeat (simultaneous) or the audio is stored and replayed
(simplex repeater).  That difference decides how much of the port's RF model has
to change.

## Why a cross-band repeater can do it and a single-path design cannot

A cross-band repeater really does receive and transmit at once.  A radio built
for it has the hardware to present both bands simultaneously — a
duplexer/diplexer (or two antennas) at the antenna, with enough isolation
between the high-power transmitter and the sensitive receiver.  This radio has
that capability (the stock repeats), but the port drives **one** band/path state
for whichever frequency is current, so with the port's model the two directions
exclude each other.  The stock's repeater routine must drive the two chips'
paths separately; that is what the port needs to learn from it.

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

- **Find the stock's `Aut Repeat` routine** in the disassembly (the manual's
  Bidir option): which transceiver receives, which transmits, how it drives
  `pa_band_path()` / `pa_apply_chip_path()` and the per-chip paths, and whether
  the receive chain stays live during the repeat (simultaneous) or the audio is
  stored and replayed (simplex repeater).
- Whether the port can adopt that per-chip path model directly, or whether some
  of it is board-specific wiring needing its own driver work.
- The probe above is evidence about **the port's model** only — the board can do
  it, so the board is no longer the question.

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
