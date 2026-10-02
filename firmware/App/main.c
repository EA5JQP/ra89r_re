/* Minimal RA89R bring-up firmware: screen (bit-banged ST7565 128x64) + UART.
 *
 * Console: USART1 on PB6/PB7 at 115200 8N1 -- the radio's programming port,
 * the same USART the stock bootloader uses.
 *
 * Commands (type them in the serial terminal):
 *   h  help            i  info        c  clear         t  test card
 *   d  dump screen as ASCII art        b  border toggle
 *   f  fill pattern    p  pixel sweep animation on/off
 *   v  contrast up     V  contrast down
 *   any other character is drawn on the display.
 */
#include <stdint.h>
#include <stddef.h>

#include "board.h"
#include "driver/backlight.h"
#include "driver/led.h"
#include "driver/clock.h"
#include "driver/fault.h"
#include "driver/gpio.h"
#include "driver/keypad.h"
#include "driver/lcd_st7565.h"
#include "driver/spi_flash.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "ui.h"

#define VERSION_STRING "ra89r_fw 0.2 (uart debug)"

/* The bootloader only starts this application while this byte is 0x11; it
 * clears it on entering update mode and the stock app writes it back
 * (ra89r_bootloader.md section 4c).  Reported at boot, and worth knowing about
 * when the screen stays dark. */
#define APP_VALID_MARKER  ((volatile uint8_t *)0x0805FFF0)

static uint8_t contrast = 0x19u;
static int show_border;
static int animate;
static int heartbeat = 1;
static int keypad_monitor;
static int keypad_last = -2;            /* force a first print when enabled */
static int keypad_ptt2_last = -1;
static int keypad_stock_last = -2;

/* ------------------------------------------------------------ keypad monitor */

/* Prints the raw ADC level of every keypad line plus the decoded key whenever
 * the result changes.  The raw value is the point: a press either lands inside
 * the window the decode table expects, or it shows exactly where it does not. */
static void keypad_monitor_step(void)
{
    const uint8_t *variants;
    KEY_Code_t key;
    int stock, ptt2;
    unsigned line;

    if (!keypad_monitor)
        return;

    key = keypad_poll();
    stock = keypad_stock_code();
    /* PTT2 moves no analog line, so it has to be part of the change test or a
     * press of it would print nothing at all. */
    ptt2 = keypad_ptt2_level() ? 1 : 0;
    if ((int)key == keypad_last && ptt2 == keypad_ptt2_last && stock == keypad_stock_last)
        return;
    keypad_last = (int)key;
    keypad_ptt2_last = ptt2;
    keypad_stock_last = stock;

    uart_printf("[k %ums]", (unsigned)systick_millis());
    for (line = 0; line < KEYPAD_LINE_COUNT; line++)
        uart_printf(" %s=0x%03X", keypad_line_name(line), (unsigned)keypad_raw(line));
    uart_printf(" PTT2=%d -> ", ptt2);

    if (key == KEY_INVALID && stock == KEYPAD_NONE) {
        uart_puts("nothing\n");
        return;
    }
    uart_printf("%s", keypad_name(key));
    if (key == KEY_INVALID) {
        /* Decoded from the ladder but not mapped to a K5V3 key yet: the stock
         * code is what identifies the button. */
        uart_printf(" (stock %s -- which button is it?)", keypad_stock_name(stock));
    } else if (stock != KEYPAD_NONE) {
        variants = keypad_variants(stock);
        uart_printf(" (stock 0x%02X", stock);
        if (variants && variants[1] != 0xFF)
            uart_printf(", held 0x%02X, long 0x%02X", variants[1], variants[2]);
        uart_puts(")");
    }
    uart_puts("\n");
}

/* ------------------------------------------------------------------ helpers */

static const char *reset_cause(void)
{
    uint32_t csr = RCC->CSR;

    if (csr & (1u << 25)) return "option byte loader";
    if (csr & (1u << 26)) return "pin reset";
    if (csr & (1u << 27)) return "power-on";
    if (csr & (1u << 28)) return "software (bootloader EXIT)";
    if (csr & (1u << 29)) return "independent watchdog";
    if (csr & (1u << 30)) return "window watchdog";
    if (csr & (1u << 31)) return "low-power";
    return "unknown";
}

