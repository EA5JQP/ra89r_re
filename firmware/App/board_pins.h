/* Numeric hardware facts for the RA89R board: pin masks, baud, clock.
 *
 * This header is deliberately free of any MCU/SDK include so it can also be
 * compiled on a PC (the layout preview in firmware/tools/preview.c uses it).
 * The peripheral instances (GPIOA, USART1) live in board.h.
 *
 * All values come from reverse engineering the stock firmware -- see
 * ../ra89r_findings.md and ../ra89r_lcd.md.
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

/* GPIOA pins 0 and 1: configured as outputs by the stock (FUN_080138FC), and the
 * bootloader blinks PA1 three times right after the PA5 blink.  Driving either at
 * either level produces nothing visible on the radio, so what they are is still
 * open -- a status LED driven by the companion chip, or a lamp line this board
 * does not fit.  Kept as documentation only; the LED test drives them. */
#define LED_PIN_A           (1u << 0)   /* PA0 */
#define LED_PIN_B           (1u << 1)   /* PA1 */
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
 * A pin x window pairing is one key: see ra89r_keypad.md for the
 * full table, the scanner (0x08024324) and the per-key handlers. */
#define KEYPAD_ANALOG_A_MASK  ((1u << 2) | (1u << 3) | (1u << 6) | (1u << 7))
#define KEYPAD_ANALOG_B_MASK  ((1u << 0) | (1u << 1))
#define KEYPAD_PTT2_PIN       (1u << 9)

/* Not mapped yet: SPI1 (SCK PB3, MISO PB4, MOSI PB5, NSS PA15) talks to the
 * external SPI NOR flash (Winbond-class, id 0xEF16); the RF transceiver
 * (BK4815/BK4829) is on a 3-wire bus; the USB-C port goes to the MCU's USB
 * device peripheral, which nothing in the stock firmware enables. */

#endif /* APP_BOARD_PINS_H */
