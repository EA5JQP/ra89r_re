/* Host test for the firmware's EEPROM console logic (App/driver/eeprom_console.c).
 *
 * The module talks only to the driver interfaces (driver/uart.h and
 * driver/spi_flash.h), so this test supplies fakes for both and runs the real
 * restore and write-validation code on a PC -- no radio, no SPI:
 *
 *     gcc -std=c11 -I App -I App/driver tools/test_eeprom_console.c \
 *         App/driver/eeprom_console.c -o /tmp/test_eeprom_console && /tmp/test_eeprom_console
 *
 * It checks the restore round-trip, the size guard, the checksum verdict and the
 * empty-sector write validation.  The PTY double (tools/ra89r_eeprom_sim.py)
 * tests the host tool's framing; this tests the C that runs on the radio.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/eeprom_console.h"
#include "driver/spi_flash.h"
#include "driver/uart.h"

#define CHIP_SIZE   0x120000u
#define PATTERN_END 0x110000u

static uint8_t g_chip[CHIP_SIZE];
static uint8_t g_in[CHIP_SIZE + 128u];
static size_t  g_in_len, g_in_pos;
static char    g_out[16384];
static size_t  g_out_len;
static int     g_fails;

/* ---- fake chip -------------------------------------------------------- */

void spi_flash_init(void) {}

bool spi_flash_id(uint16_t *man_dev, uint32_t *jedec)
{
    if (man_dev)
        *man_dev = 0x8514;
    if (jedec)
        *jedec = 0x852015;
    return true;
}

uint32_t spi_flash_size(uint32_t jedec) { (void)jedec; return CHIP_SIZE; }

void spi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    memcpy(buf, g_chip + addr, len);
}

void spi_flash_program(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    memcpy(g_chip + addr, buf, len);
}

void spi_flash_sector_erase(uint32_t addr)
{
    memset(g_chip + addr, 0xFF, 4096u);
}

/* ---- fake console ----------------------------------------------------- */

static void out_add(const char *s, size_t n)
{
    if (g_out_len + n < sizeof g_out) {
        memcpy(g_out + g_out_len, s, n);
        g_out_len += n;
        g_out[g_out_len] = '\0';
    }
}

void uart_putc(char c) { out_add(&c, 1u); }
void uart_puts(const char *s) { out_add(s, strlen(s)); }
void uart_write_raw(const char *buf, uint32_t len) { out_add(buf, len); }

void uart_printf(const char *fmt, ...)
{
    char tmp[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    out_add(tmp, strlen(tmp));
}

int uart_getc_timeout(uint32_t ms)
{
    (void)ms;
    if (g_in_pos < g_in_len)
        return g_in[g_in_pos++];
    return -1;
}

/* ---- harness ---------------------------------------------------------- */

static uint32_t checksum(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    size_t i;

    for (i = 0; i < n; i++)
        s += p[i];
    return s;
}

static void check(const char *name, bool ok)
{
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        g_fails++;
}

static void reset_io(void)
{
    g_in_len = g_in_pos = 0;
    g_out_len = 0;
    g_out[0] = '\0';
}

static void feed_header(uint32_t size, uint32_t sum)
{
    /* The console loop consumes the 'W'; eeprom_restore() reads what follows. */
    int n = snprintf((char *)g_in, sizeof g_in, " %u %08X\n",
                     (unsigned)size, (unsigned)sum);
    g_in_len = (size_t)n;
}

static void feed_append(const uint8_t *p, size_t n)
{
    memcpy(g_in + g_in_len, p, n);
    g_in_len += n;
}

static void init_chip(void)
{
    uint32_t i;

    for (i = 0; i < CHIP_SIZE; i++)
        g_chip[i] = (i < PATTERN_END) ? (uint8_t)(i * 7u + 0x11u) : 0xFFu;
}

int main(void)
{
    static uint8_t image[CHIP_SIZE];

    printf("test: restore round-trip\n");
    init_chip();
    {
        uint32_t i;
        for (i = 0; i < CHIP_SIZE; i++)
            image[i] = (uint8_t)(i * 3u + 0x42u);
    }
    reset_io();
    feed_header(CHIP_SIZE, checksum(image, CHIP_SIZE));
    feed_append(image, CHIP_SIZE);
    eeprom_restore();
    check("chip equals the image", memcmp(g_chip, image, CHIP_SIZE) == 0);
    check("verdict OK", strstr(g_out, "EEPROM RESTORE OK") != NULL);

    printf("test: restore refuses a wrong size\n");
    reset_io();
    feed_header(CHIP_SIZE - 4096u, 0u);
    {
        uint32_t before = checksum(g_chip, CHIP_SIZE);
        eeprom_restore();
        check("chip untouched", checksum(g_chip, CHIP_SIZE) == before);
    }
    check("ERR reported", strstr(g_out, "EEPROM RESTORE ERR") != NULL);

    printf("test: restore reports a bad checksum\n");
    reset_io();
    feed_header(CHIP_SIZE, checksum(image, CHIP_SIZE) ^ 0xFFFFFFFFu);
    feed_append(image, CHIP_SIZE);
    eeprom_restore();
    check("verdict FAIL", strstr(g_out, "EEPROM RESTORE FAIL") != NULL);

    printf("test: write validation on an empty sector\n");
    init_chip();
    reset_io();
    eeprom_write_validate();
    check("writetest PASS", strstr(g_out, "EEPROM WRITETEST PASS") != NULL);
    check("sector restored to 0xFF", g_chip[0x110000] == 0xFFu);

    printf("test: write validation refuses a chip with no empty sector\n");
    memset(g_chip, 0x00, sizeof g_chip);
    reset_io();
    eeprom_write_validate();
    check("writetest FAIL", strstr(g_out, "EEPROM WRITETEST FAIL") != NULL);

    printf("\n%s\n", g_fails ? "FAILURES" : "all checks passed");
    return g_fails ? 1 : 0;
}
