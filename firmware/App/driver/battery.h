/* Companion gauge chip: pack voltage and charger status.
 *
 * A two-wire bus on PC14 (clock) and PB2 (data) -- I2C-shaped, and copied from
 * the stock firmware's own bit-bang rather than from a datasheet, since the
 * framing is not quite standard: a start condition, the byte 0x80, then the
 * register number sent as an address byte with the read bit (reg << 1 | 1).
 * 16-bit words are big-endian in both directions.  See ra89r_findings.md,
 * "Battery gauge".
 */
#ifndef DRIVER_BATTERY_H
#define DRIVER_BATTERY_H

#include <stdint.h>
#include <stdbool.h>

/* The registers the stock firmware polls; 11 is the voltage reading and 5 picks
 * the per-battery offset. */
#define BATTERY_REG_VOLTAGE 11u
#define BATTERY_REG_GAIN     5u

/* Set up the bus pins, then replay the stock's boot bring-up.  Idle state: clock
 * low (output), data driven high as an *output* -- never released, which is the
 * state the stock's byte write leaves behind. */
void battery_init(void);

unsigned battery_bus_scale(void);
unsigned battery_bus_rate_khz(void);
bool battery_bus_ok(void);

/* The bus-speed sweep, and the stage-by-stage result of the stock's boot
 * bring-up (FUN_0800D35C plus the enable FUN_0800D1F8 performs), which is the
 * sequence the stock runs once at boot before the first poll.  The chip answered
 * none of it when this was written, so each stage is reported separately: which
 * one (if any) acknowledges is the whole diagnostic. */
unsigned battery_bus_scale_count(void);
unsigned battery_bus_scale_value(unsigned index);
bool battery_scale_acked(unsigned index);
unsigned battery_stage_count(void);
const char *battery_stage_name(unsigned index);
bool battery_stage_ok(unsigned index);
unsigned battery_stage_acks(void);

/* Whether each bus pin could be driven and read back at the level it was set to.
 * A pin owned by something else (the LSE oscillator on PC14, an alternate
 * function, a short) fails this, and then no bus speed will help. */
bool battery_clk_pin_ok(void);
bool battery_data_pin_ok(void);

/* Whether the LSE was running (PC14 would then not be ours to drive).  Reported,
 * never changed: the stock drives PC14 without configuring it. */
bool battery_lse_on(void);

/* Read one 16-bit register, big-endian on the wire.  Returns false if the chip
 * did not acknowledge. */
bool battery_read(uint8_t reg, uint16_t *value);

/* Pack voltage in millivolts, computed the way the stock does: the 10-bit
 * reading from register 11 plus an offset (875 / 760 / 640, x 10 mV) selected by
 * the top two bits of register 5's low byte. */
bool battery_voltage_mv(uint32_t *mv);

#endif /* DRIVER_BATTERY_H */
