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

/* programming UART: USART1 on PB6 (TX) / PB7 (RX), AF2 -- the stock
 * bootloader's port, the radio's Kenwood-style jack */
#define BOARD_UART_TX_PIN   (1u << 6)
#define BOARD_UART_RX_PIN   (1u << 7)
#define BOARD_UART_AF       2u           /* GPIO_AF2_USART1 */
#define BOARD_UART_BAUD     115200u

/* Not mapped yet: SPI1 (SCK PB3, MISO PB4, MOSI PB5, NSS PA15) talks to the
 * external SPI NOR flash (Winbond-class, id 0xEF16); the RF transceiver
 * (BK4815/BK4829) is on a 3-wire bus; the USB-C port goes to the MCU's USB
 * device peripheral, which nothing in the stock firmware enables. */

#endif /* APP_BOARD_PINS_H */