static void print_diagnostics(void)
{
    uart_printf("  sysclk      %u Hz (HSI)\n", (unsigned)SystemCoreClock);
    uart_printf("  vtor        %08X\n", (unsigned)SCB->VTOR);
    uart_printf("  msp         %08X\n", (unsigned)__get_MSP());
    uart_printf("  reset cause %s (CSR=%08X)\n", reset_cause(),
                (unsigned)RCC->CSR);
    uart_printf("  app marker  0x0805FFF0 = %02X%s\n", (unsigned)*APP_VALID_MARKER,
                (*APP_VALID_MARKER == 0x11u) ? " (ok)"
                                             : " (NOT SET: the bootloader will "
                                               "not start this app)");
    uart_printf("  lcd         variant %s, column offset %u\n",
                (lcd_variant() == LCD_INIT_STOCK_APP) ? "stock-app (8 extra bytes)"
                                                      : "standard (bootloader-proven)",
                (unsigned)LCD_COLUMN_OFFSET);
    uart_printf("  uart        USART1 PB6/PB7 AF%u @ %u 8N1\n",
                (unsigned)BOARD_UART_AF, (unsigned)BOARD_UART_BAUD);
    uart_printf("  backlight   GPIOA pin 5, now %s\n",
                BACKLIGHT_IsOn() ? "on" : "off");
}

static void lcd_set_contrast(uint8_t value)
{
    contrast = value;
    lcd_write_cmd(0x81);
    lcd_write_cmd(contrast);
}

static void draw_test_card(void)
{
    ui_test_card();
    lcd_refresh();
}

static void dump_screen_ascii(void)
{
    uint16_t y;
    uint16_t x;

    uart_printf("\n+%s+\n", "----------------------------------------------------------------");
    for (y = 0; y < LCD_HEIGHT; y++) {
        uart_putc('|');
        for (x = 0; x < LCD_WIDTH; x++) {
            int on = (lcd_fb[y >> 3][x] >> (y & 7u)) & 1u;
            uart_putc(on ? '#' : '.');
        }
        uart_puts("|\n");
    }
    uart_printf("+%s+\n", "----------------------------------------------------------------");
}

static void print_info(void)
{
    uart_printf("\n%s\n", VERSION_STRING);
    uart_printf("  MCU        PY32F403xD, SYSCLK %u Hz (HSI)\n",
                (unsigned)SystemCoreClock);
    uart_printf("  flash      app linked at 0x%08X (stock bootloader keeps 0x08000000-0x08003FFF)\n",
                (unsigned)0x08004000u);
    uart_printf("  LCD        128x64 ST7565-family, bit-banged: SDA PB15, SCK PA8, DC PA10, CS PA11, RST PA9\n");
    uart_printf("  LCD column offset %u, contrast 0x%02X\n",
                (unsigned)LCD_COLUMN_OFFSET, contrast);
    uart_printf("  UART       USART1 PB6/PB7 AF%u @ %u baud\n",
                (unsigned)BOARD_UART_AF, (unsigned)BOARD_UART_BAUD);
    uart_printf("  fonts      8x16 (0x20-0x7A) and 5x7 (0x20-0x5A) lifted from the stock firmware\n");
}

static void print_help(void)
{
    uart_puts("\ncommands: h help   i diagnostics   d dump screen as ASCII\n"
              "          c clear  t test card   b border   f fill   p animation\n"
              "          v/V contrast up/down  l backlight on/off  L PA0/PA1 led test  q heartbeat\n"
              "          r re-init panel (standard, bootloader-proven)\n"
              "          s re-init panel (stock app variant, 8 extra bytes)\n"
              "          k keypad monitor (raw ADC per line + decoded key)\n"
              "          e eeprom id   E dump the whole eeprom as binary (~6 min)\n"
              "          W restore the whole eeprom (host: ra89r_eeprom.py restore)\n"
              "          Z write validation on an empty sector (run once)\n");
}

/* --------------------------------------------------------------- animation */

static void animate_step(uint32_t ms)
{
    static uint32_t last;
    static uint16_t pos;

    if (!animate || (uint32_t)(ms - last) < 40u)
        return;
    last = ms;

    ui_animate_bar(pos);
    pos = (uint16_t)(pos + 4u);
    if (pos > LCD_WIDTH - 26u)
        pos = 2u;
    lcd_refresh();
}

/* ---------------------------------------------------------------- eeprom */

/* The external SPI NOR flash: what the CPS calls the EEPROM.  'e' identifies it,
 * 'E' streams the whole chip as raw binary for tools/ra89r_eeprom.py to save.
 * The CPS's offset map for the contents is in ra89r_findings.md. */

