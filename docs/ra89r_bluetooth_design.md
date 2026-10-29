# RA89R Bluetooth earpiece — design

- **Date:** 2026-10-04
- **Branch:** `driver/bluetooth`
- **Status:** design approved; implementation not started
- **Feature doc:** `docs/ra89r_bluetooth.md` (protocol/evidence, already validated)

## Goal

Give the ported RA89R firmware the stock's Bluetooth earpiece feature: turn the
Jieli **YBT100** module on, pair/connect a headset, route the radio's audio to and
from it, and use the earpiece's PTT. The feature lives on its **own screen**,
reached with **F + MENU**, so the K1 main `MenuList[]` stays unchanged.

## Success criteria

1. F + MENU opens a dedicated BT screen; the main menu is untouched.
2. BT Switch turns the module on/off (PD0 reset + `AT+BT_DISCN`); the module's
   `+IM_*` replies are parsed and drive the UI.
3. The nine stock BT items exist and act: BT Switch, Pairing, Paired Dev, Hold
   Time, Scan, Spk Volume, Mic Gain, Blooth Inf, PTT Type.
4. A headset pairs/connects; the earpiece PTT keys the transmitter through the
   already-validated TX chain.
5. BT settings persist across a reboot.
6. *(Audio)* radio RX audio is heard in the earpiece and earpiece mic audio is
   transmitted — **blocked on tracing the module's analog audio path**.

## Background

The AT link is validated on the radio (`docs/ra89r_bluetooth.md`): a Jieli
YBT100 at 115200 8N1 on USART3 (PB10/PB11), reset on PD0, replying with
`+IM_VERSION:YBT100_FW_V01_02_012` and a boot banner
(`+IM_BLE_SLAVE`, `+IM_BPHONE_DISCN`, `+IM_BT_EMITTER`, `+IM_BT_DISCN`,
`+IM_READY`, `+IM_BT_SCAN_STOP`).

The stock's control flow, from the image:

- **`FUN_08009660(value)`** — the on/off setter. Writes the codeplug BT bool,
  mirrors it to external flash, queues `AT+BT_DISCN` and drives PD0 low on off,
  or runs `FUN_0801D69C` (PD0 low→high + load paired devices) on on.
- **`FUN_0801D69C`** — module bring-up: PD0 low, read settings, clear the
  command/ready state, load the eight paired-device records (`FUN_080106A4`),
  PD0 high.
- **`FUN_08007FD0`** — runs on `+IM_READY`: queue `AT+GMR?`, set the mode
  (`AT+BT=EMITTER`/`AT+BT=RECEIVER`), optionally `AT+WRITE_NAME`, then
  `AT+BLE_LOCAL?`, `AT+BT_SCANATCN=ON`, `AT+CONN_STATE?`, `AT+BT_CONN_LAST`.
- **`FUN_0800C588`** — the BT-menu state machine (9 cases) over the descriptor
  at `0x080256AC` (title `BT Menu`).
- **`FUN_080066CC`** — periodic reset retry while BT is on and not ready.
- **Earpiece PTT** — the parser sets `state+6` on `+IM_EAR_PTT_KEYDOWN` and
  clears it on `+IM_EAR_PTT_KEYUP`.

The port already has: the `BK4819_*`-style driver split, the FM precedent
(`App/app/fm.c` state machine + `App/ui/fmradio.c` render + `DISPLAY_FM`), the
K1 menu/settings idioms, and the measured TX chain in `App/driver/tx.c`
(`tx_poll_ptt()`).

## Architecture

New files, mirroring the FM split:

| file | responsibility |
|---|---|
| `App/app/bt.c` / `bt.h` | the BT application state machine, command queue, event handling, settings binding |
| `App/ui/bt.c` / `bt.h` | the `DISPLAY_BT` screen render and key handling |
| `App/driver/bluetooth.c` / `.h` | unchanged transport/parser; add a `bluetooth_poll()` caller contract only |

Changed files:

| file | change |
|---|---|
| `App/ui/ui.h` | add `DISPLAY_BT` |
| `App/app/main.c` | F+MENU opens `DISPLAY_BT`; `bt_poll()` in the 10 ms slice; boot init |
| `App/app/app.c` | `ProcessKeysFunctions[DISPLAY_BT]` entry; save flag flush |
| `App/settings.h` / `settings.c` | new BT fields + defaults |
| `App/driver/py25q16.c` / `.h` | paired-device region; `BLOB_VERSION` bump |
| `App/driver/tx.c` | OR `bt_ptt_down()` into `tx_poll_ptt()` |
| `App/main.c` | console commands for BT testing |

## The BT service (`App/app/bt.c`)

A state machine, driven by `bt_poll()` (called from the 10 ms slice) and by the
event callback registered with `bluetooth_set_event_cb()`:

```
BT_OFF ──(user enables)──▶ BT_RESET   PD0 low→high, queue AT+GMR?
BT_RESET ──(+IM_READY)──▶ BT_CONFIG   stock FUN_08007FD0 sequence
BT_CONFIG ──(queue drained)──▶ BT_IDLE
BT_IDLE ──(scan/connect)──▶ BT_SCAN / BT_CONNECT
BT_SCAN/CONNECT ──(+IM_SCO_CONN|+IM_BT_EAR_CONN)──▶ BT_CONNECTED
any ──(user disables)──▶ BT_OFF   AT+BT_DISCN, PD0 low
```

- Boot: `bluetooth_init()` + `bluetooth_set_event_cb()` once, near the existing
  bring-up. Poll `bluetooth_poll()` in the 10 ms slice (`App/main.c`), which only
  touches USART3 (not the external NOR, so the 10 ms rule is respected).
