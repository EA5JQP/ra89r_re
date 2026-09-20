/* Companion gauge chip -- pack voltage and charger status.
 *
 * Everything here mirrors the stock firmware's own bit-bang, function by
 * function, because the framing is not quite standard I2C and nothing else
 * documents it:
 *
 *   0x08006EF0  start: clock low, data high, clock high, data low, clock low
 *   0x08006F4C  stop:  clock low, data low,  clock high, data high
 *   0x0800705C  write a byte, MSB first, then release the line and wait up to
 *               250 polls for the chip to pull it low (its acknowledge)
 *   0x08006E78  read a byte, MSB first, sampled while the clock is high
 *   0x08007158  read a register: start, 0x80, (reg << 1) | 1, then 16-bit words
 *               -- low byte first, the master driving the acknowledge, the last
 *               word acknowledged with the line high -- then stop
 *   0x0800D138  the data pin's direction: output (arg 1) or released (arg 0)
 *
 * The stock's delay helper is 0x0802422A: a *fixed* loop of (n+1) x 21 iterations,
 * i.e. a cycle count rather than a time, so this mirrors its loop and lets the same
 * call sites give the same cycle counts.  At the 8 MHz this firmware runs on that
 * is slower in absolute time than the stock's PLL clock, which is the safe
 * direction for a bit-banged bus (there is no minimum clock rate).
 *
 * Not implemented: the stock also *writes* configuration to the chip and pulses
 * its reset line (PD0) at boot.  Reads alone are enough for a voltage reading --
 * a fuel gauge runs on its own -- so if a read comes back empty, that reset
 * pulse is the first thing to try.
 */
#include "driver/battery.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/systick.h"

#define CLK_PORT BATTERY_CLK_PORT
#define CLK_PIN  BATTERY_CLK_PIN
#define DAT_PORT BATTERY_DATA_PORT
#define DAT_PIN  BATTERY_DATA_PIN

/* The stock's own delay helper (0x0802422A): (n+1) x 21 iterations.  The inner
 * counter is volatile so the compiler cannot drop the loop, as it did not in the
 * stock image.  One unit here is ~30 us at 8 MHz. */
static void delay(unsigned n)
{
    for (unsigned i = 0; i <= n; i++)
        for (volatile unsigned j = 0; j < 21u; j++)
            ;
}

static void clk(int level)
{
    gpio_write(CLK_PORT, CLK_PIN, level);
}

static void dat(int level)
{
    gpio_write(DAT_PORT, DAT_PIN, level);
}

static void dat_output(void)
{
    gpio_config_output(DAT_PORT, DAT_PIN);
}

static void dat_input(void)
{
    gpio_config_input(DAT_PORT, DAT_PIN);
}

void battery_init(void)
{
    gpio_port_clock(CLK_PORT);
    gpio_port_clock(DAT_PORT);
    gpio_port_clock(BATTERY_RESET_PORT);

    gpio_config_output(CLK_PORT, CLK_PIN);
    /* The data pin idles as an *output*, driven high -- not released.  That is
     * what the stock's byte write leaves behind (it ends with FUN_0800D138(1),
     * i.e. back to an output) and what the stop condition ends on, so every
     * transfer starts with the pin able to drive.  Leaving it an input here was
     * the bug that made the chip deaf: the start condition and the first byte's
     * bits went nowhere, the data line never moved, and every register read back
     * as a floating 0x3FF with no acknowledge. */
    gpio_config_output(DAT_PORT, DAT_PIN);

    /* The gauge's reset line, PD0.  The stock drives it low for 10 ms and
     * releases it high (0x08006850 -- GPIO_WriteBit(GPIOD, 1, 0), a delay,
     * GPIO_WriteBit(GPIOD, 1, 1)) instead of leaving the pin alone, and a
     * floating reset input is the obvious reason a gauge would answer nothing at
     * all.  Its delay helper (0x080241F8) is not read here; for a reset pulse
     * being generous is harmless, so this just waits 10 ms each way. */
    gpio_config_output(BATTERY_RESET_PORT, BATTERY_RESET_PIN);
    gpio_clear(BATTERY_RESET_PORT, BATTERY_RESET_PIN);
    systick_delay_ms(10);
    gpio_set(BATTERY_RESET_PORT, BATTERY_RESET_PIN);
    systick_delay_ms(10);       /* let it come up before the first access */

    clk(0);
    dat(1);                     /* idle high */
}