static uint32_t eeprom_size(void)
{
    uint32_t jedec = 0;

    if (!spi_flash_id(NULL, &jedec))
        return 0u;
    return spi_flash_size(jedec);
}

static void eeprom_report(void)
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

static void eeprom_dump(void)
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
static void eeprom_restore(void)
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
static void eeprom_write_validate(void)
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

/* ------------------------------------------------------------------- main */

int main(void)
{
    uint32_t last_tick = 0;
    char echo[16];
    uint32_t echo_len = 0;

    /* Clock first: the UART divisor below depends on knowing it.  Everything
     * the bootloader left behind is captured for the log -- it is the only way
     * to see why a peripheral misbehaves after the hand-over. */
    {
        uint32_t left_cfgr;
        uint32_t left_cr;
        uint32_t left_flash_acr = FLASH->ACR;
        uint32_t u_cr1 = BOARD_UART->CR1;
        uint32_t u_cr2 = BOARD_UART->CR2;
        uint32_t u_cr3 = BOARD_UART->CR3;

        left_cfgr = clock_init();
        left_cr = RCC->CR;
        uart_init(BOARD_UART_BAUD);
        uart_printf("\n\n=== " VERSION_STRING " ===\n");
        uart_printf("clock: forced HSI; bootloader had left CFGR=%08X CR=%08X "
                    "FLASH_ACR=%08X\n", (unsigned)left_cfgr, (unsigned)left_cr,
                    (unsigned)left_flash_acr);
        uart_printf("usart1 as left by the bootloader: CR1=%04X CR2=%04X CR3=%04X "
                    "(now 8N1, no flow control, no DMA)\n",
                    (unsigned)u_cr1, (unsigned)u_cr2, (unsigned)u_cr3);
        uart_printf("sysclk %u Hz, APB1/APB2 %u Hz\n", (unsigned)SystemCoreClock,
                    (unsigned)BOARD_APB2_HZ);
    }
    uart_puts("uart console up (USART1, PB6/PB7, 115200 8N1)\n");

    systick_init();
    uart_printf("systick up, sysclk %u Hz\n", (unsigned)SystemCoreClock);

    uart_printf("built " __DATE__ " " __TIME__ "\n");
    uart_printf("reset cause: %s\n", reset_cause());
    uart_printf("app-valid marker at 0x0805FFF0 = 0x%02X (expected 0x11)\n",
                (unsigned)*APP_VALID_MARKER);
    if (*APP_VALID_MARKER != 0x11u) {
        uart_puts("  note: the bootloader started us anyway, so it saw 0x11; if\n"
                  "        this line disagrees, report it -- the marker write is\n"
                  "        then not landing where we read it.\n");
    }

    /* Keypad lines back to their default state before anything else touches
     * GPIO: the five ladder inputs (plus the ADC's other analog input) analog,
     * undriven and unpulled, and PTT2 a plain input.  A ladder line that is
     * driven or pulled has its level corrupted -- and sinks current through the
     * ladder -- so nothing else in this firmware may touch these pins. */
    gpio_config_analog(KEYPAD_ANALOG_A_PORT, KEYPAD_ANALOG_A_MASK);
    gpio_config_analog(KEYPAD_ANALOG_B_PORT, KEYPAD_ANALOG_B_MASK);
    gpio_config_input(KEYPAD_PTT2_PORT, KEYPAD_PTT2_PIN);
    uart_puts("keypad: PA2/PA3/PA6/PA7/PB0/PB1 analog, PB9 input (default state)\n");
    if (keypad_init())
        uart_printf("keypad: ADC scanning 6 channels through DMA (%u keypad lines), "
                    "PTT2 on PB9\n", (unsigned)KEYPAD_LINE_COUNT);
    else
        uart_puts("keypad: WARNING -- the ADC/DMA scan is NOT running; the key "
                  "monitor would report zeros for every line\n");

    uart_puts("lcd: reset + init (standard sequence, as the bootloader uses) ...\n");    lcd_init();
    uart_puts("lcd: init done\n");

    BACKLIGHT_Init();
    uart_puts("backlight: on (GPIOA pin 5 -- confirmed on the radio)\n");
    led_init();
    uart_puts("led: PA0/PA1 driven, nothing visible on this radio; "
              "'L' steps the test combinations\n");
    draw_test_card();
    uart_puts("lcd: test card drawn\n");
    uart_puts("boot complete. 'h' for commands, 'd' dumps the screen over this\n"
              "console, 'i' shows diagnostics.\n");
    print_help();

    for (;;) {
        uint32_t now = systick_millis();
        int c = uart_getc();

        if (c >= 0) {
            char ch = (char)c;

            /* echo and remember the last characters on the display */
            if (ch == '\r' || ch == '\n') {
                uart_puts("\n");
            } else {
                uart_printf("%c", ch);
            }

            switch (ch) {
            case 'h':
                print_help();
                break;
            case 'i':
                print_info();
                break;
            case 'c':
                ui_clear();
                lcd_refresh();
                uart_puts("\nscreen cleared\n");
                break;
            case 't':
                draw_test_card();
                uart_puts("\ntest card\n");
                break;
            case 'd':
                dump_screen_ascii();
                break;
            case 'b':
                show_border = !show_border;
                ui_border(show_border);
                lcd_refresh();
                break;
            case 'f':
                ui_pattern(0x55);
                lcd_refresh();
                uart_puts("\ncheckerboard\n");
                break;
            case 'p':
                animate = !animate;
                uart_printf("\nanimation %s\n", animate ? "on" : "off");
                break;
            case 'v':
                lcd_set_contrast((uint8_t)(contrast + 4u));
                uart_printf("\ncontrast 0x%02X\n", contrast);
                break;
            case 'V':
                lcd_set_contrast((uint8_t)(contrast - 4u));
                uart_printf("\ncontrast 0x%02X\n", contrast);
                break;
            case 'r':
                uart_printf("\npanel re-init (standard, bootloader-proven): %d\n",
                            lcd_reinit(LCD_INIT_STANDARD));
                lcd_set_contrast(contrast);
                draw_test_card();
                break;
            case 's':
                uart_printf("\npanel re-init (stock app variant): %d\n",
                            lcd_reinit(LCD_INIT_STOCK_APP));
                lcd_set_contrast(contrast);
                draw_test_card();
                break;
            case 'l':
                if (BACKLIGHT_IsOn()) {
                    BACKLIGHT_TurnOff();
                    uart_puts("\nbacklight off\n");
                } else {
                    BACKLIGHT_TurnOn();
                    uart_puts("\nbacklight on\n");
                }
                break;
            case 'L': {
                /* PA0/PA1 do nothing visible on this radio; this is here so that
                 * if they are ever identified, the test is one key away. */
                static const struct {
                    uint32_t mask;
                    const char *name;
                } test[] = {
                    { LED_PIN_B, "PA1" },
                    { LED_PIN_A, "PA0" },
                    { LED_PIN_A | LED_PIN_B, "PA0+PA1" },
                    { 0u, "off" },
                };
                static unsigned step;

                led_drive_pins(test[step].mask);
                uart_printf("\nled: %s\n", test[step].name);
                step = (step + 1u) % (sizeof(test) / sizeof(test[0]));
                break;
            }
            case 'q':
                heartbeat = !heartbeat;
                uart_printf("\nheartbeat %s\n", heartbeat ? "on" : "off");
                break;
            case 'k':
                keypad_monitor = !keypad_monitor;
                uart_printf("\nkeypad monitor %s -- press one button at a time\n",
                            keypad_monitor ? "on" : "off");
                keypad_last = -2;           /* force the next poll to print */
                break;
            case 'e':
                eeprom_report();
                break;
            case 'E':
                eeprom_dump();
                break;
            case 'W':
                eeprom_restore();
                break;
            case 'Z':
                eeprom_write_validate();
                break;
            default:
                break;
            }

            if (ch >= 0x20 && ch < 0x7F) {
                if (echo_len >= sizeof(echo) - 1u) {
                    uint32_t k;
                    for (k = 1; k < sizeof(echo) - 1u; k++)
                        echo[k - 1] = echo[k];
                    echo_len = sizeof(echo) - 2u;
                }
                echo[echo_len++] = ch;
                echo[echo_len] = '\0';
                ui_echo(echo);
                lcd_refresh();
            }
        } else {
            systick_delay_ms(1);
        }

        if ((uint32_t)(now - last_tick) >= 1000u) {
            last_tick = now;
            ui_status(now / 1000u);
            lcd_refresh();
            if (heartbeat && (now / 1000u) % 5u == 0u)
                uart_printf("[hb] uptime %us, panel variant %d, contrast 0x%02X\n",
                            (unsigned)(now / 1000u), lcd_variant(), contrast);
        }

        keypad_monitor_step();

        animate_step(now);
    }
}
