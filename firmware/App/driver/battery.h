/* Battery sense -- MCU ADC channel 9 (PB1).
 *
 * The stock measures the pack with its own ADC, not a companion chip: it scans
 * six channels (`FUN_08004E58`), five of them the keypad ladders, and reads the
 * sixth -- PB1, ADC channel 9 -- as a 0..255 level (`FUN_0800E514` /
 * `FUN_08007664`).  This driver exposes that channel.
 *
 * The earlier "two-wire gauge" here was a mis-identification: the bus on PC14 /
 * PB2 is the **BK1080 FM receiver** -- its I2C device ID is `0x80`, exactly the
 * byte that driver sent -- and the register it read (`0x0B`) is the FM RSSI, not
 * a pack voltage.  See ra89r_battery.md.
 */
#ifndef DRIVER_BATTERY_H
#define DRIVER_BATTERY_H

#include <stdint.h>
#include <stdbool.h>

/* Latest 12-bit sample of the battery channel (PB1, ADC channel 9), 0..4095.
 * The keypad's free-running DMA scan already samples it, so this is a memory
 * read, not a conversion. */
uint16_t battery_raw(void);

/* The stock's 0..255 level: raw >> 4 (`FUN_08007664`). */
uint16_t battery_level(void);

/* Pack millivolts from the raw sample.  The divider ratio is not recoverable
 * from the firmware, so this is provisional -- calibrate it on the radio:
 *   mv = raw * BATTERY_MV_NUM / BATTERY_MV_DEN
 * The default maps full scale (4095) to 8.4 V, i.e. ~2.05 mV per count. */
bool battery_mv(uint32_t *mv);

#endif /* DRIVER_BATTERY_H */