- The command queue mirrors the stock's `state+0x4a`/`+0x54`/`+0x55`: an array of
  command ids plus the current index; `+OK`/`+ERROR` advance it per the stock's
  rules (already transcribed in `docs/ra89r_bluetooth.md`).
- The reset retry (`FUN_080066CC`): while enabled and not ready, re-pulse PD0 on
  a countdown.
- Pure logic (state transitions, queue advance, event → action) is host-testable;
  the transport is the existing `bluetooth.c` under `BLUETOOTH_HOST_TEST`.

## Settings and storage

New `EEPROM_Config_t` fields (all in `gEeprom`, so the existing blob round-trips
them):

```
bool     BT_Switch;      /* on/off (the stock config[0x38]) */
uint8_t  BT_Mode;        /* emitter / receiver (the stock config[0x37]) */
char     BT_Name[16];
uint8_t  BT_SpkGain;
uint8_t  BT_MicGain;
uint8_t  BT_PTTType;
uint8_t  BT_HoldTime;
```

Defaults in `SettingsDefaults()`. Bump `BLOB_VERSION` in
`App/driver/py25q16.c` (an old blob is rejected and defaults reload once).

**Paired devices** are `8 × (16-byte name + 64-byte record) = 640 B`, which does
not fit `STORAGE_EXTRA_MAX` (240 B). They get their own region in the port's NOR
area (the stock codeplug is never written). A small accessor API
(`bt_paired_load`/`bt_paired_save`/`bt_paired_clear`) keeps the storage detail in
one place and host-testable against the NOR test double.

## The BT screen (`App/ui/bt.c`)

- `DISPLAY_BT` renders a scrollable list of the nine items, using the port's font
  helpers (as `ui/fmradio.c` does). Sub-lists (Pairing, Paired Dev) are in-place
  modes of the same screen.
- Opened by F + MENU: add a `gWasFKeyPressed` branch in `MAIN_Key_MENU`
  (`App/app/main.c`) that sets `gRequestDisplayScreen = DISPLAY_BT` instead of
  `DISPLAY_MENU`. EXIT leaves the screen.
- Key handling: `bt_screen_process_keys()` registered in
  `ProcessKeysFunctions[DISPLAY_BT]` (`App/app/app.c`). UP/DOWN move the cursor,
  MENU/select enters a sub-list or toggles, EXIT backs out.
- A small **BT** indicator on the status line (`App/ui/status.c`) when the module
  is on/connected.

The nine items and their actions:

| item | action |
|---|---|
| BT Switch | toggle `BT_Switch`; enable/disable the service |
| Pairing | sub-list: start scan (`AT+BT_SCAN=ON`), connect last (`AT+BT_CONN_LAST`), pair-clear (`AT+BT_PAIRCLR`) |
| Paired Dev | list of the 8 stored records; select to connect (`AT+EAR_CONN=<addr>`) or delete |
| Hold Time | `BT_HoldTime` value |
| Scan | scan on/off (`AT+BT_SCAN=ON/OFF`) |
| Spk Volume | `BT_SpkGain` (`AT+SPKGAIN=<v>`) |
| Mic Gain | `BT_MicGain` (`AT+MICGAIN=<v>`) |
| Blooth Inf | info: version (`+IM_VERSION`), local address (`AT+BT_LOCAL?`), conn state (`AT+CONN_STATE?`) |
| PTT Type | `BT_PTTType` (earpiece PTT behaviour) |

`Blooth Inf` and `Hold Time` semantics still need a short decode pass against
`FUN_08019B7C`/`FUN_08019B00`; the item exists either way.

## Earpiece PTT

`+IM_EAR_PTT_KEYDOWN`/`UP` set/clear a `bt_ptt_down` flag. `tx_poll_ptt()`
(`App/driver/tx.c`) ORs it into its "down" test, so an earpiece PTT keys the
transmitter through the measured `tx_start()`/`tx_stop()` chain — the same path
already validated on the radio. This avoids the K1 PTT state machine, whose
`GPIO_IsPttPressed()` is deliberately false in this port.

## Audio (open)

Where the YBT100's analog mic/speaker land on this board is not in the firmware
(the stock only sets the module's internal `SPKGAIN`/`MICGAIN`). This must be
traced (board/schematic or a scope on the module's analog pins) before the audio
phase can be implemented. It is the only risk to the full earpiece outcome.

## Phasing

Each phase is independently flashable and validated on the radio.

1. **Control plane** — service, settings, `DISPLAY_BT`, BT Switch + mode/scan/
   gains/PTT-type items, status indicator, console commands. No paired list yet.
2. **Pairing + paired list** — Pairing/Paired Dev sub-lists, the 640-byte NOR
   region, connect/delete.
3. **Earpiece PTT** — wire the events into `tx_poll_ptt()`.
4. **Audio** — trace the analog path, then route RX audio to the module and its
   mic to TX.

## Testing

- **Host:** the service state machine and command queue (exact command sequence
  per state/event), the settings round-trip, and the paired-device accessors
  against the NOR test double. Extends the existing `test_bluetooth.c` pattern.
- **On-radio:** the BT screen opens with F+MENU; BT Switch powers the module;
  the module's replies drive the UI; a headset pairs/connects; earpiece PTT keys
  TX; (phase 4) audio is heard both ways.

## Risks / open

1. **Audio path untraced** — blocks success criterion 6.
2. **`Blooth Inf` / `Hold Time`** semantics need a short decode pass.
3. **The K1 menu idiom** is flat; the BT screen is a new screen rather than a
   `MenuList[]` entry, so it needs its own render/key path (kept separate from the
   K1 menu code to avoid regressions).
4. **Blob layout** — the paired-device region must not collide with the existing
   settings/extra areas or the stock codeplug.
