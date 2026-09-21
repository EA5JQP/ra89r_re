/* Board definition for the Retevis RA89R / RA89G (MCU-specific part).
 *
 * Numeric pin masks, the baud and the clock live in board_pins.h so that the
 * screen layout can also be compiled on a PC (see tools/preview.c); this header
 * adds the peripheral instances and therefore needs the vendor device header.
 *
 * Everything below is reverse engineered from the stock firmware -- see
 * ../ra89r_findings.md and ../ra89r_lcd.md.
 */
#ifndef APP_BOARD_H
#define APP_BOARD_H

#include "py32f4xx.h"
#include "board_pins.h"

/* GPIO ports used by the drivers */
#define BACKLIGHT_PORT      GPIOA
#define LED_PORT            GPIOA
#define LCD_DATA_PORT       GPIOB
#define LCD_CTRL_PORT       GPIOA
#define BOARD_UART_PORT     GPIOB
#define BOARD_UART          USART1

/* Keypad lines (see board_pins.h): the ladder inputs and the one digital key */
#define KEYPAD_ANALOG_A_PORT GPIOA
#define KEYPAD_ANALOG_B_PORT GPIOB
#define KEYPAD_PTT2_PORT     GPIOB

/* External SPI NOR flash -- the radio's "EEPROM" (see board_pins.h) */
#define SPI_FLASH_CS_PORT    GPIOA
#define SPI_FLASH_SCK_PORT   GPIOB
#define SPI_FLASH_MISO_PORT  GPIOB
#define SPI_FLASH_MOSI_PORT  GPIOB

/* Not mapped yet: the RF transceiver (BK4815/BK4829) is on a 3-wire bus on
 * PA12 + PB8 + PB12; the USB-C port goes to the MCU's USB peripheral, which
 * nothing in the stock firmware enables. */

#endif /* APP_BOARD_H */
