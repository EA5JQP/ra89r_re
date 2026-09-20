/* Companion gauge chip: pack voltage and charger status.
 *
 * A two-wire bus on PC14 (clock) and PB2 (data) -- I2C-shaped, and copied from
 * the stock firmware's own bit-bang rather than from a datasheet, since the
 * framing is not quite standard: a start condition, the byte 0x80, then the
 * register number sent as an address byte with the read bit (reg << 1 | 1).
 * See ra89r_findings.md, "Battery gauge".
 */
#ifndef DRIVER_BATTERY_H
#define DRIVER_BATTERY_H

#include <stdint.h>
#include <stdbool.h>

/* The registers the stock firmware polls; 11 is the voltage reading and 5 picks
 * the per-battery offset. */
#define BATTERY_REG_VOLTAGE 11u
#define BATTERY_REG_GAIN     5u

/* Set up the bus pins.  Idle state: clock low (output), data released (input). */
void battery_init(void);

/* The bus speed found at init, and whether any speed was acknowledged at all.
 * The stock's delay is a cycle count rather than a time, so the right scaling
 * depends on the clock it was compiled for and cannot be read out of the image;
 * battery_init() probes for it instead. */
#define BATTERY_SCALE_COUNT 5u

unsigned battery_bus_scale(void);
bool battery_bus_ok(void);

/* Whether each bus pin could be driven and read back at the level it was set to.
 * A pin owned by something else (the LSE oscillator on PC14, an alternate
 * function, a short) fails this, and then no bus speed will help. */
bool battery_clk_pin_ok(void);
bool battery_data_pin_ok(void);

/* Whether the LSE was running (PC14 would then not be ours to drive).  Reported,
 * never changed: the stock drives PC14 without configuring it. */
bool battery_lse_on(void);

/* How many of the three bring-up writes (registers 5, 3, 3) the chip
 * acknowledged -- 3 means the write path and the bus are working. */
unsigned battery_bringup_acks(void);

/* Read one 16-bit register.  Returns false if the chip did not acknowledge. */
bool battery_read(uint8_t reg, uint16_t *value);

/* Pack voltage in millivolts, computed the way the stock does: the 10-bit
 * reading from register 11 plus an offset (875 / 760 / 640, x 10 mV) selected by
 * the top two bits of register 5's second byte. */
bool battery_voltage_mv(uint32_t *mv);

#endif /* DRIVER_BATTERY_H */
