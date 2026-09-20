/* Companion gauge chip -- pack voltage and charger status.
 *
 * Everything here mirrors the stock firmware's own bit-bang, function by
 * function, because the framing is not quite standard I2C and nothing else
 * documents it.  Addresses are the stock V49 image's (base 0x08004000):
 *
 *   0x08006EF0  start: clock low, data high, clock high, data low, clock low
 *   0x08006F4C  stop:  clock low, data low,  clock high, data high
 *   0x0800705C  write a byte, MSB first, then release the line and wait up to
 *               250 polls for the chip to pull it low (its acknowledge)
 *   0x08006E78  read a byte, MSB first, sampled while the clock is high
 *   0x08007240  write a register: start, 0x80, (reg << 1) | 0, len bytes, stop
 *   0x08007158  read a register: start, 0x80, (reg << 1) | 1, 16-bit words
 *   0x08007284  write a 16-bit register: **high byte first**, then low
 *   0x0800D138  the data pin's direction: output (arg 1) or released (arg 0)
 *   0x0800D35C  the boot bring-up this file replays
 *   0x0802422A  the delay helper: a fixed loop of (n+1) x 21 -- a cycle count,
 *               not a time, so the absolute bus speed depends on the clock it
 *               was compiled for
 *
 * The stock runs from the PLL its bootloader leaves running (CR = 0x0040e583,
 * PLLON) while this firmware forces HSI at 8 MHz, so the same call sites give a
 * slower bus here.  That cannot be resolved by reading, so battery_init() replays
 * the stock's whole boot bring-up at each delay scaling and keeps the first one
 * the chip answers: one flash settles both the speed and the configuration.
 *
 * The bring-up itself is the stock's, not a guess.  The main routine (FUN_08024448)
 * calls FUN_080167F0 **once** before entering its poll loop; that reaches
 * FUN_0800D334(1) -> FUN_0800D35C, which (battery type non-zero) runs FUN_08007124
 * -- a 68-byte block into register 0, then register 0x32 twice -- and then the
 * write-back FUN_08006F9C.  Only after that does the poll FUN_08017BB4 read the
 * voltage, and it reads it with no configuration of its own.  This driver used to
 * replay only the write-back, so it was sending the tail of the sequence and
 * wondering why the chip stayed silent.
 */
#include "driver/battery.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/systick.h"

#define CLK_PORT BATTERY_CLK_PORT
#define CLK_PIN  BATTERY_CLK_PIN
#define DAT_PORT BATTERY_DATA_PORT
#define DAT_PIN  BATTERY_DATA_PIN

/* ---------------------------------------------------------------- timing */

/* The stock's own delay helper (0x0802422A): (n+1) x scale iterations, with the
 * inner counter volatile so the compiler cannot drop the loop.  The scale is the
 * bus speed, and battery_init() finds it by replaying the bring-up at each one. */
static unsigned s_scale = 21u;              /* the stock's own inner count */

/* Deliberately unsized and counted with sizeof below: the previous array was
 * declared with a count of five and held six entries, so the compiler silently
 * dropped the zero-delay entry and the fastest end of the sweep -- the end the
 * gauge was most likely waiting for -- was never actually tried. */
static const unsigned scales[] = { 21u, 8u, 4u, 2u, 1u, 0u };

unsigned battery_bus_scale_count(void)
{
    return (unsigned)(sizeof scales / sizeof scales[0]);
}

static void delay(unsigned n)
{
    for (unsigned i = 0; i <= n; i++)
        for (volatile unsigned j = 0; j < s_scale; j++)
            ;
}

/* ------------------------------------------------------------------ pins */

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

/* ------------------------------------------------------------- the bus */

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

/* A register write, as FUN_08007240 does it: start, 0x80, the register as an
 * address byte with rw = 0, then len bytes, then the stop and the clock low. */
static bool bus_write_reg(uint8_t reg, unsigned len, const uint8_t *buf)
{
    unsigned i;
    bool acked = bus_write_byte(0x80u);

    if (!bus_write_byte((uint8_t)((reg & 0x7Fu) << 1)))
        acked = false;
    for (i = 0; i < len; i++)
        if (!bus_write_byte(buf[i]))
            acked = false;
    bus_stop();
    clk(0);
    dat(1);
    return acked;
}

