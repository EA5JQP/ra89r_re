# The RA89R noise reduction ("denoise")

**Status: located in the CPS and in the stock image; nothing here has been
exercised on the radio.**  The chip-side register is identified; which of the two
candidate paths the side key actually toggles is still open.

The stock image has **no English "noise" string**.  The feature is the CPS's
**Noise Cancellation**, and it is a *chip-side* (transceiver DSP) function, not an
algorithm the MCU runs.

## What the CPS calls it

Three places name it, and together they bound the implementation:

* **`Class1.cs:417`, `KeyDefineBlueTooth_EngCh`** — the side-key (P1/P2) function
  list; index 9 is `降噪` / **"Noise Cancellation"** (0 none, 1 VOX, 2 dual wait,
  3 scan, 4 monitor, 5 1750, 7 power select, 8 alarming, **9 noise cancellation**,
  10 temporarily monitor, 11 FM radio, 12 talk around, 13 frequency reverse).
* **`AdjWin.cs:3399`** — the alignment window's hidden **`使能降噪模块`** ("enable
  noise-reduction module") checkbox, packed into one calibration byte as **bit 5**
  (`AdjWin.cs:7331`: `num2 = checkBox3.Checked ? 1 : 0; num2 <<= 5; num5 |= num2`).
* **`AdjWin.cs:3132-3168`** — the calibration fields beside it:
  `Reg63_Snr_N`, `Reg63_Snr_W`, `Reg65_Ex_W`, `Reg65_Ex_N` — i.e. the chip's
  **register `0x63` (SNR)** and **`0x65` (Ex noise)**, per narrow and wide
  bandwidth.

## Chip-side: the compander, register `0x28`

`FUN_0801C15C(on, mode)` is the stock's compander setter:

```c
FUN_0801C15C(on, mode):
    v = BK4815_read(0x28) & 0x1FFF;          /* keep the low 13 bits */
    if (mode == 1) v |= (on ? 0xE000 : 0xA000);
    else if (mode == 2) v |= 0x8000;
    BK4815_write(0x28, v);
```

So bits 15:13 of `0x28` carry the compander control: `0xA000` (bit 13 set),
`0xE000` (bits 15:13) or `0x8000` (bit 15).  This is the K1's `SetCompander`
(which also touches `0x29`/`0x31`) exposed as `MENU_COMPAND`.

Callers:

* **`FUN_08005C34`** — the BK4815 T/R + tune routine; when its flag
  (`*0x20005D24`) is set it writes `0xf023`/`0xf823` to `0x0c` and calls
  `FUN_0801C15C(1, ...)`.
* **`FUN_0801C1BC(on, mode, which)`** — from the key/menu handlers
  `FUN_0801A228`, `FUN_0801A6D0` and `FUN_0801D890`; `which == 0` routes to
  `FUN_0801C15C` (the compander), otherwise to `FUN_0801C19C` (a `0x70` gate on
  the BK4829).

## Chip-side: the noise / SNR detectors `0x63` / `0x65`

The squelch routine `FUN_080052B8` reads **`0x63`, `0x65` and `0x67`** and returns
0/1/2; the CPS calibrates the `0x63`/`0x65` thresholds (above).  So the chip
exposes a **noise-based** detector beside the RSSI one, and the stock's squelch
uses all three.  `FUN_080052B8` also walks the AGC step `0x13` in its eight-step
ramp, which is the runtime squelch gain.

## What runs where

The transceiver's **internal DSP** does the work; the MCU only writes registers:
`0x30` (RX/TX DSP enables), `0x13` (AGC), `0x28` (compander), `0x31`/`0x2B`/`0x71`
(scramble, VOX, tone), `0x4F`/`0x78` (noise and RSSI squelch), `0x63`/`0x65`/`0x67`
(metering).

The MCU is a **Cortex-M4F** (single-cycle MAC/SIMD and a single-precision FPU), but
the stock runs **no audio DSP on it**: the audio is analog from the chip's AF DAC
to the amplifier, and the MCU's ADC is the keypad (the battery is the separate
gauge chip, `ra89r_battery.md`).  There are no audio samples in the MCU, so an
MCU-side denoise would need either a chip register or an audio ADC this board does
not route.

The radio's one other audio path is the **Bluetooth module**, controlled over
**USART3** by AT commands (`FUN_08020C2C` sends; the ISRs are `FUN_08020698` /
`FUN_080208E4`): `AT+BT=EMITTER`/`RECEIVER`, `AT+BT_CALL=ON`/`OFF`,
`AT+MICGAIN=`/`SPKGAIN=`, and the `+IM_BT_EMITTER`/`+IM_BT_RECEIVER` responses.
Its audio interface is **analog too** -- the stock uses no I2S (the only SPI it
drives is `SPI1`, the EEPROM, and there is no `SPI2`/`SPI3`/I2S access) -- so the
BT microphone reaches the transceiver's mic input, not the MCU.  The MCU drives
the control link and the audio-source selection, not the samples.

## The port

The K1 driver already carries the relevant entry points, so a port-side denoise is
a register call, not new DSP:

* `BK4819_SetCompander()` → `0x28` (the stock's compander),
* `BK4819_SetupSquelch()` → `0x4F`/`0x78`,
* `BK4819_GetRSSI()` and the `0x63`/`0x65`/`0x67` reads,
* AGC (`0x13`), filter bandwidth (`0x09`/`0x43`).

## Open

1. **Which path the side key toggles** — the compander (`0x28`) or the noise
   squelch (`0x63`/`0x65`).  The compander is the strongest match (same register,
   same K1 name), but the side-key dispatch has not been followed to the call.
2. **The calibration byte** the `使能降噪模块` bit lives in, and which calibration
   page it is on.
3. Whether the "module" is the transceiver's compander or an **external**
   noise-cancellation part — no external part has been identified in the pin map.
4. Nothing here has been exercised on the radio.
