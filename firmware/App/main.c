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
#include "driver/beeper.h"
#include "driver/bk4815.h"
#include "driver/battery.h"
#include "driver/bk1080.h"
#include "driver/i2c_bus.h"
#include "helper/battery.h"
#include "driver/bk4819.h"
#include "driver/bk4829.h"
#include "driver/audio_path.h"
#include "driver/led.h"
#include "driver/rx.h"
#include "functions.h"
#include "driver/clock.h"
#include "driver/fault.h"
#include "driver/gpio.h"
#include "driver/keypad.h"
#include "driver/pa.h"
#include "driver/rf_bus.h"
#include "driver/tx.h"
#include "driver/lcd_st7565.h"
#include "driver/st7565.h"
#include "driver/systick.h"
#include "driver/uart.h"
#include "driver/py25q16.h"
#include "app/app.h"
#include "app/common.h"
#include "app/scanner.h"
#include "helper/boot.h"
#include "misc.h"
#include "driver/py25q16.h"
#include "radio.h"
#include "ui/main.h"
#include "ui/menu.h"
#include "ui/status.h"
#include "ui/ui.h"
#include "ui/welcome.h"
#include "ui.h"

#define VERSION_STRING "ra89r_fw 0.2 (uart debug)"

/* Ask for a screen: the K1 application repaints when gUpdateDisplay is set
 * (app/app.c calls GUI_DisplayScreen). */
static void show_screen(GUI_DisplayType_t screen)
{
    gScreenToDisplay = screen;
    gUpdateDisplay = true;
}

/* 0x0805FFF0 is the bootloader's **update-mode request**, not an application
 * validity flag: 0xFF (the normal state) makes the bootloader start this
 * application, and 0x11 makes it enter update mode -- after which it consumes
 * the request by writing 0xFF back (docs/ra89r_bootloader.md section 4c).  The stock
 * application sets 0x11 from its serial command handler when the PC sends
 * "Reset" + '0'.  Reported at boot and by 'i'. */
#define UPDATE_REQUEST  ((volatile uint8_t *)0x0805FFF0)

static uint8_t contrast = 0x19u;
static int     bench_panel;   /* '0': hand the panel back to the bring-up screens */
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
    uart_printf("  sysclk      %u Hz (HSI x %u through the PLL)\n",
                (unsigned)SystemCoreClock, (unsigned)BOARD_PLL_MUL);
    uart_printf("  vtor        %08X\n", (unsigned)SCB->VTOR);
    uart_printf("  msp         %08X\n", (unsigned)__get_MSP());
    uart_printf("  reset cause %s (CSR=%08X)\n", reset_cause(),
                (unsigned)RCC->CSR);
    uart_printf("  update req  0x0805FFF0 = %02X%s\n", (unsigned)*UPDATE_REQUEST,
                (*UPDATE_REQUEST == 0x11u) ? " (set: the bootloader would enter "
                                             "update mode)"
                                           : " (clear: normal, the bootloader "
                                             "starts this app)");
    uart_printf("  lcd         variant %s, column offset %u\n",
                (lcd_variant() == LCD_INIT_STOCK_APP) ? "stock-app (8 extra bytes)"
                                                      : "standard (bootloader-proven)",
                (unsigned)LCD_COLUMN_OFFSET);
    uart_printf("  uart        USART1 PB6/PB7 AF%u @ %u 8N1\n",
                (unsigned)BOARD_UART_AF, (unsigned)BOARD_UART_BAUD);
    uart_printf("  backlight   GPIOA pin 5, now %s; BLTime %u, BLMin %u, BLMax %u, "
                "index %u, duty %u/32, pin %u\n",
                BACKLIGHT_IsOn() ? "on" : "off",
                (unsigned)gEeprom.BACKLIGHT_TIME, (unsigned)gEeprom.BACKLIGHT_MIN,
                (unsigned)gEeprom.BACKLIGHT_MAX, (unsigned)BACKLIGHT_GetBrightness(),
                BACKLIGHT_DutyOnCount(), gpio_read(GPIOA, BACKLIGHT_PIN) ? 1u : 0u);
    uart_printf("  backlight hw TIM7 CR1=%04X ARR=%u DIER=%04X; DMA1 ch2 CCR=%04X "
                "CNDTR=%u; SYSCFG CFGR2=%08X\n",
                (unsigned)TIM7->CR1, (unsigned)TIM7->ARR, (unsigned)TIM7->DIER,
                (unsigned)DMA1_Channel2->CCR, (unsigned)DMA1_Channel2->CNDTR,
                (unsigned)SYSCFG->CFGR[2]);
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
    uart_puts("\nthe K1 GUI owns the panel; these are console diagnostics\n"
              "          h help   i diagnostics   d dump screen as ASCII\n"
              "          m show the VFO/channel mode and switch it (K1: F then 3)\n"
              "          F cycle the chip-side VHF/UHF front end   B cycle the MCU PA0 level\n"
              "          D step the RF bus delay (find what still gives audio)\n"
              "          P time the loop's hot paths on this radio\n"
              "          q heartbeat   k keypad monitor   l backlight\n"
              "          u battery (pack on ADC channel 9 / PB1)\n"
              "          v/V contrast   L status led cycle (PA13/PA14)\n"
              "          Z beeper test tones (DAC tone on PA4)\n"
              "          R probe RF ids   W configure both   X verify config\n"
              "          K K1 bring-up + tune 145.7500   S sample reg 0x67\n"
              "          Q auto squelch: VHF 145.5000 then UHF 446.00625 (tinySA)\n"
              "          j BK1080 FM: init, id probe, tune 100.0 MHz, read status\n"
              "          a FM audio route: toggle the BK4829 AF mute (0x47) under FM\n"
              "          n BK4815 (PB13): boot config, tune 145.7500, read meters\n"
              "          T transmit (DTMF tone)   Y step the PA power   C toggle PC13\n"
              "          G VFO screen   2 VFO   3 menu   M menu   4 boot screen\n"
              "          1 back to the K1 GUI\n"
              "          5 save settings   6 flash write test   e flash dump\n"
              "          0 hand the panel to the bring-up screens (again: back)\n"
              "          t test card   b border   f fill   p animation   c clear\n"
              "          r/s panel re-init (standard / stock-app variant)\n");
}

/* What the two VFOs are actually on, and the state the VFO/channel switch
 * depends on.  `m` prints this before and after the switch. */
static void print_mode(void)
{
    const uint16_t a = gEeprom.ScreenChannel[0];
    const uint16_t b = gEeprom.ScreenChannel[1];

    uart_printf("  TX_VFO %u  screen A %u %s  screen B %u %s\n",
                (unsigned)gEeprom.TX_VFO,
                (unsigned)a, IS_MR_CHANNEL(a) ? "(channel)" :
                              (IS_FREQ_CHANNEL(a) ? "(frequency)" : "(?)"),
                (unsigned)b, IS_MR_CHANNEL(b) ? "(channel)" :
                              (IS_FREQ_CHANNEL(b) ? "(frequency)" : "(?)"));
    uart_printf("  FreqChannel A %u B %u  MrChannel A %u B %u  VFO_OPEN %u\n",
                (unsigned)gEeprom.FreqChannel[0], (unsigned)gEeprom.FreqChannel[1],
                (unsigned)gEeprom.MrChannel[0], (unsigned)gEeprom.MrChannel[1],
                (unsigned)gEeprom.VFO_OPEN);
    uart_printf("  front end %s (%s, PA0 %s, reg 0x33 0x%04X)\n",
                pa_band_is_uhf() ? "UHF" : "VHF",
                pa_band_is_main() ? ">134 MHz" : "<=134 MHz",
                pa_band_pa0_high() ? "high" : "low",
                (unsigned)pa_chip_path_reg());
    uart_printf("  A %u.%05u MHz  B %u.%05u MHz  CHANNEL_SAVE %u  band %u  RX_VFO %u\n",
                (unsigned)(gEeprom.VfoInfo[0].freq_config_RX.Frequency / 100000u),
                (unsigned)(gEeprom.VfoInfo[0].freq_config_RX.Frequency % 100000u),
                (unsigned)(gEeprom.VfoInfo[1].freq_config_RX.Frequency / 100000u),
                (unsigned)(gEeprom.VfoInfo[1].freq_config_RX.Frequency % 100000u),
                (unsigned)gTxVfo->CHANNEL_SAVE, (unsigned)gTxVfo->Band,
                (unsigned)gEeprom.RX_VFO);
    /* Why a key beep can be silent: the K1's AUDIO_PlayBeep() returns early in
     * these two states, so the receive state is the first thing to look at. */
    uart_printf("  function %u (%s)  squelch %s  RSSI 0x%03X  BEEP_CONTROL %u\n",
                (unsigned)gCurrentFunction,
                gCurrentFunction == FUNCTION_FOREGROUND ? "FOREGROUND, beeps ok" :
                gCurrentFunction == FUNCTION_INCOMING   ? "INCOMING, beeps ok" :
                gCurrentFunction == FUNCTION_RECEIVE    ? "RECEIVE, beeps SUPPRESSED" :
                gCurrentFunction == FUNCTION_MONITOR    ? "MONITOR, beeps SUPPRESSED" :
                gCurrentFunction == FUNCTION_TRANSMIT   ? "TRANSMIT" :
                gCurrentFunction == FUNCTION_POWER_SAVE ? "POWER_SAVE" : "?",
                rx_squelch_open() ? "open" : "closed",
                (unsigned)rx_rssi(), (unsigned)gEeprom.BEEP_CONTROL);
}

