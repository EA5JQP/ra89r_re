/* The EEPROM console commands (see eeprom_console.h).  The CPS's offset map for
 * the chip's contents is in ra89r_findings.md; the calibration window that must
 * never be lost is in ra89r_codeplug.md / docs/ra89r_calibration.md. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/eeprom_console.h"
#include "driver/spi_flash.h"
#include "driver/uart.h"

static uint32_t eeprom_size(void)
{
    uint32_t jedec = 0;

    if (!spi_flash_id(NULL, &jedec))
        return 0u;
    return spi_flash_size(jedec);
}

void eeprom_report(void)
{
    uint16_t man_dev = 0;
    uint32_t jedec = 0;
    uint32_t size;
    bool present;

    spi_flash_init();
    present = spi_flash_id(&man_dev, &jedec);
    size = present ? spi_flash_size(jedec) : 0u;

    uart_printf("\neeprom: 0x90 id 0x%04X (the stock looks for the Winbond 0xEF16), "
                "0x9F jedec 0x%06X\n", (unsigned)man_dev, (unsigned)jedec);
    if (!present) {
        uart_puts("eeprom: no chip answered -- MISO stayed high, so nothing drove\n"
                  "        it.  Check the pins before reading anything into this.\n");
        return;
    }
    if (size == 0u) {
        uart_puts("eeprom: that is not a capacity byte this driver recognises\n");
        return;
    }
    uart_printf("eeprom: %u bytes (%u KB); 'E' dumps the whole chip as binary\n",
                (unsigned)size, (unsigned)(size / 1024u));
}

void eeprom_dump(void)
{
    static uint8_t buf[256];
    uint32_t size, addr, sum = 0;

    spi_flash_init();
    size = eeprom_size();
    if (size == 0u) {
        uart_puts("\neeprom: no chip answered; nothing to dump\n");
        return;
    }

    /* A header line the host parses, then exactly <size> raw bytes, then a
     * terminator carrying a weak checksum.  The loop runs to completion before
     * the main loop resumes, so neither the UI nor the heartbeat can interleave
     * into the middle of the stream.  At 115200 a 4 MB part takes ~6 minutes. */
    uart_printf("\nEEPROM DUMP %u\n", (unsigned)size);
    for (addr = 0; addr < size; addr += (uint32_t)sizeof buf) {
        uint32_t n = size - addr;
        uint32_t i;

        if (n > (uint32_t)sizeof buf)
            n = (uint32_t)sizeof buf;
        spi_flash_read(addr, buf, n);
        for (i = 0; i < n; i++)
            sum += buf[i];
        uart_write_raw((const char *)buf, n);       /* no CR/LF rewriting */
    }
    uart_printf("\nEEPROM END %08X\n", (unsigned)sum);
}

/* Parse the decimal size and hex checksum the host puts after 'W'. */
static uint32_t parse_u32(const char **p)
{
    uint32_t v = 0;

    while (**p == ' ')
        (*p)++;
    while (**p >= '0' && **p <= '9') {
        v = v * 10u + (uint32_t)(**p - '0');
        (*p)++;
    }
    return v;
}

