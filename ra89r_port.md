# Porting the F4HWN (UV-K1/K5V3) application onto the RA89R

**Status: the branch is open, nothing is ported yet.**  This is the working
document for the port: what the RA89R side already provides, and the concrete
fixes and gaps that have to be closed before the K1 application can be brought
up.  `ra89r_findings.md` stays the hardware write-up; this is the port's own.

The two firmwares:

| | K1/K5V3 (source) | RA89R (target) |
|---|---|---|
| MCU | PY32F071 | PY32F403 (Cortex-M4F, 384 KB flash, 64 KB SRAM) |
| app size | `App/`: 394 `.c` files, ~119k lines | ~1k lines of bring-up + drivers |
| RF | one BK4819 on a bit-banged bus | BK4829 **and** BK4815 on one bit-banged bus |
| panel | ST7565 over SPI | ST7565-family **bit-banged** (`PB15`/`PA8`/`PA10`/`PA11`/`PA9`) |
| keys | GPIO matrix | **ADC ladder**, 20 buttons, already returning `KEY_Code_e` |
| storage | `gEeprom` in internal flash (+ `py25q16`) | external SPI NOR "EEPROM" + the MCU's flash |
| audio | chip AF/mic + a beeper pin | chip AF/mic + `PC13` (unmeasured), beeper on `PA4`/TIM4 |

## What the RA89R side already has (merged on `develop`)

Drivers, each validated on the radio where the doc says so:

| driver | state |
|---|---|
| `driver/gpio.c`, `clock.c`, `early.c`, `systick.c`, `uart.c`, `fault.c` | bring-up, console at 115200 on USART1 |
| `driver/lcd_st7565.c` + `ui.c` | the panel, bit-banged, fonts lifted from the stock |
| `driver/backlight.c` | `PA5`, confirmed |
| `driver/keypad.c` | 20 buttons, returns the K5V3 `KEY_Code_e` |
| `driver/led.c` | `PA13` red / `PA14` green, both active high, measured |
| `driver/rf_bus.c` | the shared 3-wire bus (clock `PA12`, data `PB12`, selects `PB8`/`PB13`) |
| `driver/bk4829.c`, `bk4815.c` | framing, ids (`0x4829`/`0x4816`), the stock's register tables |
| `driver/bk4819.c` | the **K1 API**, 80 entry points, bound to the BK4829 |
| `driver/rx.c` | bring-up + tune + the squelch (stock's `0xB4`/`0xCF` marks); RX audio heard on a second radio |
| `driver/tx.c` | the transmit chain, measured: voice heard on a second radio |
| `driver/pa.c` | the PA: `PB14`/TIM1_CH2 bias PWM, band pins, `0x33`/`0x36` enablers |
| `driver/audio_path.c` | the K1's audio-path hook (`PC13`) |

## The necessary fixes in this codebase, before the K1 app can run

Ordered by what blocks what.  The first group is *known wrong or missing in code
we already have*, the second is *modules the app needs that do not exist yet*,
the third is *decisions*.

### A. Fixes to existing driver code

1. **The transmit chain has to reach the application's path.**
   *Done on this branch:* `BK4819_ExitTxMute()` now sends `0x50 = 0x3B20` (it
   sent `0x3B18`, the value from the K1's `bk4829.c`; the stock's transmit path
   writes `0x3B20` in three places and the K1's `bk4819.c` agrees).
   *Still to do:* the other half -- the PA-CTL and bias in `0x36 = 0x8822`, the
   PA enable in `0x33`, the band path and the PB14 PWM -- lives in
   `driver/pa.c`/`tx.c` today, and the application will call `BK4819_*` entry
   points directly.  Either fold the PA into the driver's transmit path
   (`BK4819_TxOn_Beep` currently writes `0x36 = 0`, which is exactly the bug) or
   keep the T/R path in the ported `radio.c` and have it call `driver/tx.c`.
   `driver/tx.c` is the reference implementation of what the app's path must
   produce, register for register.
2. **TX power from the codeplug.**  `driver/tx.c` transmits at a fixed PB14
   compare of 128.  The stock takes it from the channel's power field
   (`FUN_080201CC`, "Pow AdjData", 0..252) through the clamped PWM compare; the
   port needs that value in `gEeprom`'s shape and one `pa_power()` call.
3. **Band and path handling.**  `pa_band_path()` is fixed at `PA1 = 1, PA0 = 0`
   and `driver/rx.c` parks the BK4815 in `0x0C = 0x0A03` -- both correct for
   145 MHz and nothing else.  The app tunes 18-620 MHz, so the port needs the
   stock's per-band path table (`FUN_08013A70`/`FUN_08013B12`, the chip GPIO
   outputs in `0x33`, the `PA0`/`PA1` pairs) and the BK4815's band register
   (`FUN_08013790`, `0x75`).
4. **The squelch.**  Ours is two fixed marks on `0x67`.  The stock ramps an AGC
   step (`0x13`) in eight steps and keeps a 0..10 counter in the channel state
   (`FUN_080052B8` -> `FUN_0801D420` -> `FUN_0801D3F0`), which is what the app's
   `BK4819_SetupSquelch`/`BK4819_IsSquelchOpen` expect.
5. **The BK4815's RF role is still open.**  It is configured, parked and
   present on the shared path, but whether it (and not the BK4829) is what
   radiates above 134 MHz is unresolved.  The app assumes one transceiver; the
   port must either settle this or keep the "bind to the BK4829" decision and
   test each band on the radio.

### B. Modules the application needs that do not exist here

The K1 tree's `App/driver/` has 22 `.c` files; the ones with no counterpart
here are the port's todo list:

| K1 driver | what it is for | what the RA89R side needs |
|---|---|---|
| `eeprom.c`, `eeprom_compat.c`, `flash.c`, `mb_flash.c`, `py25q16.c`, `spi.c`, `i2c.c` | `gEeprom` (the whole settings model), firmware slots, the codeplug | the external SPI NOR driver is on the **unmerged** `driver/eeprom` branch (read + dump validated, **write test not run**), and there is no `EEPROM_Config_t gEeprom` layer at all -- this is the largest single gap |
| `keyboard.c` | `KEYBOARD_*`, the app's key API | ours is `keypad.c` and already returns `KEY_Code_e`; needs an adapter or a rename, not new work |
| `st7565.c` | `ST7565_*` + `gFrameBuffer[7][128]`/`gStatusLine[128]` | ours is `lcd_st7565.c`, bit-banged over a different pin set; the app draws into the K1's shadow buffers, so the port needs that buffer shape (or the app's UI calls re-pointed) |
| `adc.c` | battery/S-meter sampling | **the RA89R has no battery ADC channel**: the gauge is a separate chip whose bus never answered (`ra89r_battery.md`).  Decision needed before the app's battery/saver logic can mean anything |
| `system.c` | clock/power management | ours is `clock.c`/`early.c`; the APIs differ |
| `crc.c`, `aes.c` | image validation, the codec's checks | port as-is (MCU-independent) |
| `vcp.c`, `usb/` | USB serial | the RA89R's USB-C is unused by the stock; stub or disable |
| `bk1080.c` | FM broadcast receiver | **hardware the RA89R does not have**: stub it out and hide the menu entries |
| `voice.c` | voice prompts | needs a flash voice table; the RA89R's beeper is `PA4`/TIM4 and its audio path is the chip's.  Decide: port, or compile out |