/* Where does the time go on this radio?  Every line is wall-clock milliseconds
 * for N calls, so the numbers can be compared with the loop's 10 ms slice. */
static void print_profile(void)
{
    volatile uint32_t sink = 0;
    uint32_t t0, t1, i;

    uart_puts("\nprofile (systick milliseconds for the count shown):\n");

    t0 = systick_millis();
    for (i = 0; i < 100u; i++)
        sink += BK4819_GetRSSI();
    t1 = systick_millis();
    uart_printf("  100 x BK4819_GetRSSI               %5u ms\n", (unsigned)(t1 - t0));

    /* One tune, i.e. what every UP/DOWN and channel change pays: the K1's
     * RADIO_SetupRegisters, which is ~30 RF register accesses.  With the bus
     * delay as it is, this is the number that makes tuning feel slow. */
    t0 = systick_millis();
    RADIO_SetupRegisters(true);
    t1 = systick_millis();
    uart_printf("  1 x RADIO_SetupRegisters (a tune)  %5u ms\n", (unsigned)(t1 - t0));

    t0 = systick_millis();
    for (i = 0; i < 20u; i++)
        CheckKeys();
    t1 = systick_millis();
    uart_printf("  20 x CheckKeys (the keys, no press) %3u ms\n", (unsigned)(t1 - t0));

    t0 = systick_millis();
    for (i = 0; i < 20u; i++)
        SCANNER_TimeSlice10ms();
    t1 = systick_millis();
    uart_printf("  20 x SCANNER_TimeSlice10ms        %5u ms\n", (unsigned)(t1 - t0));

    t0 = systick_millis();
    for (i = 0; i < 20u; i++)
        UI_MAIN_TimeSlice10ms();
    t1 = systick_millis();
    uart_printf("  20 x UI_MAIN_TimeSlice10ms        %5u ms\n", (unsigned)(t1 - t0));

    t0 = systick_millis();
    for (i = 0; i < 100u; i++) {
        uint8_t buf[21];
        PY25Q16_ReadBuffer(0, buf, sizeof buf);
        sink += buf[0];
    }
    t1 = systick_millis();
    uart_printf("  100 x 21-byte codeplug read       %5u ms\n", (unsigned)(t1 - t0));

    t0 = systick_millis();
    for (i = 0; i < 20u; i++) {
        ChannelScanDisplayInfo_t info;
        if (SETTINGS_FetchChannelScanDisplayInfo(0, &info))
            sink += info.rx.Frequency;
    }
    t1 = systick_millis();
    uart_printf("  20 x channel decode (CH-01)       %5u ms\n", (unsigned)(t1 - t0));

    t0 = systick_millis();
    for (i = 0; i < 20u; i++) {
        char name[16];
        SETTINGS_FetchChannelName(name, 0);
        sink += (uint8_t)name[0];
    }
    t1 = systick_millis();
    uart_printf("  20 x channel name (CH-01)         %5u ms\n", (unsigned)(t1 - t0));

    t0 = systick_millis();
    for (i = 0; i < 20u; i++)
        APP_Update();
    t1 = systick_millis();
    uart_printf("  20 x APP_Update                   %5u ms\n", (unsigned)(t1 - t0));

    t0 = systick_millis();
    for (i = 0; i < 20u; i++)
        APP_TimeSlice10ms();
    t1 = systick_millis();
    uart_printf("  20 x APP_TimeSlice10ms            %5u ms\n", (unsigned)(t1 - t0));

    t0 = systick_millis();
    UI_DisplayMain();
    t1 = systick_millis();
    uart_printf("  1 x UI_DisplayMain (full screen)  %5u ms\n", (unsigned)(t1 - t0));

    t0 = systick_millis();
    UI_DisplayStatus();
    t1 = systick_millis();
    uart_printf("  1 x UI_DisplayStatus (top line)   %5u ms\n", (unsigned)(t1 - t0));

    (void)sink;
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

/* The channel the bench sits on: 145.7500 MHz in 10 Hz units, the same
 * convention as the codeplug. */
#define BENCH_FREQ_HZ 14575000u

/* Both parts sit on the same bit-banged bus and each answers its own id in
 * register 0 -- that is the "version" read this test turns on.  A BK4829 must
 * read 0x4829 and a BK4815 0x4816; the stock's own detect is exactly that check
 * (FUN_08009772 / FUN_08009758), and a failure there is what makes it show its
 * error screen instead of configuring the part.
 *
 * 'R' writes nothing -- it is a read-only snapshot of both parts.  Pressed on a
 * freshly booted radio it shows the parts' **power-on defaults**, not the
 * stock's configuration: getting this firmware into flash means entering update
 * mode, so both parts are reset by the time this code runs.  That was tried and
 * settled on the radio -- the BK4815's register 0x0C reads 0xFFFF here while the
 * stock's boot init always writes 0x0A03 to it -- so do not read these numbers
 * as "what the stock had".  docs/ra89r_bk4829.md has the default map and the
 * argument.
 *
 * 'W' replays both parts' stock boot configuration and re-probes, so a silent
 * bus, a deaf-but-configured chip and a live one look different.  'X' reads
 * every written register back and compares, which is what shows whether the
 * writes landed rather than merely that the parts still answer. */
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

/* The K1 firmware's audio path is a GPIO of its own -- `GPIO_PIN_AUDIO_PATH` =
 * PA8 on that board, driven HIGH to enable through `AUDIO_AudioPathOn()` -- and
 * that is the call this port replaced with a callback.  On this board the line
 * the stock holds asserted is PC13 (`FUN_080177A8` raises it and its
 * `config+0x38` gate is 0 on this codeplug, so it stays HIGH), which leaves PC13
 * as the amplifier-enable candidate.  It is a *static* line: nothing in the
 * squelch path touches it, so `C` inverts it and the speaker can be listened to
 * with the line both ways.
 *
 * PA13/PA14 are *not* this line: they are the status LED (PA13 red, PA14 green,
 * both active high -- measured), which lives in `driver/led.c` behind the
 * console's `L`.  See docs/ra89r_led.md. */

static void audio_path_toggle(void)
{
    audio_path_drive(!audio_path_is_on());
    uart_printf("\nRF: PC13 (audio-path line) -> %s (IDR %s)\n",
                audio_path_is_on() ? "high" : "low",
                gpio_read(AUDIO_PATH_PORT, AUDIO_PATH_PIN) ? "high" : "low");
    uart_puts("  the stock raises this line from its T/R path and holds it high on\n"
              "  this codeplug; receive audio was validated with it high.  What it\n"
              "  switches is not measured -- see docs/ra89r_rffeatures.md.\n");
}

/* The console's 'K': bring the RF up and retune, reporting what landed.  The
 * chain itself is `driver/rx.c`. */
static void rf_k1_bringup(void)
{
    uint16_t lo, hi;

    rx_init(BENCH_FREQ_HZ);
    lo = BK4819_ReadRegister(BK4819_REG_38);
    hi = BK4819_ReadRegister(BK4819_REG_39);

    uart_printf("\nRF: bring-up done (BK4829 id 0x%04X, BK4815 id 0x%04X)\n",
                (unsigned)bk4829_read_reg(BK4829_REG_ID),
                (unsigned)bk4815_read_reg(0));
    uart_printf("  reg 0x38 = 0x%04X, 0x39 = 0x%04X (expect 0x6598 / 0x00DE)\n",
                (unsigned)lo, (unsigned)hi);
    uart_printf("  reg 0x67 RSSI = 0x%04X, squelch %s\n",
                (unsigned)BK4819_GetRSSI(),
                rx_squelch_open() ? "open" : "quiet");
    uart_puts("  press R to see the register snapshot; 'X' still checks the\n"
              "  stock configuration path, which this overwrites.\n");
}

/* The console's 'j': the BK1080 FM receiver on its own two-wire bus, PC14
 * (clock) / PB2 (data), device byte 0x80.  Bring the bus up, probe the chip
 * id, replay the stock's init block, tune 100.0 MHz and read the status back.
 * Nothing on this bus has ever run on the radio, so the first question is
 * simply whether it acknowledges at all -- docs/ra89r_bk1080.md. */
static void fm_bench(void)
{
    uint16_t id;
    uint16_t status;
    uint32_t got;

    i2c_bus_init();
    uart_puts("\nFM: BK1080 on PC14 (clock) / PB2 (data), device byte 0x80\n");

    id = bk1080_read_id();
    uart_printf("  chip id reg 0x01 = 0x%04X (datasheet 0x%04X) -- %s\n",
                (unsigned)id, (unsigned)BK1080_ID,
                id == BK1080_ID ? "present" : "no answer");

    bk1080_configure();
    uart_printf("  init: 68-byte block + %u writes to reg 0x32 sent\n",
                (unsigned)bk1080_config_reg32_writes());

    uart_printf("  after init: reg2=0x%04X reg3=0x%04X reg5=0x%04X\n",
                (unsigned)bk1080_read_reg(0x02), (unsigned)bk1080_read_reg(0x03),
                (unsigned)bk1080_read_reg(0x05));

    bk1080_set_frequency(10000000u);        /* 100.0 MHz, in 10 Hz units */
    (void)bk1080_wait_tune(300);            /* the stock polls STC */
    got = bk1080_get_frequency();
    status = bk1080_read_status();

    uart_printf("  after tune: reg2=0x%04X reg3=0x%04X reg5=0x%04X readchan=0x%04X\n",
                (unsigned)bk1080_read_reg(0x02), (unsigned)bk1080_read_reg(0x03),
                (unsigned)bk1080_read_reg(0x05), (unsigned)bk1080_read_reg(0x0B));
    uart_printf("  tuned 100.0 MHz -> READCHAN reads %u.%05u MHz\n",
                (unsigned)(got / 100000u), (unsigned)(got % 100000u));
    uart_printf("  status 0x%04X: RSSI %u dBuV, SNR %u, STC %u, SF/BL %u, ST %u\n",
                (unsigned)status, (unsigned)bk1080_get_rssi(),
                (unsigned)bk1080_get_snr(),
                (unsigned)bk1080_seek_complete(),
                (unsigned)bk1080_seek_failed(),
                (unsigned)((status & BK1080_STATUS_ST) ? 1u : 0u));

    if (id != BK1080_ID) {
        uart_puts("  0xFFFF means the bus stayed high: check PC14/PB2 and the\n"
                  "  part's supply -- see docs/ra89r_bk1080.md.\n");
        return;
    }

    uart_puts("  the part answers: the bus, the pins and the framing are ours.\n"
              "  band sweep -- RSSI should track the broadcast signals:\n");

    {
        static const uint32_t fm[] = { 8750000u, 9000000u, 9500000u, 9800000u,
                                       10000000u, 10400000u, 10800000u };
        unsigned k;

        for (k = 0; k < sizeof fm / sizeof fm[0]; k++) {
            uint16_t st;

            bk1080_set_frequency(fm[k]);
            (void)bk1080_wait_tune(300);
            st = bk1080_read_status();
            uart_printf("    %3u.%u MHz   RSSI %3u   %s\n",
                        (unsigned)(fm[k] / 100000u),
                        (unsigned)((fm[k] / 10000u) % 10u),
                        (unsigned)(st & BK1080_STATUS_RSSI_MASK),
                        (st & BK1080_STATUS_ST) ? "stereo" : "mono");
        }
    }
}

/* The console's 'n': the BK4815, the second transceiver on PB13.  The stock
 * uses it as the receive path above 134 MHz; this tunes it through the
 * 0x71/0x72 synthesizer path and reads its own 0x43/0x44 meters, to see
 * whether the synth locks and it receives.  Nothing here has run on the
 * radio -- docs/ra89r_bk4815.md. */
static void bk4815_bench(void)
{
    uint16_t id, vco, op, hi, lo, cal, snr, rssi;
    uint32_t word;
    uint8_t band;

    bk4829_init();               /* brings up the shared bus and both selects */
    uart_puts("\nBK4815 (PB13; shared PA12 clock / PB12 data):\n");

    id = bk4815_read_reg(BK4815_REG_ID);
    uart_printf("  reg 0x00 id = 0x%04X (expected 0x%04X) -- %s\n",
                (unsigned)id, (unsigned)BK4815_ID,
                id == BK4815_ID ? "present" : "not answering");

    bk4815_configure();
    uart_printf("  boot config replayed (%u writes, %u RAM-sourced as 0)\n",
                bk4815_config_writes(), bk4815_ram_sourced_writes());

    band = bk4815_vco_band(BENCH_FREQ_HZ);
    word = bk4815_frequency_word(BENCH_FREQ_HZ, band);
    bk4815_set_frequency(BENCH_FREQ_HZ, false);

    vco = bk4815_read_reg(BK4815_REG_VCO);
    op  = bk4815_read_reg(BK4815_REG_OPCTRL);
    hi  = bk4815_read_reg(BK4815_REG_FREQ_HI);
    lo  = bk4815_read_reg(BK4815_REG_FREQ_LO);
    cal = bk4815_read_reg(BK4815_REG_CAL);

    uart_printf("  tuned 145.7500 MHz: band %u, word 0x%08X\n",
                (unsigned)band, (unsigned)word);
    uart_printf("    reg 0x04=0x%04X 0x70=0x%04X 0x71=0x%04X 0x72=0x%04X 0x7E=0x%04X\n",
                (unsigned)vco, (unsigned)op, (unsigned)hi, (unsigned)lo, (unsigned)cal);

    snr  = bk4815_read_reg(0x43);
    rssi = bk4815_read_reg(0x44);
    uart_printf("    meters: 0x43 (SNR)=0x%04X  0x44 (RSSI)=0x%04X -> RSSI %u\n",
                (unsigned)snr, (unsigned)rssi, (unsigned)(rssi & 0x7fu));
    uart_puts("  if 0x71/0x72 hold the word the synth path landed; a 0x44 that\n"
              "  moves with a carrier means it receives.\n"
              "  sampling 0x44 for ~5 s -- key a transmitter on 145.7500 now:\n");

    {
        unsigned k;

        for (k = 0; k < 25u; k++) {
            uint16_t r = bk4815_read_reg(0x44);

            uart_printf("    0x44 = 0x%04X   RSSI %u\n",
                        (unsigned)r, (unsigned)(r & 0x7fu));
            for (volatile unsigned d = 0; d < 400000u; d++)
                ;
        }
    }
}

/* ------------------------------------------- cable-free audio-path bench
 *
 * The Kenwood jack cuts the internal speaker while the programming cable is
 * plugged in, and that jack is also the only way to type at this console -- so
 * the audio path can only be *listened* to with the cable out.  This arms the
 * radio with no console input at all:
 *
 *   - the K1 bring-up runs at boot (init, tune 145.7500, RX on, FM audio);
 *   - PC13 is asserted, the way the stock holds it;
 *   - a squelch mutes the chip's AF output whenever 0x67 says there is no
 *     carrier, using the stock's own marks (open at 0xCF, close below 0xB4);
 *   - the status LED is the read-out: GREEN = squelch open, OFF = quiet,
 *     RED = PC13 pulled low by the bench, so the amplifier question can be
 *     asked without the console.
 *
 * **PTT transmits**: the stock's TX sequence is the K1's `PrepareTransmit`
 * (`0x37 = 0x9D1F`, `0x30 = 0xC1FE` -- PA gain + mic ADC + TX DSP) plus the
 * power/bias register `0x7D`, which the stock computes from the codeplug level
 * (docs/ra89r_rfpath.md, "TX, and how the power is handled"); this radio's value is
 * `0xE958` (level 3 -> bias 0x18).  Releasing PTT goes back to RX.
 *
 * SIDE1/SIDE2/PTT2 still flip PC13, and the console has 'C' (PC13), 'T' (TX)
 * and 'K' for when the cable is in. */
static bool audio_bench_on;
static bool squelch_open;
static KEY_Code_t audio_bench_last = KEY_INVALID;

/* The side keys ask the PC13 question; PTT is the transmitter. */
static bool audio_bench_key(KEY_Code_t key)
{
    return key == KEY_SIDE2 || key == KEY_PTT2;
}

/* Green = receiving (the stock's Rx.Light), off = quiet, red = the bench has
 * pulled PC13 low, which wins because it is the state being tested by ear. */
static void bench_led(void)
{
    if (!audio_path_is_on())
        led_set(LED_RED);
    else if (squelch_open)
        led_set(LED_GREEN);
    else
        led_set(LED_OFF);
}

/* The radio up, once, at boot: the K1 screens read this state (the VFO's
 * frequency, the squelch for the status line), so it stays even though the
 * bench UI does not. */
static void radio_boot(void)
{
    const uint32_t frequency = (gRxVfo != 0 && gRxVfo->freq_config_RX.Frequency != 0u)
                                   ? gRxVfo->freq_config_RX.Frequency : BENCH_FREQ_HZ;

    uart_puts("\nradio: bring-up (the GUI's radio init lands with the RF layer)\n");
    uart_printf("radio: %s %u, %u.%05u MHz  (F then 3 switches VFO/channel mode)\n",
                IS_MR_CHANNEL(gEeprom.ScreenChannel[gEeprom.RX_VFO]) ? "channel" : "frequency",
                (unsigned)gEeprom.ScreenChannel[gEeprom.RX_VFO],
                (unsigned)(frequency / 100000u),
                (unsigned)(frequency % 100000u));
    rx_init(frequency);
    uart_printf("RF: up -- BK4829 id 0x%04X, BK4815 configured (%u writes, "
                "0x0C = 0x%04X), PA PWM ARR %u, audio path %s\n",
                (unsigned)bk4829_read_reg(BK4829_REG_ID), bk4815_config_writes(),
                (unsigned)bk4815_read_reg(0x0C), (unsigned)PA_PWM_ARR,
                audio_path_is_on() ? "asserted" : "low");
    bench_led();
}

/* The bring-up bench loop, only while it owns the panel ('0'). */
static void audio_bench_arm(void)
{
    uart_puts("\naudio bench: the bring-up screens have the panel.  'K' brings the\n"
              "  radio up, PC13 is asserted, PTT flips it.  LED: GREEN = squelch\n"
              "  open, OFF = quiet, RED = PC13 low.  '0' gives the panel back to\n"
              "  the K1 GUI.\n");
    audio_bench_on = true;
}

/* ---------------------------------------------------------------- transmit
 *
 * The stock never writes 0x36 (the K1's SetupPowerAmplifier register) and no
 * timer or DAC is involved: TX is the K1's own sequence -- which this port
 * already carries -- plus 0x7D for the power level.  There is nothing else to
 * switch on: the chip's PA drives the antenna, and the band/path select is
 * left where the bring-up put it. */
#define BENCH_FREQ_HZ 14575000u
#define BENCH_PA_7D   0xE958u

static bool tx_on;

/* TX, and where it stands.
 *
 * The chain now works as far as the antenna: the carrier is real and on
 * frequency (detuning the other radio 50 kHz stops its squelch opening), and
 * the *amplifier* is enabled by the chip's register `0x36` -- bit 7 (PA-CTL) with
 * a bias in bits 15:8.  The stock's own TX path never writes `0x36`; the K1 sets
 * it in `BK4819_SetupPowerAmplifier` and our imported `BK4819_TxOn_Beep` writes
 * it to 0, which is why every earlier build radiated only the chip's own output.
 * `0x8822` (bias 0x88) is the value that sounds like a real carrier here.
 *
 * What was still missing is the *modulation*, and it was one call: the K1's
 * `BK4819_PlayDTMFEx` ends with `BK4819_ExitTxMute()`, and the `EnterDTMF_TX` our
 * bench used leaves register `0x50` **muted** (`0xBB18`).  The stock's own TX
 * writes `0x50 = 0x3B20` -- the unmute value -- which our imported
 * `ExitTxMute` (0x3B18, the value from the K1's bk4829.c) never sent either.
 * Muted, the carrier is there and the audio is not.
 *
 * Two sources so both can be checked without the console:
 *   PTT    -> the chip's DTMF tone (deterministic, no microphone involved)
 *   SIDE1  -> the microphone (0x30 = 0xC1FE, mic ADC, gain in 0x40)
 */
#define TX_SOURCE_TONE 0
#define TX_SOURCE_MIC  1

static tx_source_t bench_source;
static unsigned pa_duty = TX_POWER_COMPARE;    /* 'Y' steps it */

/* The bench TX power setting (the K1's TXP_CalculatedSetting shape); 'Y' still
 * steps the raw PB14 compare on top of it. */
#define BENCH_TX_POWER 0x88u



/* Panel read-out: the source, and the registers that decide whether the signal
 * carries anything. */
static void bench_screen(unsigned duty, uint16_t r50, uint16_t r36, uint16_t r7d)
{
    static const char hex[] = "0123456789ABCDEF";
    char title[20];
    char detail[32];
    unsigned n = 0, i;

    title[n++] = 'T'; title[n++] = 'X'; title[n++] = ' ';
    title[n++] = (bench_source == TX_SOURCE_MIC) ? 'm' : 't';
    title[n++] = (bench_source == TX_SOURCE_MIC) ? 'i' : 'o';
    title[n++] = (bench_source == TX_SOURCE_MIC) ? 'c' : 'n';
    title[n++] = ' ';
    if (duty >= 100u)
        title[n++] = (char)('0' + duty / 100u);
    if (duty >= 10u)
        title[n++] = (char)('0' + (duty / 10u) % 10u);
    title[n++] = (char)('0' + duty % 10u);
    title[n] = '\0';

    for (i = 0; i < 3u; i++) {
        uint16_t v = (i == 0) ? r50 : (i == 1) ? r36 : r7d;
        const char *name = (i == 0) ? "50=" : (i == 1) ? "36=" : "7D=";
        unsigned k;
        for (k = 0; k < 3u; k++)
            detail[n++] = name[k];
        detail[n++] = hex[(v >> 12) & 0xF];
        detail[n++] = hex[(v >> 8) & 0xF];
        detail[n++] = hex[(v >> 4) & 0xF];
        detail[n++] = hex[v & 0xF];
        detail[n++] = ' ';
    }
    detail[n] = '\0';

    ui_bench(title, detail);
    lcd_refresh();
}

static void radio_tx(int on, tx_source_t source)
{
    if (on) {
        tx_start(BENCH_FREQ_HZ, BENCH_TX_POWER, source);
        pa_power((uint16_t)pa_duty);        /* the console can step this */
    } else {
        tx_stop();
        squelch_open = false;
        bench_led();
    }

    uart_printf("\nbench: TX %s %s (0x30 = 0x%04X, 0x33 = 0x%04X, 0x36 = 0x%04X, "
                "0x50 = 0x%04X, PA duty %u of %u)\n",
                on ? "ON" : "off",
                (source == TX_SOURCE_MIC) ? "mic" : "tone",
                (unsigned)BK4819_ReadRegister(BK4819_REG_30),
                (unsigned)BK4819_ReadRegister(BK4819_REG_33),
                (unsigned)BK4819_ReadRegister(BK4819_REG_36),
                (unsigned)BK4819_ReadRegister(BK4819_REG_50),
                (unsigned)TIM1->CCR2, (unsigned)PA_PWM_ARR);
    bench_screen(pa_duty, BK4819_ReadRegister(BK4819_REG_50),
                 BK4819_ReadRegister(BK4819_REG_36),
                 BK4819_ReadRegister(BK4819_REG_7D));
}

static void audio_bench_step(uint32_t now)
{
    static uint32_t last;
    KEY_Code_t key;

    if (!audio_bench_on)
        return;

    key = keypad_poll();

    if (key == KEY_PTT || key == KEY_SIDE1) {
        radio_tx(1, (key == KEY_SIDE1) ? TX_SOURCE_TONE : TX_SOURCE_MIC);
    } else {
        if (tx_on)
            radio_tx(0, bench_source);
        if (key != audio_bench_last && audio_bench_key(key)) {
            audio_path_drive(!audio_path_is_on());
            bench_led();
            uart_printf("\nbench: %s -> PC13 %s\n", keypad_name(key),
                        audio_path_is_on() ? "HIGH" : "low");
        }
    }
    audio_bench_last = key;

    if (tx_on)
        return;                         /* no squelch polling while transmitting */

    if ((uint32_t)(now - last) < 50u)
        return;
    last = now;

    /* The squelch lives in driver/rx.c; the LED and the log are the bench's. */
    {
        bool was = rx_squelch_open();

        rx_poll();
        if (rx_squelch_open() != was) {
            uart_printf("\nsquelch: %s (0x%03X)\n",
                        rx_squelch_open() ? "open" : "quiet",
                        (unsigned)rx_rssi());
            bench_led();
        }
    }
}

/* Sample the RSSI for a few seconds.  One reading cannot tell a carrier from a
 * noise floor, and the single readings taken so far have wandered over the whole
 * range; this makes "carrier on" and "carrier off" a pair of numbers to compare.
 * The stock's own squelch compares reg 0x67 against 0xB4 (180) and 0xCF (207),
 * so those are the marks printed alongside each sample. */
static void rf_watch(void)
{
    uint16_t min = 0xFFFFu, max = 0, last = 0;
    unsigned i;

    uart_printf("\nRF: BK4829 reg 0x67 every 200 ms for 4 s "
                "(0xB4 / 0xCF are the stock's squelch marks)\n"
                "  RX %u.%05u MHz, %s, %s (>134 MHz split)\n",
                (unsigned)(rx_rx_frequency() / 100000u),
                (unsigned)(rx_rx_frequency() % 100000u),
                pa_band_is_uhf() ? "UHF" : "VHF",
                pa_band_is_main() ? "main" : "sub");
    if (!rx_ready())
        uart_puts("  note: 'K' has not run this boot, so the part is not tuned or\n"
                  "  in RX and these readings will not follow a carrier.\n");

    for (i = 0; i < 20u; i++) {
        uint16_t v = BK4819_GetRSSI();

        if (v < min)
            min = v;
        if (v > max)
            max = v;
        last = v;

        uart_printf("  %02u: 0x%03X%4u %s\n", i, (unsigned)v, (unsigned)v,
                    v >= 0xCFu ? "open" : (v < 0xB4u ? "quiet" : "between"));
        systick_delay_ms(200);
    }

    uart_printf("  min 0x%03X  max 0x%03X  last 0x%03X\n",
                (unsigned)min, (unsigned)max, (unsigned)last);
}

/* Console 'Q': characterise the squelch on the two bench frequencies, VHF then
 * UHF.  For each one it measures the noise floor (tinySA output off) and the
 * carrier (output on) and reports the open/close mark that falls between them,
 * so the marks can be set from the radio instead of by eye.  Each phase waits
 * for a key, with a timeout, so the tinySA can be moved between bands. */
static void rf_squelch_autodetect(void)
{
    /* 145.5000 then 446.00625 MHz, in the codec's 10 Hz units. */
    static const uint32_t freqs[] = { 14550000u, 44600625u };
    static const char *const names[] = { "VHF", "UHF" };
    uint32_t saved = (gRxVfo != 0) ? gRxVfo->pRX->Frequency : 0u;
    unsigned f;

    uart_puts("\nsquelch auto-detect (BK4829 reg 0x67) -- "
              "VHF (145.5000) first, then UHF (446.00625)\n");

    for (f = 0; f < sizeof freqs / sizeof freqs[0]; f++) {
        uint16_t floor_lo = 0xFFFFu, floor_hi = 0;
        uint16_t sig_lo = 0xFFFFu, sig_hi = 0;
        uint32_t floor_sum = 0, sig_sum = 0;
        unsigned i;

        uart_printf("\n--- %s %u.%05u MHz ---\n"
                    "  set the tinySA to this frequency with its output OFF,\n"
                    "  then press any key (or wait 15 s)\n",
                    names[f], (unsigned)(freqs[f] / 100000u),
                    (unsigned)(freqs[f] % 100000u));
        (void)uart_getc_timeout(15000);

        rx_set_frequency(freqs[f]);
        systick_delay_ms(100);              /* let the PLL settle */

        for (i = 0; i < 10u; i++) {
            uint16_t v = BK4819_GetRSSI();
            if (v < floor_lo) floor_lo = v;
            if (v > floor_hi) floor_hi = v;
            floor_sum += v;
            systick_delay_ms(100);
        }
        uart_printf("  floor  : min 0x%03X max 0x%03X mean 0x%03X\n",
                    (unsigned)floor_lo, (unsigned)floor_hi,
                    (unsigned)(floor_sum / 10u));

        uart_puts("  now turn the tinySA output ON, then press any key (or wait 15 s)\n");
        (void)uart_getc_timeout(15000);

        for (i = 0; i < 10u; i++) {
            uint16_t v = BK4819_GetRSSI();
            if (v < sig_lo) sig_lo = v;
            if (v > sig_hi) sig_hi = v;
            sig_sum += v;
            systick_delay_ms(100);
        }
        uart_printf("  carrier: min 0x%03X max 0x%03X mean 0x%03X\n",
                    (unsigned)sig_lo, (unsigned)sig_hi,
                    (unsigned)(sig_sum / 10u));

        if ((int)sig_lo - (int)floor_hi >= 0x10) {
            /* Midpoint between the floor and the carrier, keeping the stock's
             * 0x1B (0xCF-0xB4) hysteresis between open and close. */
            uint16_t open_mark = (uint16_t)(floor_hi + ((int)sig_lo - (int)floor_hi) / 2);

            uart_printf("  detected: open 0x%03X, close 0x%03X "
                        "(between floor and carrier; stock 0x%03X / 0x%03X)\n",
                        (unsigned)open_mark, (unsigned)(open_mark - 0x1Bu),
                        (unsigned)RX_SQUELCH_OPEN_MARK,
                        (unsigned)RX_SQUELCH_CLOSE_MARK);
        } else {
            uart_puts("  detected: floor and carrier overlap -- no carrier seen\n");
        }
    }

    if (saved) {
        rx_set_frequency(saved);
        uart_printf("\nback on %u.%05u MHz\n", (unsigned)(saved / 100000u),
                    (unsigned)(saved % 100000u));
    }
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

/* Write each register the configuration carries and read it straight back.
 *
 * This deliberately does *not* compare against whatever the chip happens to
 * hold: the first version did, and on the second hardware run it reported 19 of
 * 36 "differences" on the BK4829 purely because the K1-compatible 'K' command
 * had initialised the part from a different table in between.  Those numbers
 * were history, not faults.  Writing first makes the test independent of what
 * ran before, and its side effect is that the stock configuration is re-applied.
 *
 * Two things to expect, so a mismatch is not read as a fault on its own:
 *
 *   - register 0 is the reset write and reads back as the chip id, not as the
 *     value sent, so it is reported separately;
 *   - where the table writes one register more than once (0x48 twice here,
 *     0x30/0x4a in the per-mode routines), only the last write is observable,
 *     so the earlier ones are skipped.
 *
 * A read-only or self-clearing register will still differ, and the bits the part
 * refuses to store (the BK4815's 0x44 bit 4 and 0x49 bit 10 among them) show up
 * every time.  That is why the values are printed and not just a verdict. */
static void rf_verify_one(const char *name, unsigned count, bool is_4815)
{
    unsigned i, j, checked = 0, bad = 0;
    uint16_t id;

    id = is_4815 ? bk4815_read_reg(BK4815_REG_ID) : bk4829_read_reg(BK4829_REG_ID);
    uart_printf("  %s reg 0x00 reads 0x%04X (the reset write; expected the id)\n",
                name, (unsigned)id);

    for (i = 0; i < count; i++) {
        uint8_t reg, reg2;
        uint16_t want, got;
        int superseded = 0;

        if (is_4815)
            bk4815_config_entry(i, &reg, &want);
        else
            bk4829_config_entry(i, &reg, &want);

        if (reg == 0x00)
            continue;                   /* the reset write, reported above */

        /* Only the last write to a register is observable. */
        for (j = i + 1u; j < count; j++) {
            if (is_4815)
                bk4815_config_entry(j, &reg2, 0);
            else
                bk4829_config_entry(j, &reg2, 0);
            if (reg2 == reg) {
                superseded = 1;
                break;
            }
        }
        if (superseded)
            continue;

        if (is_4815)
            bk4815_write_reg(reg, want);
        else
            bk4829_write_reg(reg, want);

        got = is_4815 ? bk4815_read_reg(reg) : bk4829_read_reg(reg);
        checked++;
        if (got != want) {
            bad++;
            uart_printf("    %s 0x%02X: wrote 0x%04X, read 0x%04X\n",
                        name, (unsigned)reg, (unsigned)want, (unsigned)got);
        }
    }

    uart_printf("  %s: %u registers compared, %u differ%s\n",
                name, checked, bad,
                bad ? "  (read-only or self-clearing registers will show here)"
                    : "  -- every write landed");
}

/* The BK4815's boot configuration is mostly a 36-byte block written in one
 * select pulse, which the per-register check above cannot see.  Write the block
 * and compare each of the 18 words it carries. */
static void rf_verify_block(void)
{
    unsigned len = 0, i, bad = 0;
    const uint8_t *blk = bk4815_config_block(&len);

    if (len != 36u) {
        uart_puts("  BK4815 block: unexpected length\n");
        return;
    }

    bk4815_write_block(2u, blk, len);

    for (i = 0; i < 18u; i++) {
        uint16_t want = (uint16_t)((blk[i * 2u] << 8) | blk[i * 2u + 1u]);
        uint16_t got = bk4815_read_reg((uint8_t)(2u + i));

        if (got != want) {
            bad++;
            uart_printf("    BK4815 reg 0x%02X (block): wrote 0x%04X, read 0x%04X\n",
                        (unsigned)(2u + i), (unsigned)want, (unsigned)got);
        }
    }

    uart_printf("  BK4815 block: 18 registers compared, %u differ\n", bad);
}

static void rf_verify(void)
{
    uart_puts("\nRF: writing and reading back every register the configuration carries\n");
    rf_verify_one("BK4829", bk4829_config_writes(), false);
    rf_verify_one("BK4815", bk4815_config_writes(), true);
    rf_verify_block();
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
        uart_printf("clock: HSI 8 MHz x %u through the PLL = %u Hz, %u flash "
                    "wait state(s), PLL %s; the bootloader had left CFGR=%08X "
                    "CR=%08X FLASH_ACR=%08X\n",
                    (unsigned)BOARD_PLL_MUL, (unsigned)BOARD_SYSCLK_HZ,
                    (unsigned)BOARD_FLASH_WS,
                    gClockPllRunning ? "locked" : "DID NOT LOCK -- running on HSI",
                    (unsigned)left_cfgr, (unsigned)left_cr,
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
    uart_printf("update request at 0x0805FFF0 = 0x%02X (%s)\n",
                (unsigned)*UPDATE_REQUEST,
                (*UPDATE_REQUEST == 0x11u)
                    ? "set: next reset enters the bootloader's update mode"
                    : "clear: normal, the bootloader started this app");

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

    /* The K1's boot-time key mode (helper/boot.c): PTT + SIDE1 held at power-on
     * opens the hidden menu.  Read it *now*, while the keys are still held: the
     * K1 reads it right after its settings load, but the port's panel lights and
     * backlight fade run later in the boot, and a user who releases the keys when
     * the screen appears would miss a check placed at the end. */
    const bool       boot_ptt  = !keypad_ptt2_level();
    const KEY_Code_t boot_key  = keypad_poll();
    const BOOT_Mode_t boot_mode = BOOT_GetMode();

    uart_printf("boot: PTT %d, key %s -> mode %u\n",
                boot_ptt ? 1 : 0, keypad_name(boot_key), (unsigned)boot_mode);

    uart_puts("lcd: reset + init (standard sequence, as the bootloader uses) ...\n");    lcd_init();
    uart_puts("lcd: init done\n");

    /* Clear the panel RAM and the K1 buffers, as the K1's BOARD_Init does with
     * ST7565_Init(); without it the controller keeps its power-on RAM, which is
     * the noise the un-drawn pages show. */
    ST7565_Init();

    BACKLIGHT_InitHardware();
    uart_printf("backlight: K1 driver up (GPIOA pin 5, TIM7+DMA PWM), now %s\n",
                BACKLIGHT_IsOn() ? "on" : "off");
    led_init();
    uart_puts("led: PA13 red / PA14 green, both active high (measured); "
              "'L' cycles off/red/green/both\n");
    beeper_init();
    uart_puts("beeper: DAC tone on PA4 (DAC_OUT1), TIM4 as the sample clock; "
              "'Z' plays the test tones\n");
    /* The radio's interface is the ported K1 application from here on.  The
     * bring-up test card and its bench loop are console diagnostics ('0' hands
     * the panel back to them); at boot the K1 shows its own screen instead.
     * The settings and the codeplug come first, so radio_boot() tunes the
     * measured receive chain to the channel the codeplug put the radio on. */
    /* The K1's own boot order (its App/main.c): settings, calibration, then the
     * two VFOs from the codeplug and the pointer swap that publishes them. */
    SETTINGS_InitEEPROM();
    SETTINGS_LoadCalibration();
    RADIO_ConfigureChannel(0, VFO_CONFIGURE_RELOAD);
    RADIO_ConfigureChannel(1, VFO_CONFIGURE_RELOAD);
    RADIO_SelectVfos();
    SETTINGS_FixupVfoPointers();

    /* Sample the pack before any screen is drawn.  The status bar (and the
     * welcome screen) would otherwise show level 0 until the first 500 ms
     * slice, which reads as the battery jumping from 0% to the real value.
     * BOARD_ADC_GetBatteryInfo() seeds all four K1 samples, so one call is
     * enough. */
    {
        uint16_t batt_v = 0, batt_i = 0;

        BOARD_ADC_GetBatteryInfo(&batt_v, &batt_i);
        BATTERY_GetReadings(true);
    }
    /* The RF/audio chain first: radio_boot() -> rx_init() -> BK4819_Init() brings
     * the shared RF bus up.  BACKLIGHT_TurnOn() both lights the panel and plays
     * the startup beep through the chip, so it must come after -- the K1 orders
     * it the same way (BK4819_Init(), then SETTINGS, then BACKLIGHT_TurnOn at the
     * welcome).  Before the settings load BACKLIGHT_TIME is 0 and the K1 driver
     * reads that as "off"; before radio_boot the beep is silent. */
    radio_boot();
    BACKLIGHT_TurnOn();
    uart_printf("backlight: %s, brightness index %u of %u, %u/32 duty\n",
                BACKLIGHT_IsOn() ? "on" : "off",
                (unsigned)BACKLIGHT_GetBrightness(),
                (unsigned)gEeprom.BACKLIGHT_MAX,
                BACKLIGHT_DutyOnCount());
    show_screen(DISPLAY_MAIN);

    /* Apply the boot mode read earlier.  The K1 sets gF_LOCK *before* building
     * the view, so the hidden items are in it, then lets BOOT_ProcessMode() pick
     * the screen (the menu for F-lock, the VFO otherwise). */
    {
        if (boot_mode == BOOT_MODE_F_LOCK) {
            gF_LOCK = true;
            gEeprom.KEY_LOCK = 0;
            SETTINGS_SaveSettings();
            gMenuCursor = UI_MENU_GetMenuIdx(FIRST_HIDDEN_MENU_ITEM);
            gSubMenuSelection = gSetting_F_LOCK;
            uart_puts("boot: PTT+SIDE1 held -- the hidden menu is open\n");
        }

        /* The K1's main() builds the menu view once, before its loop: the menu
         * key only asks for DISPLAY_MENU, so without this the menu screen would
         * have an empty list. */
        UI_MENU_BuildView();
        BOOT_ProcessMode(boot_mode); /* F-lock -> the menu, else straight into
                                      * the VFO (console '4' has the K1 boot
                                      * screen if it is wanted) */
    }

    /* The K1's power-on display (PonMSG).  When the mode calls for a message the
     * K1 shows the welcome screen for ~2.5 s, or until a key; the port used to
     * skip it entirely, so the PonMSG setting did nothing. */
    if (gEeprom.POWER_ON_DISPLAY_MODE != POWER_ON_DISPLAY_MODE_NONE &&
        gEeprom.POWER_ON_DISPLAY_MODE != POWER_ON_DISPLAY_MODE_SOUND) {
        unsigned t;

        UI_DisplayWelcome();
        for (t = 0; t < 250u; t++) {
            if (keypad_poll() != KEY_INVALID)
                break;
            systick_delay_ms(10);
        }
        gUpdateDisplay = true;
    }

    /* Paint the status line once at boot, as the K1's Main() does with
     * gUpdateStatus = true: gUpdateDisplay only draws pages 1..7, so without
     * this the top bar keeps whatever the panel powered up with. */
    gUpdateStatus = true;
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
                print_diagnostics();
                break;
            case 'u': {
                /* The pack: ADC channel 9 (PB1), via the K1's BOARD_ADC_* hook
                 * (board.c -> driver/battery.c).  Print the whole chain so a
                 * single run says where a zero reading comes from. */
                uint16_t v = 0, c = 0;

                BOARD_ADC_GetBatteryInfo(&v, &c);
                uart_printf("\nbattery: raw %u (ch9/PB1), board %u (10 mV), "
                            "cal[3] %u, avg %u (10 mV), level %u, cur %u\n",
                            (unsigned)battery_raw(), (unsigned)v,
                            (unsigned)gBatteryCalibration[3],
                            (unsigned)gBatteryVoltageAverage,
                            (unsigned)gBatteryDisplayLevel, (unsigned)c);
                uart_printf("  slots %u %u %u %u (idx %u) -- they fill over "
                            "~4 s in the K1 loop\n",
                            (unsigned)gBatteryVoltages[0],
                            (unsigned)gBatteryVoltages[1],
                            (unsigned)gBatteryVoltages[2],
                            (unsigned)gBatteryVoltages[3],
                            (unsigned)gBatteryVoltageIndex);
                uart_printf("  type %u (%s), percent %u%%, text mode %u\n",
                            (unsigned)gEeprom.BATTERY_TYPE,
                            gSubMenu_BATTYP[gEeprom.BATTERY_TYPE],
                            BATTERY_VoltsToPercent(gBatteryVoltageAverage),
                            (unsigned)gSetting_battery_text);
                break;
            }
            case 'm':
                /* The K1 switches between channel and frequency mode with F
                 * then 3 (MAIN_ProcessKeys -> processFKeyFunction -> KEY_3).
                 * This is the same call plus the reconfigure it asks for, so
                 * the mechanism can be exercised without the key sequence --
                 * and it prints the state the switch depends on either way. */
                uart_puts("\nmode: before\n");
                print_mode();
                COMMON_SwitchVFOMode();
                gRequestSaveVFO   = true;
                gVfoConfigureMode = VFO_CONFIGURE_RELOAD;
                gFlagResetVfos    = true;
                APP_Update();              /* apply it now, not on the next slice */
                uart_puts("mode: after COMMON_SwitchVFOMode()\n");
                print_mode();
                break;
            case 'D': {
                /* Step the RF bus delay down towards the fastest value that still
                 * works: the clock went up 6x and the delay was cut 5x, and a
                 * marginal write shows up as 'the squelch opens, no audio'. */
                static const uint8_t steps[] = { 40, 24, 16, 12, 8, 4, 1 };
                uint8_t cur = rf_bus_delay_setting();
                unsigned i, next = 0;

                for (i = 0; i < sizeof steps; i++) {
                    if (steps[i] == cur) {
                        next = (i + 1u) % (unsigned)sizeof steps;
                        break;
                    }
                }
                rf_bus_set_delay(steps[next]);
                uart_printf("\nRF bus delay: %u iterations (was %u).\n",
                            (unsigned)steps[next], (unsigned)cur);
                uart_puts("  'K' re-tunes and opens the squelch; listen, and note"
                          " the value that still gives audio.\n");
                break;
            }
            case 'P':
                print_profile();
                break;
            case 'F': {
                /* The chip-side receive path.  AUTO is the K1 application's
                 * rule (VHF LNA pin 4 below 280 MHz, UHF LNA pin 3 at or above
                 * it); the other modes walk the individual LNA bits so the
                 * radio can settle the stock's own pin-4 activity.  Cycle and
                 * listen (or watch 'S'). */
                static const char *const names[] = {
                    "leave the register as BK4819_Init() left it",
                    "VHF bit: 0x33 |= 0x04, 0x08 clear",
                    "UHF bit: 0x33 |= 0x08, 0x04 clear",
                    "both cleared",
                    "the K1/app rule: VHF pin 4 below 280 MHz, UHF pin 3 (default)",
                };
                uint8_t mode = (uint8_t)((pa_chip_path_mode() + 1u) % 5u);
                uint32_t freq = gRxVfo ? gRxVfo->pRX->Frequency : 0u;

                pa_set_chip_path_mode(mode);
                if (freq)
                    pa_select_band(freq);
                uart_printf("\nfront end: mode %u -- %s\n", (unsigned)mode, names[mode]);
                uart_printf("  PA0 %s, reg 0x33 = 0x%04X\n",
                            pa_band_pa0_high() ? "high" : "low",
                            (unsigned)pa_chip_path_reg());
                uart_puts("  listen (or 'S' with a carrier) and press 'F' for the next;"
                          " note which mode receives\n");
                break;
            }
            case 'B': {
                /* The MCU band pin PA0.  The stock takes PA0 from its config,
                 * not the frequency (pa.h); on this codeplug that is PA0 = 0,
                 * so the default is the stock's value and this experiment is
                 * what settles it on the radio. */
                static const char *const names[] = {
                    "PA0 low whatever the band (the stock's value, default)",
                    "PA0 high whatever the band",
                    "auto: PA0 high for UHF, low for VHF (an inference)",
                };
                uint8_t mode = (uint8_t)((pa_band_pin_mode() + 1u) % PA_BAND_PIN_MODES);
                uint32_t freq = gRxVfo ? gRxVfo->pRX->Frequency : 0u;

                pa_set_band_pin_mode(mode);
                if (freq)
                    pa_select_band(freq);
                uart_printf("\nMCU band pin: mode %u -- %s\n", (unsigned)mode, names[mode]);
                uart_printf("  PA1 high, PA0 %s (%s)\n",
                            pa_band_pa0_high() ? "high" : "low",
                            pa_band_is_uhf() ? "UHF" : "VHF");
                uart_puts("  retune UHF, press 'S' with a carrier and listen;"
                          " note the level that works\n");
                break;
            }
            case '0':
                /* Hand the panel back to the bring-up screens (and to the
                 * GUI again on the next press). */
                bench_panel = !bench_panel;
                if (bench_panel) {
                    audio_bench_arm();
                    draw_test_card();
                    uart_puts("\npanel: bring-up screens\n");
                } else {
                    show_screen(gScreenToDisplay);
                    uart_puts("\npanel: K1 GUI\n");
                }
                break;
            case 'c':
                bench_panel = 1;
                ui_clear();
                lcd_refresh();
                uart_puts("\nscreen cleared\n");
                break;
            case 't':
                bench_panel = 1;
                draw_test_card();
                uart_puts("\ntest card\n");
                break;
            case 'd':
                dump_screen_ascii();
                break;
            case 'b':
                bench_panel = 1;
                show_border = !show_border;
                ui_border(show_border);
                lcd_refresh();
                break;
            case 'f':
                bench_panel = 1;
                ui_pattern(0x55);
                lcd_refresh();
                uart_puts("\ncheckerboard\n");
                break;
            case 'p':
                bench_panel = 1;
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
                bench_panel = 1;
                uart_printf("\npanel re-init (standard, bootloader-proven): %d\n",
                            lcd_reinit(LCD_INIT_STANDARD));
                lcd_set_contrast(contrast);
                draw_test_card();
                break;
            case 's':
                bench_panel = 1;
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
                /* The status LED: PA13 = red, PA14 = green, both active high
                 * (measured, docs/ra89r_led.md).  Cycle off -> red -> green -> both. */
                led_set((led_colour_t)((led_get() + 1) % LED_STATE_COUNT));
                uart_printf("\nled: %s (PA13=%u PA14=%u)\n", led_name(led_get()),
                            gpio_read(LED_PORT, LED_RED_PIN) ? 1u : 0u,
                            gpio_read(LED_PORT, LED_GREEN_PIN) ? 1u : 0u);
                break;
            }
            case 'Z':
                /* The beeper: a DAC tone (driver/beeper.c), not the RF chip's
                 * (the K1's beeps use it).  Three notes so a wrong pin or a
                 * dead DAC is obvious by ear. */
                uart_puts("\nbeeper: 500, 1000, 2000 Hz (DAC_OUT1 / PA4)\n"
                          "  nothing? then the beep is PA5, or the amp's "
                          "enable is elsewhere (docs/ra89r_beeper.md)\n");
                beeper_play(500u, 250u);  systick_delay_ms(80);
                beeper_play(1000u, 250u); systick_delay_ms(80);
                beeper_play(2000u, 250u);
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
            case 'R':
                rf_report();
                break;
            case 'W':
                rf_configure();
                break;
            case 'X':
                rf_verify();
                break;
            case 'K':
                rf_k1_bringup();
                break;
            case 'S':
                rf_watch();
                break;
            case 'Q':
                rf_squelch_autodetect();
                break;
            case 'j':
                fm_bench();
                break;
            case 'a': {
                /* The FM audio route: the stock's FM-on mutes the BK4829's AF
                 * output (`FUN_08009CC4` writes `0x47 = 0x6042`, and
                 * `REG_47<11:8> = 0` = Mute in the BK4829 register table)
                 * because this board's BK4829 `EARO` and BK1080 `LOUT`/`ROUT`
                 * reach the same amplifier input.  Toggle it here while FM is
                 * playing: the broadcast audio should be heard with the RF chip
                 * muted and lost with it at Normal AF.  See
                 * docs/ra89r_bk1080.md, "The FM audio path". */
                static bool rf_af_muted;
                rf_af_muted = !rf_af_muted;
                BK4819_SetAF(rf_af_muted ? BK4819_AF_MUTE : BK4819_AF_FM);
                uart_printf("\nFM route: BK4829 0x47 -> %s (read back 0x%04X)\n",
                            rf_af_muted ? "Mute (0x6042)" : "Normal AF (0x6142)",
                            (unsigned)BK4819_ReadRegister(BK4819_REG_47));
                break;
            }
            case 'n':
                bk4815_bench();
                break;
            case 'e': {
                /* The external SPI NOR flash: identity, then a hexdump. */
                uint16_t man_dev = 0;
                uint32_t jedec = 0;
                uint32_t addr;
                const uint32_t at = 0x00000000u;   /* the codeplug area */

                if (!storage_id(&man_dev, &jedec)) {
                    uart_puts("\nstorage: no answer (MISO idle high -- absent or "
                              "unpowered chip?)\n");
                    break;
                }
                uart_printf("\nstorage: 0x90 -> 0x%04X, JEDEC 0x%06X, size %u KB\n",
                            (unsigned)man_dev, (unsigned)jedec,
                            (unsigned)(storage_size() / 1024u));

                for (addr = at; addr < at + 64u; addr += 16u) {
                    uint8_t buf[16];
                    unsigned i;

                    PY25Q16_ReadBuffer(addr, buf, sizeof buf);
                    uart_printf("  %06X:", (unsigned)addr);
                    for (i = 0; i < sizeof buf; i++)
                        uart_printf(" %02X", buf[i]);
                    uart_puts("\n");
                }
                break;
            }
            case '5':
                /* Save the port's settings to the external flash (blob in the
                 * empty tail of the part -- see driver/py25q16.c). */
                uart_printf("\nstorage: settings save %s\n",
                            storage_save_settings() ? "PASS (read back)"
                                                         : "FAILED");
                break;
            case '6': {
                /* The write test docs/ra89r_eeprom.md has been carrying as pending:
                 * erase + program + read back on a scratch sector. */
                uint32_t bad = 0;

                if (storage_write_test(&bad)) {
                    uart_puts("\nstorage: write test PASS (erase, program and "
                              "read back of 256 bytes)\n");
                } else {
                    uart_printf("\nstorage: write test FAILED at 0x%06X\n",
                                (unsigned)bad);
                }
                break;
            }
            case '1':
                /* The K1 GUI has the panel and the keys; this re-selects it
                 * after the bench screens were used ('0'). */
                bench_panel = 0;
                show_screen(DISPLAY_MAIN);
                uart_puts("\nK1 GUI: the radio's keys drive the ported screens\n");
                break;
            case '4':
                /* The K1's boot screen (shown once at boot). */
                bench_panel = 0;
                UI_DisplayWelcome();
                uart_puts("\nK1 boot screen\n");
                break;
            case '2':
                bench_panel = 0;
                show_screen(DISPLAY_MAIN);
                uart_puts("\nK1 VFO screen\n");
                break;
            case '3':
                bench_panel = 0;
                UI_MENU_BuildView();
                show_screen(DISPLAY_MENU);
                uart_puts("\nK1 menu screen\n");
                break;
            case 'M':
                bench_panel = 0;
                UI_MENU_BuildView();
                show_screen(DISPLAY_MENU);
                uart_puts("\nK1 UI_DisplayMenu() drawn\n");
                break;
            case 'G':
                bench_panel = 0;
                gRxVfo->freq_config_RX.Frequency = BENCH_FREQ_HZ;
                gRxVfo->freq_config_TX.Frequency = BENCH_FREQ_HZ;
                show_screen(DISPLAY_MAIN);
                uart_puts("\nK1 UI_DisplayMain() drawn\n");
                break;
            case 'C':
                audio_path_toggle();
                break;
            case 'T':
                radio_tx(!tx_on, TX_SOURCE_TONE);
                break;
            case 'Y': {
                /* The PA bias PWM compare: the one transmit level worth tuning
                 * by ear or S-meter now that the amplifier works. */
                static const uint16_t steps[] = { 64, 96, 128, 160, 192, 224 };
                static unsigned i;

                pa_duty = steps[i];
                i = (i + 1u) % (sizeof(steps) / sizeof(steps[0]));
                if (tx_active())
                    pa_power((uint16_t)pa_duty);
                uart_printf("\nPA power (PB14 compare) -> %u of %u\n",
                            (unsigned)pa_duty, (unsigned)PA_PWM_ARR);
                break;
            }
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
                if (bench_panel) {
                    ui_echo(echo);
                    lcd_refresh();
                }
            }
        } else {
            systick_delay_ms(1);
        }

        if (bench_panel) {
            if ((uint32_t)(now - last_tick) >= 1000u) {
                last_tick = now;
                ui_status(now / 1000u);
                lcd_refresh();
                if (heartbeat && (now / 1000u) % 5u == 0u)
                    uart_printf("[hb] uptime %us, panel variant %d, contrast 0x%02X\n",
                                (unsigned)(now / 1000u), lcd_variant(), contrast);
            }
            audio_bench_step(now);
            animate_step(now);
        } else {
            /* The ported K1 application owns the panel and the keys.  Keys go
             * through the K1's own app/app.c CheckKeys() (press/hold/repeat, and
             * MAIN_/MENU_/SCANNER_ProcessKeys per screen), the app's periodic
             * duties run on their slices, and driver/tx.c handles PTT (measured
             * transmit chain) plus the repaint.  The console stays a debugging
             * channel throughout. */
            static uint32_t slice10, slice500;
            static uint8_t  last_battery_level = 0xFFu;

            /* The K1's own loop: APP_Update() runs the state machine and
             * repaints when it sets gUpdateDisplay; APP_TimeSlice10ms() ends
             * with CheckKeys(), so the keys are handled there and must not be
             * polled again here.
             *
             * The port's two calls belong on the same 10 ms slice, not on every
             * pass of the loop.  PTT is read by the K1's CheckKeys() on this
             * slice too, and driver/rx.c’s squelch read is one BK4819_GetRSSI()
             * -- about 0.6 ms of bit-banged RF bus, so running it thousands of
             * times a second leaves the application almost no CPU at all. */
            APP_Update();

            if ((uint32_t)(now - slice10) >= 10u) {
                slice10 = now;
                APP_TimeSlice10ms();

                tx_poll_ptt();
                rx_service();
            }
            if ((uint32_t)(now - slice500) >= 500u) {
                slice500 = now;
                APP_TimeSlice500ms();

                /* The K1 refreshes the status bar -- where the battery icon
                 * lives -- only for a charging pack or when the battery-text
                 * setting is on (app/app.c).  A level change must repaint it
                 * regardless, or the icon keeps whatever the boot draw left
                 * (which is level 0, because the samples fill over ~4 s). */
                if (gBatteryDisplayLevel != last_battery_level) {
                    last_battery_level = gBatteryDisplayLevel;
                    gUpdateStatus      = true;
                }
            }
        }

        keypad_monitor_step();
    }
}