static uint32_t parse_hex(const char **p)
{
    uint32_t v = 0;

    while (**p == ' ')
        (*p)++;
    for (;;) {
        char c = **p;

        if (c >= '0' && c <= '9')
            v = v * 16u + (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v = v * 16u + (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v = v * 16u + (uint32_t)(c - 'A' + 10);
        else
            break;
        (*p)++;
    }
    return v;
}

/* Restore the whole chip: the host sends "W <size> <sum>\n" (the 'W' is already
 * consumed by the console loop), then exactly <size> raw bytes.  Each 4 KB
 * sector is erased and programmed as it arrives; the checksum the host declared
 * is compared with what was received, so a damaged transfer is reported even
 * though the flash cannot be verified without another read.  Nothing here
 * touches the MCU's own flash. */
void eeprom_restore(void)
{
    static uint8_t buf[4096];
    char line[40];
    unsigned n = 0;
    int c;
    uint32_t size, declared, addr, i, sum = 0;
    const char *p;

    for (;;) {
        c = uart_getc_timeout(2000);
        if (c < 0) {
            uart_puts("\nEEPROM RESTORE ERR no-header\n");
            return;
        }
        if (c == '\n' || c == '\r')
            break;
        if (n < sizeof line - 1u)
            line[n++] = (char)c;
    }
    line[n] = '\0';

    p = line;
    size = parse_u32(&p);
    declared = parse_hex(&p);

    spi_flash_init();
    {
        uint32_t chip = eeprom_size();

        if (chip == 0u) {
            uart_puts("\nEEPROM RESTORE ERR no-chip\n");
            return;
        }
        if (size != chip) {
            uart_printf("\nEEPROM RESTORE ERR size %u, chip is %u\n",
                        (unsigned)size, (unsigned)chip);
            return;
        }
    }
    if (size == 0u || (size & 0xFFFu) != 0u) {
        uart_printf("\nEEPROM RESTORE ERR size %u is not whole 4 KB sectors\n",
                    (unsigned)size);
        return;
    }

    uart_printf("\nEEPROM RESTORE %u\n", (unsigned)size);

    /* One sector at a time, with a '.' after each.  The ack is not cosmetic:
     * the UART has no flow control and an erase takes tens of milliseconds, so
     * without it the host's next bytes would be lost while this core is busy
     * with the flash.  The 4 KB buffer is filled first (no flash access, so the
     * receive can keep up), then the sector is erased and programmed. */
    for (addr = 0; addr < size; addr += 4096u) {
        for (i = 0; i < 4096u; i++) {
            c = uart_getc_timeout(5000);
            if (c < 0) {
                uart_printf("\nEEPROM RESTORE FAIL %08X (timed out at %u)\n",
                            (unsigned)sum, (unsigned)(addr + i));
                return;
            }
            buf[i] = (uint8_t)c;
            sum += (uint8_t)c;
        }
        spi_flash_sector_erase(addr);
        spi_flash_program(addr, buf, 4096u);
        uart_putc('.');
    }

    uart_printf("\nEEPROM RESTORE %s %08X\n",
                (sum == declared) ? "OK" : "FAIL", (unsigned)sum);
}

/* One-shot validation that the write path works: find an empty (all-0xFF)
 * sector in the erased tail, write a pattern, read it back, compare, then erase
 * it again so the initial value is restored.  It refuses a sector that holds
 * anything, so the "initial value" is 0xFF and the restore is an erase -- it can
 * never damage real data.  Run it once, on a radio whose write path is unproven. */
void eeprom_write_validate(void)
{
    static uint8_t buf[4096];
    uint32_t size, addr, i;
    bool found = false;

    spi_flash_init();
    size = eeprom_size();
    if (size == 0u) {
        uart_puts("\nEEPROM WRITETEST FAIL no-chip\n");
        return;
    }

    for (addr = 0x110000u; addr + 4096u <= size; addr += 4096u) {
        spi_flash_read(addr, buf, 4096u);
        for (i = 0; i < 4096u; i++)
            if (buf[i] != 0xFFu)
                break;
        if (i == 4096u) {
            found = true;
            break;
        }
    }
    if (!found) {
        uart_puts("\nEEPROM WRITETEST FAIL no empty sector in the free tail\n");
        return;
    }
    uart_printf("\neeprom: write test on empty sector 0x%06X\n", (unsigned)addr);

    for (i = 0; i < 4096u; i++)
        buf[i] = (uint8_t)(i * 7u + 0x11u);
    spi_flash_sector_erase(addr);
    spi_flash_program(addr, buf, 4096u);

    spi_flash_read(addr, buf, 4096u);
    for (i = 0; i < 4096u; i++) {
        if (buf[i] != (uint8_t)(i * 7u + 0x11u)) {
            uart_printf("\nEEPROM WRITETEST FAIL read-back differs at 0x%06X\n",
                        (unsigned)(addr + i));
            spi_flash_sector_erase(addr);        /* restore anyway */
            return;
        }
    }

    spi_flash_sector_erase(addr);
    spi_flash_read(addr, buf, 4096u);
    for (i = 0; i < 4096u; i++) {
        if (buf[i] != 0xFFu) {
            uart_printf("\nEEPROM WRITETEST FAIL erase left 0x%02X at 0x%06X\n",
                        (unsigned)buf[i], (unsigned)(addr + i));
            return;
        }
    }

    uart_puts("eeprom: wrote 4096 bytes, read them back, erased to 0xFF\n");
    uart_puts("EEPROM WRITETEST PASS\n");
}
