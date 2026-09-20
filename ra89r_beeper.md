# RA89R beeper

**Status: traced, not implemented.**  The beep is a synthesised tone on the DAC, not
a square wave on a GPIO, and the whole path from the key dispatcher to the DAC is
mapped below.  Its pin is `PA4` (`DAC_OUT1`) -- `PA5` is the panel backlight, so the
two do not conflict (see `ra89r_led.md`).

### Beeper -- the beep is the DAC, not a GPIO

The key beep is a **synthesised tone on the DAC output**, not a square wave on a
spare pin, so this one does not become a second `driver/backlight`.  The path,
read from the vendor's own code:

* `FUN_08005BD0(id, n)` -- what the key dispatcher calls (`FUN_08005bd0(7,1)`,
  `(8,2)`, `(7,2)`) -- is gated on the menu's **BEEP** setting (byte
  `0x20003DDC+0x16`), which is why the beep can be off.
* `FUN_08005BEC` queues it: message id at `+0xd`, a u16 `+0x12 = 4`, then
  `FUN_0801E75C(20000)`.
* `FUN_0801E75C(freq)` fills the TIM4 handle at `0x2000377C`: base
  **`TIM4` (`0x40000800`)**, `PSC = 0x8F` (143), `ARR = 1000000/freq - 1` (49 for
  20000), `CR1 |= ARPE` (`0x80`), then `FUN_08012F5E()` -> `FUN_0801DD80(TIM4, cfg)`
  writes ARR (`+0x2C`), PSC (`+0x28`), EGR (`+0x14`).  At the assumed 1 MHz that
  is 20 kHz; with the real 8 MHz timer clock it is ~1.1 kHz, i.e. an audible beep
  plus the prescaler's own divide, which is a detail to settle by ear.
* TIM4's update ISR (**`0x0801DD54`**) clears `SR` and calls `FUN_080076E0`, which
  in turn calls **`FUN_08004AF8`** on *every* update, before any flag check.  That
  is the tone generator: a phase accumulator (`[0x200044B0+0x28]`, `lsrs #1`,
  wraps at `0x4FE0`), a waveform-shape nibble chosen by bits 1..5, a note-length
  table at `0x20000160` (`ldrsh` per note index), and a 32-byte block copied by
  `FUN_08004168` -- the sample stream the DAC plays.  (Debug string for it:
  `"Tonech:%x"`.)
* The DAC is `0x40007400`, clocked by `RCC_AHB2ENR` bit 2, initialised in
  `FUN_0800A8F4`, and its GPIO config is **GPIOA mask `0x30` = PA4 + PA5**, i.e.
  `DAC_OUT1`/`DAC_OUT2`.

A second timer path has the same shape and is *not* the beeper: TIM2 (handle
`0x20000CE8`, ISR `0x0801DD28`) calls `FUN_0801E5EC`, the audio/AF side.

**Which of PA4 / PA5 carries it is not yet pinned down** -- the vendor configures
both pins -- and that matters because our `driver/backlight` drives **PA5**, a DAC
output.  The stock drives PA1 as a GPIO and treats PA5 as the DAC, so the lamp
probably only needs PA1; but that driver was validated with both pins driven, so
any change there has to be re-heard on the radio rather than assumed.
