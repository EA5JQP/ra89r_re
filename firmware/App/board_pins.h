/* Numeric hardware facts for the RA89R board: pin masks, baud, clock.
 *
 * This header is deliberately free of any MCU/SDK include so it can also be
 * compiled on a PC (the layout preview in firmware/tools/preview.c uses it).
 * The peripheral instances (GPIOA, USART1) live in board.h.
 *
 * All values come from reverse engineering the stock firmware -- see
 * ../../docs/ra89r_findings.md and ../../docs/ra89r_lcd.md.
 */
#ifndef APP_BOARD_PINS_H
#define APP_BOARD_PINS_H

/* MCU: Puya PY32F403xD, Cortex-M4F, 384K flash @0x08000000, 64K SRAM.
 * Radio flash layout:
 *   0x08000000 - 0x08003FFF   stock bootloader (16K, do not overwrite)
 *   0x08004000 - 0x0805FFFF   application (this firmware) */

/* clock: the part boots on HSI = 8 MHz (datasheet) and this firmware stays
 * there -- enough for the bit-banged LCD and 115200 baud, no PLL bring-up risk */
#define BOARD_SYSCLK_HZ     (8000000u)
#define BOARD_APB1_HZ       (8000000u)   /* APB1 prescaler = 1 */
#define BOARD_APB2_HZ       (8000000u)   /* APB2 prescaler = 1 */

/* LCD: PB15 SDA, PA8 SCLK, PA10 A0/DC, PA11 CS, PA9 RESET
 * (stock firmware 0x08015032 / 0x0801501C / 0x08015086+0x080150AC /
 *  0x08015008+0x0801506C / 0x08014F44) */
#define LCD_DATA_PIN        (1u << 15)
#define LCD_SCK_PIN         (1u << 8)
#define LCD_DC_PIN          (1u << 10)
#define LCD_CS_PIN          (1u << 11)
#define LCD_RST_PIN         (1u << 9)

/* LCD backlight: GPIOA pin 5.  Confirmed on the radio: driving it lights the
 * panel, and the bootloader blinks it five times when it enters update mode
 * (0x08000AD2, low for 100 ms then high, ending lit).
 *
 * Note the pin is also DAC_OUT2 in the stock application (FUN_0800A8F4 configures
 * PA4 and PA5 as DAC outputs), which is why this is driven as a plain on/off
 * output here and not through the DAC: the stock may well be dimming the
 * backlight through it, and that is a later refinement, not a blocker.
 *
 * The old driver also drove PA1 on the theory that one of the two was the lamp.
 * PA1 does nothing visible, so it is not driven any more. */
#define BACKLIGHT_PIN       (1u << 5)
#define BACKLIGHT_ON_LEVEL  1

/* The status LED: a red/green pair on GPIOA 13 and 14, **both active high**.
 * Measured on the radio by stepping the pair through all four combinations and
 * reading the colour after each:
 *
 *   PA13 PA14   LED
 *     1    0    red
 *     1    1    red+green
 *     0    0    off
 *     0    1    green
 *
 * so PA13 is the red die and PA14 the green one.  The stock drives exactly this
 * pair: the level comes from bit 2 of codeplug settings byte 2 (`Led Type`),
 * `FUN_08018A10` sets PA14 and `FUN_08020028` PA13, both configured as push-pull
 * outputs by the boot GPIO init `FUN_08013C74` (GPIOA mask `0x6000`).  PA14 is
 * driven high while the squelch is open (`FUN_08004C84`) and low when it closes
 * (`FUN_0801D458`) -- the `Rx.Light` behaviour -- and PA13 is raised on the TX
 * entry path (`FUN_08018AB8`) and cleared on the RX one (`FUN_08017340`).
 *
 * That is why `PA13`/`PA14` are the `SWDIO`/`SWCLK` pads: the stock gives the
 * debug port up for the indicator.  See docs/ra89r_led.md.
 *
 * GPIOA pins 0 and 1, by contrast, are configured as outputs by the stock
 * (FUN_080138FC) and the bootloader blinks PA1 three times after the backlight
 * blink, but *driving either at either level produces nothing visible* here, so
 * they are not an indicator on this board and are no longer driven. */
#define LED_RED_PIN         (1u << 13)  /* PA13 */
#define LED_GREEN_PIN       (1u << 14)  /* PA14 */
#define LED_ON_LEVEL        1

/* programming UART: USART1 on PB6 (TX) / PB7 (RX), AF2 -- the stock
 * bootloader's port, the radio's Kenwood-style jack */
#define BOARD_UART_TX_PIN   (1u << 6)
#define BOARD_UART_RX_PIN   (1u << 7)
#define BOARD_UART_AF       2u           /* GPIO_AF2_USART1 */
#define BOARD_UART_BAUD     115200u

/* Keypad -- not read by this firmware, but parked back to its default state.
 *
 * The stock application reads 19 of the 20 buttons as analog levels: five lines,
 * each with a four-value resistor ladder, decoded by a 6-channel ADC scan.  The
 * lines are PA2, PA3, PA6, PA7 and PB0 (ADC channels 2 (or 14 on the other board
 * variant), 3, 6, 7 and 8) and both of the ADC's other inputs are analog too
 * (PB1 = channel 9); the twentieth button is PTT2 on PB9, a plain input.
 *
 * Those pins must stay analog, undriven and unpulled: driving or pulling a
 * ladder line corrupts its level and sinks current through it (that is what made
 * the radio warm up while a pin probe held the lines up).  main.c therefore sets
 * them back to that default at boot, and nothing else touches them.
 *
 * Windows and key codes (the vendor's own calibration, reusable verbatim):
 *   (0, 0x07C] (0x384, 0x47C] (0x8B2, 0x9AA] (0xABB, 0xBB3]   -- every line
 *   PA2 additionally (0x4AA, 0x5A2] and (0x74E, 0x846], plus a digital read
 * A pin x window pairing is one key: see docs/ra89r_keypad.md for the
 * full table, the scanner (0x08024324) and the per-key handlers. */
