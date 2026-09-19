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

#include "board.h"
#include "driver/backlight.h"
#include "driver/clock.h"
#include "driver/fault.h"
#include "driver/gpio.h"
#include "driver/keypad.h"
#include "driver/lcd_st7565.h"
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

/* ------------------------------------------------------------ keypad monitor */

/* Prints the raw ADC level of every keypad line plus the decoded key whenever
 * the result changes.  The raw value is the point: a press either lands inside
 * the window the decode table expects, or it shows exactly where it does not. */
static void keypad_monitor_step(void)
{
    const uint8_t *variants;
    int code;
    unsigned line;

    if (!keypad_monitor)
        return;

    code = keypad_scan();
    if (code == keypad_last)
        return;
    keypad_last = code;

    uart_printf("[k %ums]", (unsigned)systick_millis());
    for (line = 0; line < KEYPAD_LINE_COUNT; line++)
        uart_printf(" %s=0x%03X", keypad_line_name(line), (unsigned)keypad_raw(line));
    uart_printf(" PTT2=%u -> %s", keypad_ptt2_level() ? 1u : 0u, keypad_name(code));

    variants = keypad_variants(code);
    if (variants && (variants[1] != 0xFF || variants[2] != 0xFF))
        uart_printf(" (long 0x%02X, extra 0x%02X)", variants[1], variants[2]);
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
    uart_printf("  backlight   GPIOA pin 1, now %s\n",
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
              "          v/V contrast up/down  l backlight on/off  q heartbeat\n"
              "          r re-init panel (standard, bootloader-proven)\n"
              "          s re-init panel (stock app variant, 8 extra bytes)\n"
              "          k keypad monitor (raw ADC per line + decoded key)\n");
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
    keypad_init();
    uart_printf("keypad: ADC up, %u analog lines (channels 2/3/6/7/8), PTT2 on PB9\n",
                (unsigned)KEYPAD_LINE_COUNT);

    uart_puts("lcd: reset + init (standard sequence, as the bootloader uses) ...\n");    lcd_init();
    uart_puts("lcd: init done\n");

    BACKLIGHT_Init();
    uart_puts("backlight: on (GPIOA pin 1 + pin 5, test)\n");
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