/* A 16-bit register write, as FUN_08007284 packs it -- the high byte first:
 * 0800728a  asrs r0,r4,#8 ; strb r0,[sp,#0] ; uxtb r0,r4 ; strb r0,[sp,#1].
 * The low-first order this driver used before was a byte-swapped write. */
static bool bus_write_reg16(uint8_t reg, uint16_t value)
{
    uint8_t b[2];

    b[0] = (uint8_t)(value >> 8);
    b[1] = (uint8_t)(value & 0xFFu);
    return bus_write_reg(reg, sizeof b, b);
}

/* One 16-bit register read (FUN_08007158): the master drives the acknowledge
 * after the first byte and drives it **high** after the last one. */
static bool bus_read_reg16(uint8_t reg, uint16_t *value)
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

    *value = (uint16_t)(((uint16_t)high << 8) | low);
    return acked;
}

bool battery_read(uint8_t reg, uint16_t *value)
{
    return bus_read_reg16(reg, value);
}

/* --------------------------------------------- the boot bring-up, staged */

/* The 68 bytes the stock writes into gauge register 0 (FUN_08007124).  They are
 * .data in the stock image: the C runtime expands them from a compressed block at
 * flash 0x08027748 (scatter table 0x08027728, decompressor FUN_0800483A) into RAM
 * 0x20000088, and FUN_08007240 sends that buffer.  Recovered by running that
 * decompressor: it decodes to exactly the 844 bytes the table asks for and stops
 * exactly where the next block begins, and the 68 bytes are identical in the
 * RA89R V49 and RA89G V52 builds, which otherwise differ by 83% of their bytes.
 * Nothing else in the image references the buffer, so this is its boot value. */
