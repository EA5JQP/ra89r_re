/* The EEPROM console commands, split out of main.c so the firmware's
 * dump/restore/write-validation logic can be compiled and tested on a host
 * (see tools/test_eeprom_console.c) as well as on the radio.
 *
 * The transport and the chip are the driver interfaces (driver/uart.h,
 * driver/spi_flash.h); nothing here touches the board headers directly, so a
 * host test can supply its own fakes.
 */
#ifndef DRIVER_EEPROM_CONSOLE_H
#define DRIVER_EEPROM_CONSOLE_H

/* Identify the chip and report its size (console 'e'). */
void eeprom_report(void);

/* Stream the whole chip as raw binary (console 'E'). */
void eeprom_dump(void);

/* Restore the whole chip from the host: "W <size> <sum>\n" followed by <size>
 * raw bytes, 4 KB at a time with a '.' ack per sector (console 'W'). */
void eeprom_restore(void);

/* One-shot write validation on an empty sector (console 'Z'). */
void eeprom_write_validate(void);

#endif /* DRIVER_EEPROM_CONSOLE_H */