#define KEYPAD_ANALOG_A_MASK  ((1u << 2) | (1u << 3) | (1u << 6) | (1u << 7))
#define KEYPAD_ANALOG_B_MASK  ((1u << 0) | (1u << 1))
#define KEYPAD_PTT2_PIN       (1u << 9)

/* RF transceivers: two BK481x parts are fitted, sharing one bit-banged bus with
 * a chip select each (docs/ra89r_rf.md).  Clock `PA12`, bidirectional data `PB12`
 * (driven to send, released to read), and `PB8` = BK4829, `PB13` = BK4815 --
 * both selects are active low and the stock drives them by hand.
 *
 * Evidence: the stock's write primitive `FUN_08021FF4` (BK4829, select `PB8`)
 * and read `FUN_080180F0` bracket their transfer with `PB8` low, while
 * `FUN_08021F78`/`FUN_08018060` do the same with `PB13`; `FUN_08009772` reads
 * register 0 over the first and compares with `0x4829`, `FUN_08009758` over the
 * second against `0x4816`. */
#define BK_SCL_PIN           (1u << 12)   /* PA12 */
#define BK_SDA_PIN           (1u << 12)   /* PB12 */
#define BK4829_CS_PIN        (1u << 8)    /* PB8  */
#define BK4815_CS_PIN        (1u << 13)   /* PB13 */

/* The status LED is on `PA13`/`PA14`, and it is an MCU line after all.
 *
 * The stock drives this pair from its receive and transmit state machines:
 *
 *   state[0x21] >= 7 (signal present, squelch open) -> FUN_08004C84
 *        -> FUN_08018A10 -> PA14 = *(char *)(0x20009F28 + 0xc)
 *   state[0x21] <  3 (squelch closed)              -> FUN_0801D458 -> PA14 LOW
 *   TX (mode field state[2] != 0)                  -> FUN_08015D88/15E28 -> PA14 high
 *
 * with `FUN_08020028` the `PA13` half of the same level pair, and both configured
 * together as push-pull outputs by the boot GPIO init `FUN_08013C74` (GPIOA mask
 * `0x6000`).  The level comes from bit 2 of codeplug settings byte 2
 * (`0x20009F28 + 0xc`); this radio's block has it clear, i.e. active HIGH.  The CPS
 * setting list carries `LED Mode`, `Led Type` and `Rx.Light`, which is what that bit
 * and these two routines look like.
 *
 * *Measured on the radio*: toggling `PA14` from the console changes the LED -- it
 * goes from green+red to red -- so this pair is the indicator, not a speaker
 * enable.  The green/red assignment and the active level of each die are being
 * mapped (see docs/ra89r_led.md).  The three "toggle the pin" helpers the stock has
 * (`FUN_08005F90` for PA13, `FUN_0800656C`/`FUN_08006070`/`FUN_08019C58` for PA14)
 * are blinks, which is what an indicator driver looks like.
 *
 * `PC13` is the separate, static line: `FUN_080177A8` raises it and its
 * `config+0x38` gate is 0 on this codeplug, so it is held HIGH.  That is the
 * remaining candidate for the audio-path/amplifier enable, and it is what the
 * K1-driver callback drives. */
#define AUDIO_PATH_PIN       (1u << 13)   /* PC13 -- amplifier enable candidate, held HIGH */

/* The PA power/bias is a PWM, not a register: the stock's "Pow AdjData" path
 * (FUN_08016A2C -> FUN_0801830C -> FUN_0801BDE8 -> FUN_08018A88 -> FUN_080167B4
 * -> FUN_0801306E) programs **TIM1 channel 2**, whose pin is configured at
 * 0x080131AC as GPIOB mask 0x4000 with alternate function 4 -- i.e. `PB14`.  The
 * timer runs from the 144 MHz APB2 clock with the period computed by
 * FUN_08016C58 from the boot argument 100: 144e6 / 100 / 1000 = 1440, so
 * ARR = 1439, PSC = 0, a 100 kHz PWM.  Compare = the codeplug's power value
 * (0..252), clamped to ARR/2, and 0 in receive -- the PA has no bias at all
 * unless this is programmed, which is why a bench that only set the chip's
 * registers produced a weak, hissing signal. */
#define PA_PWM_PIN          (1u << 14)  /* PB14, TIM1_CH2, AF4 */
#define PA_PWM_AF           4u

/* The band-path pins the stock's transmit and receive selects both leave at
 * PA1 = 1, PA0 = 0 (FUN_08013A70(2) and (3)); only the chip's PA enable and
 * register 0x36 differ between the two directions.  See driver/pa.c. */
#define PA_BAND_PA1_PIN     (1u << 1)   /* PA1 */
#define PA_BAND_PA0_PIN     (1u << 0)   /* PA0 */

/* Not mapped yet: SPI1 (SCK PB3, MISO PB4, MOSI PB5, NSS PA15) talks to the
 * external SPI NOR flash (see docs/ra89r_eeprom.md); the USB-C port goes to the
 * MCU's USB device peripheral, which nothing in the stock firmware enables. */

#endif /* APP_BOARD_PINS_H */