static const uint8_t gauge_config_block[0x44] = {
    0x00, 0x08, 0x10, 0x80, 0x02, 0x01, 0x00, 0x00,
    0x40, 0xC0, 0x0A, 0x5D, 0x00, 0x2E, 0x02, 0xFF,
    0x5B, 0x11, 0x00, 0x00, 0x41, 0x1E, 0x00, 0x00,
    0xCE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x31, 0x97, 0x00, 0x00, 0x13, 0xFF, 0x98, 0x52,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x51, 0xE1, 0x28, 0xDC, 0x26, 0x45, 0x00, 0xE4,
    0x1C, 0xD8, 0x3A, 0x50, 0xEA, 0xF0, 0x30, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

#define STAGE_COUNT 8

static const char *const stage_names[STAGE_COUNT] = {
    "reg0 68B",
    "reg0x32 285C",
    "reg0x32 28DC",
    "wb reg5",
    "wb reg3",
    "wb reg3 8000",
    "en reg3 ~80",
    "en reg2 |7",
};

static bool stage_ok[STAGE_COUNT];
static bool s_bus_ok;

unsigned battery_stage_count(void) { return STAGE_COUNT; }

const char *battery_stage_name(unsigned index)
{
    return index < STAGE_COUNT ? stage_names[index] : "?";
}

bool battery_stage_ok(unsigned index)
{
    return index < STAGE_COUNT ? stage_ok[index] : false;
}

unsigned battery_stage_acks(void)
{
    unsigned n = 0, i;

    for (i = 0; i < STAGE_COUNT; i++)
        if (stage_ok[i])
            n++;
    return n;
}

bool battery_bus_ok(void)
{
    return s_bus_ok;
}

/* The stock's boot sequence for a non-zero battery type (FUN_0800D35C and the
 * enable FUN_0800D1F8 performs), in its own order, with each write recorded
 * separately so the console can say which one -- if any -- the chip answers.
 * uv is the pack voltage the stock programs in the write-back; the stock gets it
 * from FUN_0800F440 *before* the gauge is readable, so a known-good constant is
 * what it does too. */
static bool gauge_bringup(uint32_t uv)
{
    uint32_t count = uv / 10000u;
    uint32_t off;
    uint16_t sel, n, cap, v;
    unsigned i = 0, k;

    for (k = 0; k < STAGE_COUNT; k++)
        stage_ok[k] = false;

    systick_delay_ms(30);                   /* FUN_080241F8(0x1e) */

    /* FUN_08007124: the 68-byte block into register 0, ... */
    stage_ok[i++] = bus_write_reg(0, sizeof gauge_config_block, gauge_config_block);
    delay(100);
    stage_ok[i++] = bus_write_reg16(0x32u, 0x285Cu);
    delay(100);
    stage_ok[i++] = bus_write_reg16(0x32u, 0x28DCu);

    /* ... then the write-back FUN_08006F9C: the measured voltage in register 5
     * (the gain byte picks the per-battery offset) and twice in register 3. */
    if (count < 0x2F8u) {
        off = 0x280u;
        sel = 0xC0u | 0x1Fu;
    } else {
        off = 0x2F8u;
        sel = 0x40u | 0x1Fu;
    }
    n = (uint16_t)(count - off);
    cap = (uint16_t)((((n >> 8) & 3u) << 8) | (n & 0xFFu));
    stage_ok[i++] = bus_write_reg16(5u, (uint16_t)((10u << 8) | sel));
    stage_ok[i++] = bus_write_reg16(3u, cap);
    stage_ok[i++] = bus_write_reg16(3u, (uint16_t)(cap | 0x8000u));

    /* FUN_0800D1F8's enable: clear bit 7 of register 3, then set bits 0-2 of
     * register 2 (FUN_080069A6 / FUN_08006952), each a read-modify-write. */
    if (bus_read_reg16(3u, &v))
        v = (uint16_t)(v & (uint16_t)~0x80u);
    else
        v = 0u;
    stage_ok[i++] = bus_write_reg16(3u, v);

    if (bus_read_reg16(2u, &v))
        v = (uint16_t)(v | 7u);
    else
        v = 0u;
    stage_ok[i++] = bus_write_reg16(2u, v);

    return battery_stage_acks() != 0u;
}

/* ------------------------------------------------------------ the pins */

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

/* PC14 is OSC32_IN, so the LSE oscillator would own it if it were running.  This
 * firmware does not configure it and neither does the stock app, which drives
 * PC14 perfectly well -- so the LSE is off, and turning it off "just in case" was
 * fixing something that was not broken.  It is *reported* instead: if the pin ever
 * tests as not-ours, this is the first thing to look at.
 */
static bool s_lse_on;

bool battery_lse_on(void)
{
    return s_lse_on;
}

static bool s_clk_ok;
static bool s_data_ok;

bool battery_clk_pin_ok(void) { return s_clk_ok; }
bool battery_data_pin_ok(void) { return s_data_ok; }

void battery_init(void)
{
    unsigned i;

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

    /* The gauge's reset line, PD0.  The stock drives it low, waits and releases
     * it high (FUN_0801D69C: FUN_08011B74(GPIOD, 1, 0), FUN_080241F8(10),
     * ... FUN_08011B74(GPIOD, 1)), instead of leaving the pin alone, and a
     * floating reset input is the obvious reason a gauge would answer nothing. */
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

    if (!s_clk_ok || !s_data_ok)
        return;                 /* a bus pin is not ours; no speed will help */

    /* The stock configures the gauge once at boot, before the first poll, and
     * never reads it unconfigured.  A probe that only sends the address byte
     * cannot tell "wrong speed" from "not configured yet", so it is the whole
     * bring-up that is swept -- first speed that answers wins. */
    for (i = 0; i < battery_bus_scale_count() && !s_bus_ok; i++) {
        s_scale = scales[i];
        if (gauge_bringup(8100000u))    /* 8.1 V, this radio's own reading */
            s_bus_ok = true;
    }
}

bool battery_voltage_mv(uint32_t *mv)
{
    uint16_t raw, gain_reg;
    uint32_t offset;

    if (!bus_read_reg16(BATTERY_REG_VOLTAGE, &raw))
        return false;
    if (!bus_read_reg16(BATTERY_REG_GAIN, &gain_reg))
        return false;

    /* The stock takes the 10-bit field: FUN_0800687C builds it as
     * (high & 3) << 8 | low, i.e. the word with its top six bits dropped.  0x3FF
     * is every bit of that field set -- a floating or absent bus, not a voltage.
     * (0 is left alone: a flat pack is a real reading.) */
    raw = (uint16_t)(raw & 0x3FFu);
    if (raw == 0x3FFu)
        return false;

    /* The offset is selected by the top two bits of register 5's low byte. */
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
