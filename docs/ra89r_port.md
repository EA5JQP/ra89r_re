# Porting the F4HWN (UV-K1/K5V3) application onto the RA89R

**Status: the VFO screen and the menu are ported and render, driven by the
radio's keys.**  What is in (`port`, unvalidated on the radio -- see "Progress"
at the bottom for the stage-by-stage log):

| layer | state |
|---|---|
| the K1's own screens | `ui/main.c` (VFO), `ui/menu.c` (menu + `MenuList[]`), `ui/status.c` (status line), `ui/welcome.c` (boot screen), `ui/ui.c` (dispatcher), `ui/helper.c` + `ui/inputbox.c` (drawing/text) -- imported verbatim and compiled here |
| the K1's own tables and support code | `font.c`, `bitmaps.c`, `dcs.c`, `frequencies.c`, `version.c`, `helper/battery.c`, `app/menu.c`, `app/action.c` -- imported verbatim |
| the display | `driver/st7565.c` in the K1's layout (`gStatusLine`, `gFrameBuffer[7][128]`, page 0 = status) over this repo's bit-banged panel and the bootloader-proven init |
| keys | `driver/keyboard.c` -- the K1's interface over this repo's ADC-ladder keypad; `port_gui.c` routes press/hold edges (`MENU_ProcessKeys` for the menu) and switches screens |
| storage | `port_storage.c` implements the K1's `PY25Q16_*` over this repo's SPI NOR driver (`driver/spi_flash.c`, write path new); the port's settings blob lives in the part's empty tail |
| the state facade | `port_state.c` -- `gEeprom` (VFO objects, settings), the runtime globals and one inert stub per K1 module not yet ported, each named after its owner |
| not ported, stubbed | the RF/audio/scanner/DTMF engines, the CPS codeplug mapping, USB/voice/FM features the hardware lacks |