### C. Board facts to re-point when copying K1 code

* **The device header and register map differ** (PY32F071 -> PY32F403): GPIO
  alternate-function numbers, DMA channel mapping, SPI/ADC/TIM base addresses.
  Our `board.h`/`board_pins.h` split exists for this: keep K1 code away from the
  K1's `board.h` and give it ours.
* **The panel is bit-banged here** (no SPI to the LCD), so `st7565.c`'s SPI mode
  3 setup has to become our `lcd_st7565.c` transport.
* **The keypad is an ADC ladder** (5 lines, 20 buttons) and already maps to the
  K5V3 codes, so the app's input layer should be reusable once adapted.
* **The EEPROM is external SPI NOR**, not internal flash: the settings, the
  calibration, the journal and the "blob" area are all on that chip
  (`ra89r_eeprom.md`).
* **`0x0805FFF0` is the update-mode request, and the port must not touch it at
  boot.**  `0xFF` is normal (the bootloader starts the app); `0x11` asks for
  update mode, and the bootloader consumes the request by writing `0xFF` back
  (`ra89r_bootloader.md` §4c).  The port's equivalent of the stock's 0x08015710
  handler -- five bytes compared against `"Reset"`, command `'0'` writes `0x11`
  and resets -- is what lets the CPS switch a *running* port into update mode;
  without it the key combination is the only way in.  Writing `0x11` anywhere
  else, and in particular at startup, would send the radio straight back into the
  bootloader.

### D. Repository hygiene before app code lands

* **There is no `LICENSE` file**, yet the tree already contains Apache-2.0 code
  from the K1 (`driver/bk4819.c`, `bk4819.h`, `bk4819-regs.h` carry its headers).
  Bringing in the ~119k-line application makes this the first thing to fix:
  add the Apache-2.0 text and a `NOTICE`/`README` section recording the
  provenance of every imported file.
* **`main.c` must give way to the app's `main()`.**  Our console and bench are
  the port's debugging channel and should survive behind a build option (a
  `-DRA89R_BRINGUP_CONSOLE` style switch), with the app's entry point taking
  over by default.