static void bus_start(void)
{
    dat_output();
    clk(0);
    delay(1);
    dat(1);
    delay(1);
    clk(1);
    delay(1);
    dat(0);
    delay(1);
    clk(0);
}

static void bus_stop(void)
{
    clk(0);
    delay(1);
    dat(0);
    delay(1);
    clk(1);
    delay(1);
    dat(1);
    delay(1);
}

static bool bus_write_byte(uint8_t v)
{
    unsigned i, t;
    bool ack = false;

    dat_output();               /* never assume: before this we may have been listening */
    clk(0);
    for (i = 0; i < 8u; i++) {
        dat((v & 0x80u) ? 1 : 0);
        delay(4);
        clk(1);
        delay(4);
        clk(0);
        v = (uint8_t)(v << 1);
    }

    dat_input();                /* release the line and wait for the chip's ack */
    delay(4);
    clk(1);
    delay(4);
    for (t = 0; t < 250u; t++) {
        delay(1);
        if (!gpio_read(DAT_PORT, DAT_PIN)) {
            ack = true;
            break;
        }
    }
    clk(0);
    dat_output();
    delay(5);
    return ack;
}

static uint8_t bus_read_byte(void)
{
    uint8_t v = 0;
    unsigned i;

    dat_input();                /* the chip drives it; we only listen */
    delay(8);
    clk(0);
    for (i = 0; i < 8u; i++) {
        clk(1);
        delay(8);
        v = (uint8_t)(v << 1);
        if (gpio_read(DAT_PORT, DAT_PIN))
            v |= 1u;
        delay(8);
        clk(0);
        delay(8);
    }
    return v;
}

bool battery_read(uint8_t reg, uint16_t *value)
{
    uint8_t high, low;
    bool acked;

    bus_start();
    acked = bus_write_byte(0x80u);
    if (!bus_write_byte((uint8_t)(((reg & 0x7Fu) << 1) | 1u)))
        acked = false;

    /* First byte of the word is its high half, second the low half. */
    high = bus_read_byte();
    dat_output();               /* the master acknowledges: line low, one clock */
    delay(8);
    dat(0);
    delay(8);
    clk(1);
    delay(8);
    clk(0);

    low = bus_read_byte();
    dat_output();               /* last word: acknowledged with the line high */
    delay(8);
    dat(1);
    delay(8);
    clk(1);
    delay(8);
    clk(0);

    bus_stop();

    *value = (uint16_t)(((uint16_t)(high & 0x03u) << 8) | low);
    return acked;
}

bool battery_voltage_mv(uint32_t *mv)
{
    uint16_t raw, gain_reg;
    uint32_t offset;

    if (!battery_read(BATTERY_REG_VOLTAGE, &raw))
        return false;
    if (!battery_read(BATTERY_REG_GAIN, &gain_reg))
        return false;

    /* 0x3FF is every bit of the 10-bit field set: a floating or absent bus, not a
     * voltage.  (0 is left alone -- a flat pack is a real reading.) */
    if (raw == 0x3FFu)
        return false;

    /* The offset is selected by the top two bits of the second byte read. */
    switch ((gain_reg & 0xFFu) >> 6) {
    case 0:
        offset = 875u;
        break;
    case 1:
    case 2:
        offset = 760u;
        break;
    default:
        offset = 640u;
        break;
    }

    *mv = (uint32_t)(raw + offset) * 10u;   /* the stock returns (raw+offset)*10000 uV */
    return true;
}
