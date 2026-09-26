# RA89R indicators: the status LED and the backlight

**Status: the backlight is solved and working; the status LED is on `PA13`/`PA14`,
measured, with the per-die mapping still being read off.**  Both are recorded here,
including the searches that found nothing, so nobody repeats them.

## The status LED is a transmit/receive indicator

Green while receiving, red while transmitting -- the owner's description, and the
settings corroborate it from two directions:

* the firmware's own menu strings include `RX.LIGHT` (`0x08026D40`), `LIGHT`
  (`0x08026D4C`) and `Led Type` (`0x08026F8C`);
* the CPS's settings list carries `LED Mode`, `Led Type`, `Light` and **`Rx.Light`**.

### Measured: it is driven by `PA13`/`PA14` after all

**On the radio** (RA89R V49, our bring-up firmware, `driver/audiocontrol`): driving
the `PA13`/`PA14` pair from the console changes the LED -- the pair going to
(high, high) took it from **green+red** to **red**.  So the indicator is an MCU
line, contrary to the "not an MCU pin" conclusion below, and the earlier
`driver/audiocontrol` reading of the same pins as a *speaker* enable was wrong too
(the squelch does reach a pin, but that pin is this LED).

The pair is the stock's own LED drive:

* `FUN_08018A10` sets `PA14` to the level in `*(char *)(0x20009F28 + 0xc)` and
  `FUN_08020028` does the same for `PA13` -- the level bit, i.e. what `Led Type`
  most likely is, fed by `FUN_0800FE18` from bit 2 of codeplug settings byte 2;
* the squelch handler drives `PA14` (`FUN_08004C84` open -> high, `FUN_0801D458`
  closed -> low) and the TX/RX handlers drive it by mode (`FUN_08015D88`,
  `FUN_08015E28`: RX -> low, TX -> the settings level), which is exactly the
  `Rx.Light` / TX-red behaviour;
* the three helpers that **read the pin and flip it** (`FUN_08005F90` for `PA13`,
  `FUN_0800656C`/`FUN_08006070`/`FUN_08019C58` for `PA14`) are blinks -- what an
  indicator driver looks like, and what the "audio enable" reading never explained.

Both are configured as push-pull outputs together by the boot GPIO init
`FUN_08013C74` (GPIOA mask `0x6000`); they are the `SWDIO`/`SWCLK` pads, so the
stock gives up SWD to use them.

**Still open, and it is one radio session:** which die each pin drives and at which
level.  Expected mapping from the one measurement so far -- `PA14` = green,
active low, `PA13` = red, active high -- because that is what turns
`(PA13, PA14) = (high, low)` into green+red and `(high, high)` into red.  The
console's `A` now steps the pair through `(PA13, PA14)` = 10, 11, 00, 01 so all
four states can be read in one pass, and `C` toggles `PC13` for the separate
question of what that static line does.

### It is not GPIOA 0/1

This took a detour worth recording.  The theory was that the LED was on GPIOA pins 0
and 1: the stock *does* configure exactly those two as outputs (`FUN_080138FC`, mask
`3` as plain push-pull, alongside `PC15` and `PD0`, and nothing else in that pin setup
looks like an indicator).  Driving them, at either level, in every combination,
produces **nothing visible** on this radio.

The bootloader is no help either: its blink sequence (`0x0800057A` calls the PA5 block
`0x08000AD2`, then falls into the PA1 loop at `0x08000582`) lights **PA5 five times
and then PA1 three times**, each low for 100 ms then high.  PA5 is the panel
backlight, so what you see in update mode is the screen flashing; the PA1 half of that
blink is invisible.

### The evidence for where it really lives

`FUN_08016228` is the stock's TX/RX path -- it sets up the RF transceiver for transmit
or receive -- and it writes **only RF transceiver registers**, no GPIO at all:
`FUN_080220A0(0x6042 | 0x6142 | 0x6740, 0x47)`, `(0x3be | 0x3ff, 0x13)`,
`(0xbff1, 0x30)`, and `FUN_08022082(0x203, 0xc)` on the receive side (via
`FUN_080220A0`/`FUN_080180F0`, the BK4815/4829 register layer).

So the indicator is most likely a register bit on the RF chip, driven by the
transceiver's own T/R setup.  It arrives with the **RF bring-up**, and `driver/led.c`
is parked (it drives PA0/PA1 on request and documents that nothing happens).

> **Superseded** by the measurement above: the indicator is `PA13`/`PA14`.  The
> reasoning in this subsection is kept because it is why `PA0`/`PA1` were ruled out,
> not because the conclusion holds -- `FUN_08016228` writing only RF registers does
> not exclude the LED being driven from the *other* handlers around it, which is
> where `PA13`/`PA14` are.

### Open

* Whether a physical LED is fitted at all -- worth one look at the radio (front, top,
  beside the PTT) while it charges or while the stock firmware transmits.
* If it is: whether the RF chip's GPIO drives it, which the RF bring-up will show.
* What `Led Type` and `LED Mode` actually select, as distinct from `Light`.

## The backlight is GPIOA pin 5

**Confirmed on the radio by eye**: driving it lights the panel.  `driver/backlight.c`
drives it as a plain push-pull output, level 1 = on, and the console's `l` toggles it.

Related facts:

* The bootloader blinks the same pin five times when it enters update mode
  (`0x08000AD2`), ending lit -- which is the "Update..." screen being visible.
* The stock application also configures PA5 as `DAC_OUT2` (`FUN_0800A8F4` configures
  PA4 and PA5 as DAC outputs), so the stock is probably *dimming* the backlight through
  the DAC.  Plain on/off is what this radio needs today; a brightness ramp through the
  DAC is a later refinement.
* The old driver drove PA1 alongside PA5 on the theory that one of them was the lamp.
  PA1 does nothing, so it is not driven any more.

**A consequence worth keeping**: since PA5 is the backlight, the **beep must be PA4**
(`DAC_OUT1`) -- the beep and the backlight are separate pins and do not conflict.  The
beep itself is traced elsewhere: `FUN_08005BD0` -> `FUN_08005BEC` (gated on the BEEP
setting) -> `FUN_0801E75C`, which programs **TIM4** (`PSC = 143`,
`ARR = 1000000/freq - 1`), and the TIM4 update ISR (`0x0801DD54`) calls
`FUN_08004AF8`, a tone generator (phase accumulator, waveform nibble, note table at
`0x20000160`, 32-byte blocks for the DAC).

## What the pins ended up being

For the record, from the datasheet's pin map and the stock's configuration:

| pin | ADC channel | what it is |
|---|---|---|
| PA0, PA1 | `IN0`, `IN1` | plain outputs (stock); nothing visible when driven |
| PA2, PA3 | `IN2`, `IN3` | keypad ladder |
| PA4 | `IN4` | `DAC_OUT1` -- the beep |
| PA5 | `IN5` | `DAC_OUT2` **and the panel backlight** |
| PA6, PA7 | `IN6`, `IN7` | keypad ladder |
| PB0, PB1 | `IN8`, `IN9` | keypad ladder |
| PC0-PC3 | `IN10`-`IN13` | unused |
| PC4 | `IN14` | keypad line on the other board variant |
| PC14, PB2, PD0 | -- | the battery gauge bus and its handshake line |
