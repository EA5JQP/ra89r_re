/* Bit-banged I2C for the BK1080 -- see i2c_bus.h for the pins and the phases. */
#include "driver/i2c_bus.h"

#include "board.h"
#include "driver/gpio.h"

#define SCL_PORT BK1080_SCL_PORT
#define SCL_PIN  BK1080_SCL_PIN
#define SDA_PORT BK1080_SDA_PORT
#define SDA_PIN  BK1080_SDA_PIN

uint8_t gI2cBusDelay = I2C_BUS_DELAY_DEFAULT;

void i2c_bus_set_delay(uint8_t iterations)
{
    if (iterations > 0u)
        gI2cBusDelay = iterations;
}

uint8_t i2c_bus_delay_setting(void)
{
    return gI2cBusDelay;
}

static void i2c_delay(void)
{
    unsigned n = gI2cBusDelay;

    while (n-- != 0u)
        __NOP();
}

static void scl(int level) { gpio_write(SCL_PORT, SCL_PIN, level); }
static void sda(int level) { gpio_write(SDA_PORT, SDA_PIN, level); }

/* The stock reconfigures PB2 for direction (`FUN_0800D138`), speed 2, no pull:
 * mode 1 = output, mode 0 = input. */
static void sda_output(void) { gpio_config_output_ospeed(SDA_PORT, SDA_PIN, 2u); }
static void sda_input(void)  { gpio_config_input(SDA_PORT, SDA_PIN); }

void i2c_bus_init(void)
{
    gpio_port_clock(SCL_PORT);
    gpio_port_clock(SDA_PORT);

    gpio_config_output_ospeed(SCL_PORT, SCL_PIN, 2u);
    gpio_config_output_ospeed(SDA_PORT, SDA_PIN, 2u);

    scl(0);
    sda(1);
}

void i2c_bus_start(void)
{
    scl(0); i2c_delay();
    sda(1); i2c_delay();
    scl(1); i2c_delay();
    sda(0); i2c_delay();
    scl(0); i2c_delay();
}

void i2c_bus_stop(void)
{
    scl(0); i2c_delay();
    sda(0); i2c_delay();
    scl(1); i2c_delay();
    sda(1); i2c_delay();
}

bool i2c_bus_write_byte(uint8_t value)
{
    unsigned i;
    bool ack = false;

    for (i = 0; i < 8u; i++) {
        sda((value & 0x80u) ? 1 : 0); i2c_delay();
        scl(1); i2c_delay();
        scl(0); i2c_delay();
        value = (uint8_t)(value << 1);
    }

    /* Release the line and let the target pull it low for the acknowledge.  The
     * stock polls up to 250 times before giving up (`FUN_0800705C`). */
    sda_input(); i2c_delay();
    scl(1); i2c_delay();
    for (i = 0; i < 250u; i++) {
        i2c_delay();
        if (!gpio_read(SDA_PORT, SDA_PIN)) {
            ack = true;
            break;
        }
    }
    scl(0); i2c_delay();
    sda_output(); i2c_delay();

    return ack;
}

uint8_t i2c_bus_read_byte(void)
{
    uint8_t value = 0;
    unsigned i;

    sda_input(); i2c_delay();
    scl(0); i2c_delay();

    for (i = 0; i < 8u; i++) {
        scl(1); i2c_delay();
        value = (uint8_t)(value << 1);
        if (gpio_read(SDA_PORT, SDA_PIN))
            value |= 1u;
        i2c_delay();
        scl(0); i2c_delay();
    }

    return value;
}

void i2c_bus_send_ack(bool ack)
{
    sda_output(); i2c_delay();
    sda(ack ? 0 : 1); i2c_delay();
    scl(1); i2c_delay();
    scl(0); i2c_delay();
}
