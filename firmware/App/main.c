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
#include "driver/bk4815.h"
#include "driver/bk4829.h"
#include "driver/led.h"
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
              "          R probe both RF chips (ids)   W configure both\n");
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

/* --------------------------------------------------------------------- rf */

/* Both parts sit on the same bit-banged bus and each answers its own id in
 * register 0 -- that is the "version" read this test turns on.  A BK4829 must
 * read 0x4829 and a BK4815 0x4816; the stock's own detect is exactly that check
 * (FUN_08009772 / FUN_08009758), and a failure there is what makes it show its
 * error screen instead of configuring the part.
 *
 * 'R' writes nothing -- it is a read-only snapshot of both parts, so two things
 * are worth knowing about it:
 *
 *   - it can be pressed on a freshly booted radio before any configuration has
 *     been sent, and
 *   - the radio does not touch the RF pins at boot, so if the two chips keep
 *     their state across an MCU reset, 'R' shows what the *stock* firmware left
 *     in them.  That is the only way to read the stock's own values without a
 *     logic analyser, which matters for the three BK4815 registers the stock
 *     takes from its RAM and for the BK4829's computed register 0x7d.  If the
 *     chips reset with the MCU instead, the same read gives their power-on
 *     defaults -- also worth having as a baseline.
 *
 * 'W' replays both parts' stock boot configuration and re-probes, so a silent
 * bus, a deaf-but-configured chip and a live one look different. */
static void rf_dump(const char *name, uint8_t id_reg, const uint8_t *regs,
                    unsigned n, bool is_4815)
{
    unsigned i;

    uart_printf("  %s:", name);
    for (i = 0; i < n; i++) {
        uint16_t v = is_4815 ? bk4815_read_reg(regs[i]) : bk4829_read_reg(regs[i]);
        uart_printf(" %02X=%04X", (unsigned)regs[i], (unsigned)v);
    }
    uart_putc('\n');
    (void)id_reg;
}

static void rf_report(void)
{
    /* The registers worth watching: what our configuration writes (0x21, 0x24,
     * 0x30, 0x33, 0x47), the one the stock computes (0x7d), and the two the
     * stock reads while squelching (0x63, 0x67). */
    static const uint8_t bk4829_regs[] = { 0x21, 0x24, 0x30, 0x33, 0x47, 0x7d,
                                           0x63, 0x67 };
    /* Its band/mode words, plus the three the stock fills from its own RAM. */
    static const uint8_t bk4815_regs[] = { 0x0c, 0x75, 0x73, 0x4c, 0x55, 0x62 };
    uint16_t a, b;

    bk4829_init();              /* brings up the shared bus and both selects */

    uart_puts("\nRF bus: PA12 clock, PB12 data, PB8 = BK4829, PB13 = BK4815\n");

    a = bk4829_read_reg(BK4829_REG_ID);
    b = bk4815_read_reg(BK4815_REG_ID);

    uart_printf("  BK4829 reg 0x00 = 0x%04X  (expected 0x%04X) -- %s\n",
                (unsigned)a, (unsigned)BK4829_ID,
                a == BK4829_ID ? "present" : "not answering");
    uart_printf("  BK4815 reg 0x00 = 0x%04X  (expected 0x%04X) -- %s\n",
                (unsigned)b, (unsigned)BK4815_ID,
                b == BK4815_ID ? "present" : "not answering");

    rf_dump("BK4829", BK4829_REG_ID, bk4829_regs,
            (unsigned)(sizeof bk4829_regs / sizeof bk4829_regs[0]), false);
    rf_dump("BK4815", BK4815_REG_ID, bk4815_regs,
            (unsigned)(sizeof bk4815_regs / sizeof bk4815_regs[0]), true);

    if (a != BK4829_ID || b != BK4815_ID)
        uart_puts("  one id is wrong: check that part's select, and that both\n"
                  "  share PA12/PB12 -- a bus fault hits both, a select fault one.\n");
}

static void rf_configure(void)
{
    uart_puts("\nRF: replaying both boot configurations\n");

    bk4829_configure();
    uart_printf("  BK4829: %u writes sent;    reg 0 now 0x%04X\n",
                bk4829_config_writes(), (unsigned)bk4829_read_reg(BK4829_REG_ID));

    bk4815_configure();
    uart_printf("  BK4815: 1 block + %u writes sent;  reg 0 now 0x%04X\n",
                bk4815_config_writes(), (unsigned)bk4815_read_reg(BK4815_REG_ID));
    uart_printf("          %u of those writes went out as 0: the stock takes\n"
                "          0x4c/0x55/0x62 from its own RAM and we have no source.\n",
                bk4815_ram_sourced_writes());

    /* Read-back proves the register took the value and kept it -- not that the
     * stock sends it.  Press 'R' on a stock-booted radio for that. */
    uart_printf("  BK4829 reg 0x7d = 0x%04X after we wrote the derived 0xE958\n",
                (unsigned)bk4829_read_reg(0x7d));
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
            case 'R':
                rf_report();
                break;
            case 'W':
                rf_configure();
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
