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

/* RF transceivers -- one shared 3-wire bus, a select per chip (board_pins.h) */
#define BK_SCL_PORT          GPIOA
#define BK_SDA_PORT          GPIOB
#define BK4829_CS_PORT       GPIOB
#define BK4815_CS_PORT       GPIOB
#define AUDIO_PATH_PORT      GPIOC

/* External SPI NOR flash -- the radio's "EEPROM" (see board_pins.h) */
#define SPI_FLASH_CS_PORT    GPIOA
#define SPI_FLASH_SCK_PORT   GPIOB
#define SPI_FLASH_MISO_PORT  GPIOB
#define SPI_FLASH_MOSI_PORT  GPIOB

/* The K1 application's board surface (see NOTICE).  The K1's board.c owns its
 * vendor bring-up and the battery ADC; on the RA89R the bring-up is this repo's
 * drivers and only the battery read is left, implemented in port_board.c. */
void BOARD_FLASH_Init(void);
void BOARD_GPIO_Init(void);
void BOARD_ADC_Init(void);
void BOARD_ADC_GetBatteryInfo(uint16_t *pVoltage, uint16_t *pCurrent);
void BOARD_Init(void);

/* Not mapped yet: the USB-C port goes to the MCU's USB peripheral -- nothing in
 * the stock firmware enables it, so it is not wired up here. */

#endif /* APP_BOARD_H */
