/* The shared RF 3-wire bus -- see rf_bus.h for the pins and the timing. */
#include "driver/rf_bus.h"

#include "board.h"
#include "driver/gpio.h"

#define SCL_PORT BK_SCL_PORT
#define SCL_PIN  BK_SCL_PIN
#define SDA_PORT BK_SDA_PORT
#define SDA_PIN  BK_SDA_PIN
#define CS_PORT  BK4829_CS_PORT     /* both selects live on GPIOB */

/* The stock's delay helper (FUN_0802422A) is a (n + 1) x 21 cycle loop; the
 * K1's SHORT_DELAY() is 40 NOPs.  The part is good for far faster than either,
 * so this only keeps the edges clean rather than meeting a timing requirement. */
static void rf_delay(void)
{
    for (volatile unsigned i = 0; i < 40u; i++)
        ;
}

static void scl(int level) { gpio_write(SCL_PORT, SCL_PIN, level); }
static void sda(int level) { gpio_write(SDA_PORT, SDA_PIN, level); }
static void sel(uint32_t cs, int level) { gpio_write(CS_PORT, cs, level); }

static void bus_write_u8(uint8_t v)
{
    unsigned i;

    for (i = 0; i < 8u; i++) {
        rf_delay();
        sda((v & 0x80u) ? 1 : 0);
        rf_delay();
        scl(1);
        rf_delay();
        v = (uint8_t)(v << 1);
        scl(0);
    }
}

static uint16_t bus_read_u16(void)
{
    uint16_t v = 0;
    unsigned i;

    sda(1);                     /* release before handing the pin to the part */
    gpio_config_input(SDA_PORT, SDA_PIN);

    for (i = 0; i < 16u; i++) {
        scl(0);
        rf_delay();
        v = (uint16_t)(v << 1);
        if (gpio_read(SDA_PORT, SDA_PIN))
            v |= 1u;
        rf_delay();
        scl(1);
        rf_delay();
    }

    scl(0);
    gpio_config_output(SDA_PORT, SDA_PIN);
    sda(1);
    return v;
}

void rf_bus_init(void)
{
    gpio_port_clock(SCL_PORT);
    gpio_port_clock(SDA_PORT);
    gpio_port_clock(CS_PORT);

    gpio_config_output(SCL_PORT, SCL_PIN);
    gpio_config_output(SDA_PORT, SDA_PIN);
    gpio_config_output(CS_PORT, BK4829_CS_PIN | BK4815_CS_PIN);

    scl(0);
    sda(1);
    sel(BK4829_CS_PIN, 1);
    sel(BK4815_CS_PIN, 1);
}

void rf_bus_write(uint32_t cs, uint8_t addr, const uint8_t *data, unsigned len)
{
    unsigned i;

    scl(0);
    sel(cs, 0);
    sda(0);
    bus_write_u8(addr);
    for (i = 0; i < len; i++)
        bus_write_u8(data[i]);
    sel(cs, 1);
    scl(0);
    sda(1);
}

uint16_t rf_bus_read(uint32_t cs, uint8_t addr)
{
    uint16_t v;

    scl(0);
    sel(cs, 0);
    sda(0);
    bus_write_u8(addr);
    v = bus_read_u16();
    sel(cs, 1);
    scl(0);
    sda(0);
    return v;
}

/* ------------------------------------------------------------- manual mode */

void rf_bus_assert(uint32_t cs)
{
    scl(0);
    sel(cs, 0);
    sda(0);
}

void rf_bus_release(uint32_t cs)
{
    sel(cs, 1);
    scl(0);
    sda(1);
}

void rf_bus_delay(void)
{
    rf_delay();
}

void rf_bus_bit_out(int bit)
{
    rf_delay();
    sda(bit ? 1 : 0);
    rf_delay();
    scl(1);
    rf_delay();
    scl(0);
}

int rf_bus_bit_in(void)
{
    int v;

    scl(0);
    rf_delay();
    v = gpio_read(SDA_PORT, SDA_PIN) ? 1 : 0;
    rf_delay();
    scl(1);
    rf_delay();
    return v;
}
