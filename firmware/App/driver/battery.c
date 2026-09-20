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
 * i.e. a cycle count, not a time -- so its absolute bus speed depends on the clock
 * it was compiled for, and this firmware's 8 MHz puts the same loop about nine
 * times slower (~8 kHz instead of ~70 kHz).  Since that cannot be resolved by
 * reading, the bus *calibrates itself*: battery_init() probes the first command
 * byte at a range of delay scalings and keeps the first one the chip acknowledges,
 * so one flash settles the speed instead of one guess per flash.
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

/* The stock's own delay helper (0x0802422A): (n+1) x scale iterations, with the
 * inner counter volatile so the compiler cannot drop the loop (as it did not in
 * the stock image).  The scale is the bus speed and is found at init. */
static unsigned s_scale = 21u;              /* the stock's own inner count */

static const unsigned scales[BATTERY_SCALE_COUNT] = { 21u, 8u, 4u, 2u, 1u, 0u };
/* The last entry is zero delay: the transfer's own code overhead is what bounds
 * the fastest rate (~50 kHz at 8 MHz, comparable to the stock's ~57 kHz at the PLL
 * clock its compiler assumed).  If the gauge has a maximum clock-low time it will
 * only answer at that end, and this is the entry that reaches it. */

static bool s_bus_ok;
static bool s_clk_ok;
static bool s_data_ok;
static unsigned s_bringup_acks;

static void bus_start(void);
static void bus_stop(void);
static bool bus_write_byte(uint8_t v);
static bool bus_write_reg16(uint8_t reg, uint16_t value);
static void gauge_bringup(uint32_t uv);

static void delay(unsigned n)
{
    for (unsigned i = 0; i <= n; i++)
        for (volatile unsigned j = 0; j < s_scale; j++)
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

/* Probe the bus: send the first command byte at each scaling until the chip
 * acknowledges.  This is the speed calibration -- the only unknown the stock's
 * code does not settle, because its delay is a cycle count rather than a time. */
static void bus_probe(void)
{
    unsigned round, i;

    for (round = 0; round < 2 && !s_bus_ok; round++) {
        for (i = 0; i < BATTERY_SCALE_COUNT; i++) {
            s_scale = scales[i];
            bus_start();
            s_bus_ok = bus_write_byte(0x80u);
            bus_stop();
            if (s_bus_ok)
                break;
            delay(20);
        }
        if (!s_bus_ok)
            systick_delay_ms(200);      /* a gauge can still be booting */
    }
}

unsigned battery_bus_scale(void)
{
    return s_scale;
}

/* What the fastest scaling really achieves, measured rather than assumed: the log
 * then says whether the sweep covers the stock's own rate.  Counts clock cycles
 * for a bounded window, so it costs a few milliseconds at boot. */
static unsigned s_rate_khz;

unsigned battery_bus_rate_khz(void)
{
    return s_rate_khz;
}

static void measure_bus_rate(void)
{
    unsigned save = s_scale;
    uint32_t t0, t1;
    unsigned cycles = 0;

    s_scale = 0;
    t0 = systick_millis();
    do {
        clk(1);
        clk(0);
        cycles++;
        t1 = systick_millis();
    } while (cycles < 1000u && (t1 - t0) < 20u);

    if (t1 > t0)
        s_rate_khz = (unsigned)((cycles / (t1 - t0)) / 2u);   /* two toggles per cycle */
    s_scale = save;
    clk(0);
}

bool battery_bus_ok(void)
{
    return s_bus_ok;
}

static bool pin_toggles(GPIO_TypeDef *port, uint32_t pin)
{
    unsigned lo, hi;

    gpio_clear(port, pin);
    delay(5);
    lo = gpio_read(port, pin) ? 1u : 0u;
    gpio_set(port, pin);
    delay(5);
    hi = gpio_read(port, pin) ? 1u : 0u;
    return lo == 0u && hi == 1u;
}

bool battery_clk_pin_ok(void) { return s_clk_ok; }
bool battery_data_pin_ok(void) { return s_data_ok; }

/* PC14 is OSC32_IN, so the LSE oscillator would own it if it were running.  This
 * firmware does not configure it and neither does the stock app, which drives
 * PC14 perfectly well -- so the LSE is off, and turning it off "just in case" was
 * fixing something that was not broken.  It is *reported* instead: if the pin ever
 * tests as not-ours, this is the first thing to look at, and the pin self-test
 * below is what detects it.
 */
static bool s_lse_on;

bool battery_lse_on(void)
{
    return s_lse_on;
}

void battery_init(void)
{
    s_lse_on = (RCC->BDCR & (RCC_BDCR_LSEON | RCC_BDCR_LSERDY)) != 0u;

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
    systick_delay_ms(100);      /* a gauge takes a moment to come up */

    clk(0);
    dat(1);                     /* idle high */

    /* Prove the pins are ours before blaming the chip: reading back an output
     * returns the level the pin is actually at. */
    s_clk_ok = pin_toggles(CLK_PORT, CLK_PIN);
    s_data_ok = pin_toggles(DAT_PORT, DAT_PIN);
    clk(0);
    dat(1);
    measure_bus_rate();

    if (s_clk_ok && s_data_ok) {
        gauge_bringup(8100000u);    /* 8.1 V, this radio's own reading */
        bus_probe();
    }
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

/* A 16-bit register write, as FUN_08007240 does it: the same start and 0x80, the
 * register as an address byte with rw = 0, then the two bytes low-first (which is
 * the packing FUN_08007284 does), then the stop and the clock low. */
static bool bus_write_reg16(uint8_t reg, uint16_t value)
{
    bool acked;

    bus_start();
    acked = bus_write_byte(0x80u);
    if (!bus_write_byte((uint8_t)((reg & 0x7Fu) << 1)))
        acked = false;
    if (!bus_write_byte((uint8_t)(value & 0xFFu)))
        acked = false;
    if (!bus_write_byte((uint8_t)(value >> 8)))
        acked = false;
    bus_stop();
    clk(0);
    dat(1);
    return acked;
}

unsigned battery_bringup_acks(void)
{
    return s_bringup_acks;
}

/* The write-back the stock does after every poll (FUN_08006F9C): the measured
 * voltage in register 5 (gain) and twice in register 3 (capacity, the second with
 * bit 15 set).  It is replayed here as a bring-up step, with the 8.1 V this
 * radio's stock firmware reports -- the stock gets its value for this from
 * FUN_0800F440, before the gauge is readable, so hardcoding a known-good one is
 * what it does too.  The point of the experiment is the acknowledge: if these
 * writes are answered, the bus and the write path work. */
static void gauge_bringup(uint32_t uv)
{
    uint32_t count = uv / 10000u;
    uint32_t off;
    uint16_t sel, n, v;

    if (count < 0x2F8u) {
        off = 0x280u;
        sel = 0xC0u | 0x1Fu;
    } else {
        off = 0x2F8u;
        sel = 0x40u | 0x1Fu;
    }
    n = (uint16_t)(count - off);

    s_bringup_acks = 0;
    if (bus_write_reg16(5, (uint16_t)((10u << 8) | sel)))
        s_bringup_acks++;
    v = (uint16_t)((((n >> 8) & 3u) << 8) | (n & 0xFFu));
    if (bus_write_reg16(3, v))
        s_bringup_acks++;
    if (bus_write_reg16(3, (uint16_t)(v | 0x8000u)))
        s_bringup_acks++;
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
    clk(0);                     /* the stock leaves the clock low between transfers
                                 * (FUN_08007158 drops it right after the stop) */
    dat(1);

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