**The K1 GUI is what the radio boots into**: the panel and the keys belong to it
from power-on, straight into the VFO (the K1 boot screen is still there behind
console `4`), and the bring-up screens are console diagnostics behind `0`.  The
**double-channel UI** is the default, but it no longer rides on dual-watch:
`port_features.h`'s `PORT_TWO_ROW_UI` forces `ui/main.c`'s `isMainOnly()` false
(which normally means "dual watch off *and* cross-band off"), and
`gEeprom.DUAL_WATCH`/`CROSS_BAND_RX_TX` stay **OFF**.  Leaving dual-watch on just
to draw the second row also ran the K1's `DualwatchAlternate()`: it toggles
`gEeprom.RX_VFO` and retunes every ~500 ms, so the receiver ignored the VFO the
user selected with `EXIT` (this radio's A/B key), and it diverted
`CheckForIncoming()` away from the port's polled `g_SquelchLost`.  The receiver
now follows the selected VFO; `preview_k1` asserts it.
Placeholder channels until the stock codeplug is mapped: VFO A on 145.7500 and
VFO B on 145.5000, so the two rows are visibly different.  The console: **`1`** back to the
GUI, **`2`** VFO, **`3`** menu, **`M`**/`G` draw those once, **`4`** the boot
screen, **`5`** saves the settings blob, **`6`** runs the flash write test,
**`e`** dumps the flash, **`0`** hands the panel to the bring-up screens (test
card, border, fill, animation, panel re-init).  On a PC, `preview_k1.c` renders
the same screens (and drives the same key loop) as ASCII -- see AGENTS.md,
"Offline checks".

`ra89r_findings.md` stays the hardware write-up; this is the port's own.

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
| `driver/backlight.c` | `PA5`, confirmed; the K1 driver: TIM7+DMA software PWM (4 kHz, 32 levels), fade, `BACKLIGHT_MIN/MAX` |
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

   *This branch's current stage (2026-09-30):* RX is deliberately **BK4829-only**.
   The port tunes the BK4829 (`rx_set_frequency()`, now called from
   `port_gui_tick()` when the GUI RX frequency moves, in addition to the app's
   own `RADIO_SetupRegisters()`) and selects its receive path.  `pa.c`'s
   `PA_CHIP_PATH_AUTO` uses `BK4819_ToggleGpioOut` (a read-modify-write, as the
   stock's `FUN_080137D4` does) with the K1/app LNA rule, so register `0x33`'s
   `0x9000` bits survive; the previous raw write (`0x04`/`0`) had cleared them
   and stopped `0x67` following a carrier.  The BK4815 is still configured and
   parked (`0x75`/`0x0c`), but its receive dispatch is deferred until this stage
   is settled on the radio.  `firmware/tools/test_rf.c` pins the `0x9000`
   preservation as a host regression.

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

* **Stage 2b -- the menu screen is in and renders too (unvalidated on the
  radio).**  `App/ui/menu.c` (the K1's `UI_DisplayMenu`, 1844 lines, verbatim
  with its `MenuList[]` table) compiles, links and draws: the list, the
  selected item's name and its value column.  With it came the real
  `app/menu.c` (sub-menu strings), `app/action.c`, `app/common.h`,
  `app/generic.h`, `app/scanner.h`, `version.c`, `dcs.c`, `frequencies.c`,
  `helper/battery.c` and `ui/{battery,welcome}.h`, plus `driver/eeprom.h`; the
  feature set now follows the K1's `default` CMake preset for the entries that
  only affect drawing (`ENABLE_BIG_FREQ`, `ENABLE_CUSTOM_MENU_LAYOUT`,
  `ENABLE_KEEP_MEM_NAME`, `ENABLE_WIDE_RX` on top of `ENABLE_FEAT_F4HWN`,
  `ENABLE_SMALL_BOLD`, `ENABLE_VOX`), and stays off for what the RA89R lacks or
  the port cannot back yet.  The PC previews got a host-only device header
  (`tools/host/py32f4xx.h`, because CMSIS's `__DSB()` is ARM assembly) and a
  stand-in for the backlight and the two `BK4819_*` calls the screens make.
  Console `M` draws the menu on the radio, `G` the VFO.

* **Stage 2c -- the GUI is driven by the radio's own keys (unvalidated on the
  radio).**  `App/driver/keyboard.{c,h}` is the adapter: the K1's keyboard
  interface over this repo's ADC-ladder reader (its `KEY_Code_e` now comes from
  `driver/keypad.h`, so there is one definition of the codes), and
  `App/port_gui.c` is the port's small stand-in for the K1 `app/main.c` key
  routing -- press/hold edges, `MENU_ProcessKeys` for the menu screen, tuning
  for the VFO, and screen switching through `gRequestDisplayScreen`.  `ui/ui.c`
  (the K1's dispatcher) is in as well; the port still draws its two screens
  directly rather than through `UI_DisplayFunctions[]`, because that table
  links every screen it lists.  Console `1` toggles the interactive GUI, `2`
  the VFO and `3` the menu; `preview_k1.c` presses the same keys on a PC and
  now shows the cursor and the frame changing (the redraw was gated wrongly at
  first -- a key that set `gRequestDisplayScreen` suppressed the repaint).

* **Stage 2d -- the status line and the boot screen (unvalidated on the
  radio).**  `ui/status.c` (`UI_DisplayStatus`, battery drawing included) and
  `ui/welcome.c` (`UI_DisplayWelcome`) are in; `port_gui.c` now redraws the
  status line with every screen, which is what the K1 does from its app loop
  (app/app.c calls UI_DisplayStatus), and console `4` shows the boot screen.
  Both render on the host too: the welcome screen draws its "WELCOME" + voltage
  fallback because the boot-message area of the (absent) external flash reads as
  empty.  That read is the storage seam: `PY25Q16_ReadBuffer` is a
  `port_state.c` stub until the SPI NOR driver lands, so the codeplug and the
  boot messages are empty and every menu shows its compiled-in default.

* **Stage 2e -- storage: the external flash answers, and the port keeps its
  settings on it (unvalidated on the radio).**  `driver/spi_flash.c` (this
  repo's bit-banged SPI NOR driver, brought over from branch `driver/eeprom`)
  gained the write path -- write-enable, page program split at page boundaries,
  sector erase, WIP polling -- and `App/port_storage.c` implements the K1's
  `PY25Q16_*` interface over it plus the port's own settings blob in the part's
  empty tail (`0x1FF000`; the write test uses `0x1FE000`).  `port_state_init()`
  now loads that blob if it is valid, so the menus see saved settings instead of
  defaults; console `5` saves it, `6` runs the write test and `e` prints the
  flash identity and a hexdump.  The host fake in `tools/host/host_hw.c` models
  the two sectors (AND on program, `0xFF` on erase), and `preview_k1.c` checks
  the round-trip there: save -> change -> load returns the saved values, and the
  write test passes.

  One bug this shook out, worth keeping in mind: `gEeprom`'s VFO objects carry
  pointers into themselves, so anything that replaces the struct -- the defaults
  or a blob read back from flash -- invalidates them.  `port_state_fixup_vfo()`
  re-establishes them, and `port_storage_load_settings()` calls it after loading;
  the host preview crashed on exactly this before the fix.

  The stock's *channel* data is mapped now: the one-time import in
  `driver/py25q16.c` (`storage_import_k1()`) also writes the K1's channel
  records (`0x9000`), names (`0x4000`) and attributes (`0x8000`) from the stock's
  codeplug, so `settings.c`'s `SETTINGS_Fetch*`/`SaveChannel*` and `misc.c`'s
  attribute table are the K1's own code reading the K1's own image -- the stock
  decoder is reached only by the import (`ra89r_codeplug.md` has its format).

  The import is gated by a layout marker at `0x100B8` ("RKT" plus a version
  digit), and **the version is part of the value on purpose.**  It was `...1`
  when the import carried only the calibration, so a radio that had already
  imported under the older build skipped the channel import and booted with an
  empty image -- which `RADIO_ConfigureChannel` turns into a fallback to a
  frequency channel (the "stuck on F3 / 18 MHz" fault, and
  `COMMON_SwitchVFOMode` then has no channel to switch to).  Bumping it to
  `...2` makes this build re-import on first boot; bump it again whenever the
  image's layout changes.  The host fake in `tools/host/host_hw.c` now models
  the image region and `preview_k1.c` asserts the boot lands on a memory
  channel, so a skipped import fails offline instead of on the radio.

* **Stage 3a -- the app core is in and the radio works from the K1 UI
  (unvalidated on the radio).**  `radio.c`, `functions.c`, `audio.c`, `misc.c`
  and a port `driver/system.c` (delay over this repo's SysTick) now compile and
  link, which retired ~65 of `port_state.c`'s stubs.  The imported K1 sources
  reach the hardware through a small logical surface, so `driver/gpio.h` grew it
  (`GPIO_SetOutputPin`/`Reset`/`Toggle`/`IsInputPinSet`, the audio path, the
  backlight, `GPIO_IsPttPressed` reading the keypad) mapped to this board's pins,
  and `SQL_TONE` (the K1 CMake's `-DSQL_TONE=550`) is in `port_features.h`.  The
  core touches almost no LL GPIO: two calls in the whole set.

  `port_gui.c` now drives the measured chains rather than only the views: UP/DOWN
  retunes the receiver (`rx_set_frequency`), PTT keys the transmitter
  (`tx_start`/`tx_stop`, the measured `0x36`/`0x50`/`0x33` chain -- *not* the
  K1's unvalidated `RADIO_SetTxParameters`), the squelch tick polls `rx_poll()`
  and sets `FUNCTION_INCOMING`/`FUNCTION_RECEIVE`, and `RADIO_*`/`FUNCTION_*`
  supply the state the screens read.

  Known split, deliberate: `radio.c`'s own chip sequences
  (`RADIO_SetupRegisters`, `RADIO_SetTxParameters`) are the K1's and are *not*
  used yet -- on this radio they need comparing against the stock first
  (`ra89r_rffeatures.md` locates them).  The measured `rx.c`/`tx.c` chains are what
  the port runs.

* **Stage 3b -- the K1's own key handling and app loop run the radio
  (unvalidated on the radio).**  `app/app.c` (the loop, `CheckKeys`,
  `APP_Update`, the 10 ms/500 ms slices), `app/main.c` (`MAIN_ProcessKeys`),
  `app/generic.c`, `app/common.c`, `app/chFrScanner.c`, `app/dtmf.c` and
  `app/scanner.c` come in verbatim, plus `port_board.c` (the `BOARD_*` surface:
  only the battery read is the application's, and it is a placeholder pack
  because the gauge chip never answers) and `driver/system.c`
  (`SYSTEM_DelayMs` over SysTick).  That retired the rest of `port_state.c`'s
  stubs -- what is left there is settings (step 2) and `ui/status.c`'s
  `UI_DisplayBattery`.

  The main loop now calls `CheckKeys()` (the K1's press/hold/repeat and
  `MAIN_`/`MENU_`/`SCANNER_ProcessKeys` per screen) and `APP_Update()`.  **PTT is
  deliberately excluded from `CheckKeys`**: `GPIO_IsPttPressed()` returns false so
  the K1's `GENERIC_Key_PTT -> FUNCTION_Transmit -> RADIO_SetTxParameters` path
  does not run, and `port_gui.c` keeps driving the *measured* `tx_start`/`tx_stop`
  chain instead.  Flipping that one function back is the switch, once the K1's
  transmit sequence has been compared with the stock.

  The host preview now links the whole application core with doubles for the
  drivers it does not build (`BK4819_*`, the GPIO path helpers, the SysTick
  delay, the transmit chain, `gRxIdleMode`), and still renders its 8 frames and
  the storage round-trip.

* **Stage 3c -- the app owns the panel and the keys (unvalidated on the radio).**
  Three fixes after the first radio test of stage 3b, all of them the port
  fighting the application rather than the application being wrong:

  * the menu showed an empty list because nothing built its view: the K1 does
    that once in its own `main()` (`App/main.c:190`), so the port now calls
    `UI_MENU_BuildView()` at boot, after the state is up;
  * the keys were handled **twice** -- `APP_TimeSlice10ms()` ends with
    `CheckKeys()` and the port's loop was calling it as well -- which is what
    made them feel intermittent; the loop is now the K1's own:
    `APP_Update()` + the 10 ms/500 ms slices;
  * the panel was drawn twice (the port's own repaint plus the application's
    `gUpdateDisplay -> GUI_DisplayScreen()`), so `port_gui` no longer draws at
    all: it keeps PTT (measured chain), the receiver poll and the welcome
    screen, and asks for a repaint with `gUpdateDisplay`.

  Also, as asked: **EXIT switches VFO A/B**.  This radio has no dedicated A/B
  key, so the port adapted `MAIN_Key_EXIT` -- a short EXIT press with nothing to
  cancel (no digits typed, not scanning) calls `COMMON_SwitchVFOs()`, the K1's
  F + 2 action.  Recorded in NOTICE.

  And the K1's **boot-time key mode** (`helper/boot.c`): holding **PTT + SIDE1**
  at power-on (then releasing) opens the hidden menu -- `gF_LOCK = true`, the
  cursor on `FIRST_HIDDEN_MENU_ITEM`.  The K1 reads PTT through
  `GPIO_IsPttPressed()`, which the port keeps false (its PTT runs through
  `driver/tx.c`), so the port reads PB9 via `keypad_ptt2_level()` instead; only
  the F-lock mode is ported (air-copy/rescue-ops/multiboot are not enabled).

* **Stage 3d -- the visible gaps (unvalidated on the radio).**  `ui/battery.c`
  and `ui/scanner.c` come in verbatim (the battery widget and the scanner screen
  were stubs); `BK4819_ToggleGpioOut` mirrors the K1's two status-LED chip
  outputs onto this board's MCU LED (PA13/PA14), because the application lights
  its LED through the chip; and the backlight follows the K1's timer
  (`gEeprom.BACKLIGHT_TIME`, 5 s per unit, 61 = always on, 0 = off).

* **Stage 4a -- the codeplug is decoded and the channels are real (unvalidated on
  the radio).**  Step 2's first half.  `ra89r_codeplug.md` now holds the layout,
  from the CPS's own reader/writer and confirmed against the dump;
  `App/port_codeplug.c` implements the read side (the 21-byte channel records,
  the channel-used and scan-allow bitmaps, the tone encoding, the K1's
  `ChannelAttributes_t`), and `App/settings.c` is the K1's `SETTINGS_*`
  interface over it instead of a stub.  Two imported files were adapted for it
  and both are in `NOTICE`: `radio.c`'s `RADIO_ConfigureChannel` reads through
  `SETTINGS_FetchChannelScanDisplayInfo()` instead of the K1's own 16-byte
  record (so all layout knowledge lives in one file), and `misc.c`'s two
  channel-attribute flash functions map the stock's bitmaps rather than the
  K1's table at `0x8000` -- which on this chip is the middle of the codeplug.
  `RADIO_CheckValidChannel` also had to learn the K1's `0xFFFF` marker, or the
  channel walk stops on an unused slot: this radio really has four channels out
  of 210.  `port_state_init()` now performs the K1's own boot order
  (`SETTINGS_InitEEPROM`, `SETTINGS_LoadCalibration`, `RADIO_ConfigureChannel`
  twice, `RADIO_SelectVfos`) instead of leaving placeholder frequencies in
  `gEeprom`, which is why the screens showed a channel they never loaded.  The
  boot lands on the first two channels the codeplug has (CH-01 144.9750 and
  CH-02 145.7500 on this radio) and the measured receive chain tunes to
  whichever one is up.  On a PC the preview preloads a factory-shaped codeplug
  into its RAM flash and prints what the decoder made of it, tones included.

  The first radio run of it showed two things that had nothing to do with the
  codeplug and everything to do with `APP_TimeSlice10ms()`.  That slice calls
  `BACKLIGHT_Update()` and `SETTINGS_SaveVfoIndicesFlush()`, and the port had
  given both of them work the K1 does not do there: the backlight *fade* step
  was decrementing the 500 ms timeout (so the light went out in half a second,
  100 decrements a second instead of two) and the save *flush* was writing the
  whole settings blob (a 4 KB sector erase plus a program on every slice, which
  is what made the radio crawl).  Both now do what the K1's do, and both are
  asserted by the preview -- which is also why the preview now links the real
  `driver/backlight.c` and the host's flash double counts sector erases: with a
  stubbed backlight and a silent flash it could not have seen either.  The rule
  for the next ported module: nothing on the 10 ms slice may touch the external
  flash, and a timeout does not belong in a fade.

  The second radio run found the two that were left.  The port's own
  `port_gui_poll()`/`port_gui_tick()` sat outside the 10 ms slice, so the
  squelch read -- one `BK4819_GetRSSI()`, about 0.6 ms of bit-banged RF bus --
  ran on *every* pass of the main loop and left the application a tenth of the
  CPU; they are on the slice now, which is where the K1 reads PTT and the
  squelch too.  And the port's frequency (VFO) channel store was never
  initialised, so switching to frequency mode decoded 0 Hz, which
  `RADIO_ConfigureChannel` clamps to band 1: 18 MHz, a dead frequency, which is
  what "I cannot switch to VFO mode" looked like.  It starts at each band's own
  lower bound and, because the K1's band key does not exist here, the frequency
  channel now follows the band of the channel in use.  The preview drives the
  switch the way the radio does (`F` then `3`) and prints where it lands.

  The third radio run found the one that mattered most, and it had been there
  since the first commit: `driver/clock.c` left the MCU at the HSI reset default
  of **8 MHz with the PLL off**, "to avoid PLL bring-up risk".  The K1 this was
  ported from runs its smaller PY32F071 at 48 MHz, so every bit-banged bus and
  every busy-wait in the port was six times slower than the firmware it was
  compared against -- `rf_delay()`'s 40-iteration loop was 25 us, one RF
  register transfer ~1.8 ms, a full `RADIO_SetupRegisters` ~100 ms.  The
  PY32F403 is rated to 155 MHz.  The clock now runs at 48 MHz (HSI x 6, 1 flash
  wait state), with a bounded-wait fail-safe that falls back to 8 MHz and says
  so rather than hanging, and the keypad's ADCCLK prescaler, the RF driver's own
  delay loop and the boot log were brought with it.  `driver/clock.h` records
  the three constants to change for 96 or 120 MHz.  The lesson is the same as
  the last two: on a port, check what the *hardware* is doing before believing
  the software is at fault.

* **Still step 2, second half -- the settings block.**  `ra89r_codeplug.md` has
  the whole 32-byte block at `0x2020` mapped, with the CPS's labels and this
  radio's values, but nothing reads it yet: the port's own defaults and its blob
  decide.  Reading it into `EEPROM_Config_t` is the next piece, and it is what
  makes squelch, backlight, power-on display and the rest agree with what the
  stock firmware shows.  The stock's regions stay read-only until a channel
  editor needs the journal (`ra89r_eeprom.md`).

* **What is left, measured.**  Of the K1's 45k lines of application `.c`, what
  the port does not have:
  * the settings-block mapping above (the read side of `settings.c` now exists;
    what it lacks is the stock's own values behind it);
  * the K1's channel and name *writes*, deliberately: they are writes into the
    stock's codeplug, and the stock's own journal is how that has to be done;
  * features this hardware cannot run, kept off: `app/spectrum.c` (2,617),
    `app/foxhunt.c` (1,504), `app/rxtx_log.c` (1,376), `app/aircopy.c` (482) +
    `ui/aircopy.c`, `ui/multiboot.c` (576), `k5viewer.c`;
    *the FM broadcast feature is no longer in this list*: the RA89R does carry a
    BK1080 (`docs/ra89r_bk1080.md`), and the K1's `app/fm.c` + `ui/fmradio.c` +
    driver API are imported on branch `driver/fm` (unvalidated on the radio; the
    audio path is the open question);
  * the K1's own boot (`main.c` 343, `init.c`, `scheduler.c`, `board.c`), which
    this repo's bring-up replaces -- except the battery ADC, which is a
    placeholder until the gauge chip (silent, `ra89r_battery.md`) or the stock's
    battery path is used;
  * `driver/py25q16.c` (579): this repo's `spi_flash.c` + `port_storage.c` stand in
    for it, with the K1's sector cache and multiboot banking left out.

* **The compatibility rule, as built.**  The stock firmware keeps working on the
  same chip because nothing in the port writes a stock region: the codeplug, the
  journal, the blob, the calibration window and the `TYTDXC` signature are
  read-only, and the port's own state lives in its blob at `0x1FF000` (test
  sector `0x1FE000`) in the tail the dump shows erased from `0x10FE41`.  The
  channels the port shows are the stock's channels, so both firmwares and the
  CPS see the same radio.  When a channel editor does arrive it has to write
  through the stock's journal, in the stock's own layout; `ra89r_codeplug.md`
  records the two regions it must never assume it owns -- the calibration window
  and the signature.

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
