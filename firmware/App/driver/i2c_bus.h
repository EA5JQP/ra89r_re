/* Bit-banged I2C for the BK1080 FM receiver.
 *
 *   PC14  clock  (`BK1080_SCL_PIN`)  -- idles low
 *   PB2   data   (`BK1080_SDA_PIN`)  -- bidirectional, driven to send and
 *                                       released to read, idles high
 *
 * These are the two lines `ra89r_battery.md` once read as a companion gauge;
 * the part is the BK1080 and `ra89r_bk1080.md` is the re-identification.  This
 * layer is only the wire: start/stop, a byte in, a byte out, and the master's
 * ACK bit.  The BK1080's own device id and register control word live in
 * `bk1080.c`, so a host test can replace these primitives and check the exact
 * frame -- the same split as `rf_bus.c` and `bk4829.c`.
 *
 * Timing follows the stock's own bit-bang rather than a datasheet guess:
 * `FUN_08006EF0` (start), `FUN_08006F4C` (stop), `FUN_0800705C` (write a byte,
 * then release the data line and poll it low for the ACK), `FUN_08006E78`
 * (read a byte, sampled while the clock is high) and the master ACK in
 * `FUN_08007158`.  Note the read samples on the *high* half of the clock, the
 * opposite phase from the 3-wire RF bus (`rf_bus.c`).
 *
 * The stock's delay helper (`FUN_0802422A`) is a cycle count, not a time, so
 * the edge spacing here is a settable loop count with the same caveat.  The
 * default is deliberately below the BK1080's 2.5 MHz ceiling and above the
 * stock's own ~57 kHz (docs/ra89r_battery.md measured the zero-delay variant at
 * ~250 kHz).  Nothing on this bus has been exercised on the radio yet.
 */
#ifndef DRIVER_I2C_BUS_H
#define DRIVER_I2C_BUS_H

#include <stdbool.h>
#include <stdint.h>

/* Bring up the two pins as outputs and park the bus (clock low, data high). */
void i2c_bus_init(void);

/* Start condition: clock low, data high, clock high, data low, clock low. */
void i2c_bus_start(void);

/* Stop condition: clock low, data low, clock high, data high. */
void i2c_bus_stop(void);

/* Shift one byte out MSB first, then release the data line and poll it for the
 * target's acknowledge.  Returns true when the target pulled it low (ACK). */
bool i2c_bus_write_byte(uint8_t value);

/* Shift one byte in MSB first, sampling while the clock is high.  Leaves the
 * data line released; the caller sends the ACK with `i2c_bus_send_ack`. */
uint8_t i2c_bus_read_byte(void);

/* Drive the master's ACK bit after a read byte and pulse the clock: `ack` true
 * pulls the line low, false releases it high (NACK, for the last byte). */
void i2c_bus_send_ack(bool ack);

/* Edge delay, in loop iterations of a few cycles each.  A register access is a
 * few hundred of them, so the default stays in the hundreds of kHz. */
#define I2C_BUS_DELAY_DEFAULT  16u
extern uint8_t gI2cBusDelay;
void    i2c_bus_set_delay(uint8_t iterations);
uint8_t i2c_bus_delay_setting(void);

#endif /* DRIVER_I2C_BUS_H */
