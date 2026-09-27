/* External SPI NOR flash -- the radio's "EEPROM".  See spi_flash.h.
 *
 * Bit-banged on SPI1's remapped pins.  A hardware SPI1 setup would be faster,
 * but at 8 MHz this already runs at roughly 1 MHz and the UART is the bottleneck
 * for a dump by two orders of magnitude, so there is nothing to win and one more
 * peripheral (plus its remap and DMA) to get wrong.
 */
#include "driver/spi_flash.h"

#include "board.h"
#include "driver/gpio.h"

#define CS_PORT   SPI_FLASH_CS_PORT
#define CS_PIN    SPI_FLASH_CS_PIN
#define SCK_PORT  SPI_FLASH_SCK_PORT
#define SCK_PIN   SPI_FLASH_SCK_PIN
#define MISO_PORT SPI_FLASH_MISO_PORT
#define MISO_PIN  SPI_FLASH_MISO_PIN
#define MOSI_PORT SPI_FLASH_MOSI_PORT
#define MOSI_PIN  SPI_FLASH_MOSI_PIN

static void cs(int level)   { gpio_write(CS_PORT, CS_PIN, level); }
static void sck(int level)  { gpio_write(SCK_PORT, SCK_PIN, level); }
static void mosi(int level) { gpio_write(MOSI_PORT, MOSI_PIN, level); }

/* One byte, MSB first, mode 0: MOSI changes while the clock is low, MISO is
 * sampled on the rising edge. */
static uint8_t xfer(uint8_t out)
{
    uint8_t in = 0;
    unsigned i;

    for (i = 0; i < 8u; i++) {
        mosi((out & 0x80u) ? 1 : 0);
        out = (uint8_t)(out << 1);
        sck(1);
        in = (uint8_t)(in << 1);
        if (gpio_read(MISO_PORT, MISO_PIN))
            in |= 1u;
        sck(0);
    }
    return in;
}

void spi_flash_init(void)
{
    gpio_port_clock(CS_PORT);
    gpio_port_clock(SCK_PORT);
    gpio_port_clock(MISO_PORT);
    gpio_port_clock(MOSI_PORT);

    gpio_config_output(CS_PORT, CS_PIN);
    gpio_config_output(SCK_PORT, SCK_PIN);
    gpio_config_output(MOSI_PORT, MOSI_PIN);
    gpio_config_input(MISO_PORT, MISO_PIN);

    cs(1);                      /* deselected, clock low (mode 0 idle) */
    sck(0);
    mosi(0);
}

bool spi_flash_id(uint16_t *man_dev, uint32_t *jedec)
{
    uint8_t a, b, c;

    /* 0x90: manufacturer + device id, addressed (Winbond-compatible). */
    cs(0);
    xfer(0x90u);
    xfer(0x00u);
    xfer(0x00u);
    xfer(0x00u);
    a = xfer(0xFFu);
    b = xfer(0xFFu);
    cs(1);
    if (man_dev)
        *man_dev = (uint16_t)(((uint16_t)a << 8) | b);

    /* 0x9F: JEDEC id, which also carries the capacity. */
    cs(0);
    xfer(0x9Fu);
    a = xfer(0xFFu);
    b = xfer(0xFFu);
    c = xfer(0xFFu);
    cs(1);
    if (jedec)
        *jedec = ((uint32_t)a << 16) | ((uint32_t)b << 8) | c;

    return !(a == 0xFFu && b == 0xFFu && c == 0xFFu);
}

uint32_t spi_flash_size(uint32_t jedec)
{
    uint32_t capacity = jedec & 0xFFu;

    if (capacity < 0x11u || capacity > 0x1Cu)   /* 2 KB .. 256 MB */
        return 0u;
    return 1u << capacity;
}

void spi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    cs(0);
    xfer(0x03u);                /* read data */
    xfer((uint8_t)(addr >> 16));
    xfer((uint8_t)(addr >> 8));
    xfer((uint8_t)addr);
    while (len--)
        *buf++ = xfer(0xFFu);
    cs(1);
}

/* ---------------------------------------------------------------------------
 * Writing.
 *
 * The read path above is validated on the radio (docs/ra89r_eeprom.md); this half is
 * new and is what the pending write test in that document exercises, so it stays
 * conservative: explicit write-enable before every erase or program, and a
 * status-register poll for WIP after each one.  A program never crosses a page
 * boundary -- the chip wraps within the page if the host does not split -- and
 * nothing here erases on its own: the caller erases first, which is what the
 * port's settings save does.
 * ------------------------------------------------------------------------- */

#define CMD_WRITE_ENABLE   0x06u
#define CMD_READ_STATUS    0x05u
#define CMD_PAGE_PROGRAM   0x02u
#define CMD_SECTOR_ERASE   0x20u
/* SPI_SPI_FLASH_SECTOR_SIZE is in the header; the page size is local. */
#define SPI_FLASH_PAGE_SIZE    256u

static void cmd_only(uint8_t command)
{
    cs(0);
    xfer(command);
    cs(1);
}

static void addr_command(uint8_t command, uint32_t addr)
{
    cs(0);
    xfer(command);
    xfer((uint8_t)(addr >> 16));
    xfer((uint8_t)(addr >> 8));
    xfer((uint8_t)addr);
    cs(1);
}

static uint8_t status_read(void)
{
    uint8_t status;

    cs(0);
    xfer(CMD_READ_STATUS);
    status = xfer(0xFFu);
    cs(1);
    return status;
}

/* The chip holds WIP (status bit 0) while an erase or program runs. */
static void wait_ready(void)
{
    while ((status_read() & 0x01u) != 0u)
        ;
}

void spi_flash_write_enable(void)
{
    cmd_only(CMD_WRITE_ENABLE);
}

void spi_flash_wait_ready(void)
{
    wait_ready();
}

void spi_flash_program(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    while (len > 0u) {
        uint32_t room = SPI_FLASH_PAGE_SIZE - (addr & (SPI_FLASH_PAGE_SIZE - 1u));
        uint32_t chunk = (len < room) ? len : room;
        uint32_t i;

        wait_ready();
        spi_flash_write_enable();
        cs(0);
        xfer(CMD_PAGE_PROGRAM);
        xfer((uint8_t)(addr >> 16));
        xfer((uint8_t)(addr >> 8));
        xfer((uint8_t)addr);
        for (i = 0; i < chunk; i++)
            xfer(buf[i]);
        cs(1);

        addr += chunk;
        buf += chunk;
        len -= chunk;
    }
    wait_ready();
}

void spi_flash_sector_erase(uint32_t addr)
{
    wait_ready();
    spi_flash_write_enable();
    addr_command(CMD_SECTOR_ERASE, addr);
    wait_ready();
}