* **The build** is a single `CMakeLists.txt` with an explicit source list; the
  app brings its own, so the port needs one CMake target with the K1 sources
  plus our drivers, and a clear boundary (no K1 file may include `board.h`).

## Progress

* **Stage 1 -- the rendering substrate: in, unvalidated on the radio.**  The
  K1's display layer now builds here and is exercised on a PC:
  `App/driver/st7565.{c,h}` (the K1 API -- `gStatusLine`, `gFrameBuffer[7][128]`,
  status line on panel page 0, frame lines on pages 1..7, the `+4` column offset
  -- over this repo's bit-banged transport and the bootloader-proven init),
  `App/font.{c,h}` + `App/bitmaps.{c,h}` (verbatim tables), `App/ui/helper.{c,h}`
  + `App/ui/inputbox.{c,h}` (verbatim drawing/text helpers), and the plumbing
  that made them compile: `App/port_features.h` (force-included feature macros,
  so the imported files stay untouched), `App/external/printf/printf.h` (newlib),
  `App/syscalls.c` (newlib stubs, `_write` to the console) and a **stopgap**
  `App/settings.{c,h}` that carries the single field `ui/helper.c` needs
  (`gEeprom.KEY_LOCK`) until the real state model arrives.  `LICENSE` and
  `NOTICE` are in, with the provenance table the port needs before more of the
  K1 lands.
  `firmware/tools/preview_k1.c` renders the K1 buffers as ASCII on a PC, and
  `App/k1_vfo_draft.c` draws a first VFO screen with the K1's own helpers
  (status line, `UI_DisplayFrequency`, `UI_PrintStringSmallBold`,
  `ST7565_Gauge`); console `G` shows it on the radio.  The draft is scaffolding:
  `ui/main.c`'s `UI_DisplayMain` replaces it.

* **Stage 2a -- the VFO screen is in and renders, unvalidated on the radio.**
  `App/ui/main.c` (the K1's `UI_DisplayMain`, 2465 lines, verbatim) compiles,
  links and draws here: `settings.h`/`radio.h`/`frequencies.h`/`dcs.h`/
  `functions.h`/`audio.h`/`helper/battery.h`/`app/{app,chFrScanner,dtmf}.h`/
  `driver/system.h` came in as the headers it needs, and
  `App/port_state.c` is the facade it links against -- `gEeprom.VfoInfo[2]`
  with a 145.7500 MHz default, the VFO pointers, and one stub per missing K1
  module, each naming its owner (functions.c, misc.c, radio.c, settings.c,
  helper/battery.c, app/dtmf.c, dcs.c) so replacing them is mechanical.
  `firmware/tools/preview_k1.c` renders `UI_DisplayMain()` on a PC (the
  frequency, the status line, the channel and the power/RSSI readouts all
  draw), and console `G` draws it on the radio.

* **Next -- stage 2b.**  `driver/keyboard.h` bound to our `keypad.c`, then
  `ui/ui.c` (the screen dispatcher) and `ui/status.c`/`ui/welcome.c`, then
  `ui/menu.c` with the real `settings.c`/`menu.c` behind it, and the key/action
  layer (`functions.c`) -- each step first on `preview_k1.c`, then on the radio.
  The stubs in `port_state.c` come out as their owner modules come in.

## Suggested order

1. **Fix A1, A2 (C/D)**, add the license, and wire the console behind a flag --
   all small, and all before any app code.  (The `Reset`+`'0'` handler rides
   along with the app's serial layer, not before it.)
2. **Storage**: finish the EEPROM write test, then implement `gEeprom` and the
   settings model over the external NOR flash.  The app cannot boot without it.
3. **Bring up the app's shell**: `main()`, the scheduler and the panel drawing
   on our bit-banged transport, with RF and audio stubbed.  Confirm the UI.
4. **RF**: fold the measured RX/TX chain into the `BK4819_*` layer, add the band
   table and the BK4815 handling (A3, A4, A5), and check each band on the radio.
5. **Audio**: the AF gains, the beeper (`PA4`/TIM4), the microphone and the
   `PC13` line's real role; then CTCSS/CDCSS, VOX, scramble and the compander
   from `ra89r_rffeatures.md`.
6. **Strip or disable** what the hardware lacks: FM broadcast, USB VCP, voice
   prompts, and pick a battery story.

## Open questions the port will have to answer

* Does the BK4815 radiate (above 134 MHz), and if so per band or exclusively?
* What does `PC13` actually switch (`driver/audio_path.c`)?
* Where does the RA89R get a battery voltage from, if the gauge chip stays
  silent?
* Which of the K1's `0x30` bit fields mean what on the BK4829 (the stock's TX
  value `0xC1FE` and the K1's `0xBFF1`/`0xC1FE` agree, but the bit *names* are
  the K1's)?
