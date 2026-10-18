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
#include <string.h>

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
#include "driver/rf_dual.h"
#include "driver/rx.h"
#include "functions.h"
#include "settings.h"
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

/* FT-817 CAT auto-detect (App/cat.c) shares USART1 with the bring-up console.
 *
 * A CAT frame is a 5-byte burst -- the caller writes it in one go, so the bytes
 * arrive back-to-back -- while console input is typed with gaps.  Bytes are
 * buffered and then: a full, well-formed frame is applied to the radio; a pause
 * (>20 ms) flushes whatever is buffered to the console; an invalid full window
 * hands its oldest byte to the console.  Both features stay live and neither
 * corrupts the other (the console is never fed a frame's bytes, so it cannot
 * echo them or mistake a BCD byte for a command). */
#ifdef ENABLE_CAT
#include "cat.h"

#define CAT_BURST_GAP_MS 20u
#define CAT_Q_LEN        64u

static uint8_t  cat_pend[CAT_FRAME_LEN];
static uint8_t  cat_pend_n;
static uint32_t cat_pend_last;
static char     cat_q[CAT_Q_LEN];
static uint8_t  cat_q_head;
static uint8_t  cat_q_tail;

static void cat_queue_char(char c)
{
    uint8_t next = (uint8_t)((cat_q_head + 1u) % CAT_Q_LEN);

    if (next != cat_q_tail) {                 /* drop on overflow */
        cat_q[cat_q_head] = c;
        cat_q_head        = next;
    }
}

static void cat_flush_pending(void)
{
    uint8_t i;

    for (i = 0; i < cat_pend_n; i++)
        cat_queue_char((char)cat_pend[i]);
    cat_pend_n = 0;
}

static void cat_input_service(uint32_t now)
{
    int raw;

    while ((raw = uart_getc()) >= 0) {
        uint8_t b = (uint8_t)raw;
        uint8_t i;

        if (cat_pend_n > 0u && (uint32_t)(now - cat_pend_last) > CAT_BURST_GAP_MS)
            cat_flush_pending();
        cat_pend_last = now;

        if (cat_pend_n < CAT_FRAME_LEN)
            cat_pend[cat_pend_n++] = b;
        else {
            for (i = 1; i < CAT_FRAME_LEN; i++)
                cat_pend[i - 1u] = cat_pend[i];
            cat_pend[CAT_FRAME_LEN - 1u] = b;
        }

        if (cat_pend_n == CAT_FRAME_LEN) {
            if (cat_frame_valid_bytes(cat_pend)) {
                cat_apply(cat_pend);
                cat_pend_n = 0;
            } else {
                cat_queue_char((char)cat_pend[0]);
                for (i = 1; i < CAT_FRAME_LEN; i++)
                    cat_pend[i - 1u] = cat_pend[i];
                cat_pend_n = CAT_FRAME_LEN - 1u;
            }
        }
    }

    /* A lone typed character: flush it once the burst gap has clearly passed,
     * so a single keystroke still reaches the console promptly. */
    if (cat_pend_n > 0u && (uint32_t)(now - cat_pend_last) > CAT_BURST_GAP_MS)
        cat_flush_pending();
}

static int cat_console_getc(void)
{
    int c;

    if (cat_q_head == cat_q_tail)
        return -1;
    c          = (int)(uint8_t)cat_q[cat_q_tail];
    cat_q_tail = (uint8_t)((cat_q_tail + 1u) % CAT_Q_LEN);
    return c;
}
#endif
#include "driver/py25q16.h"
#include "app/app.h"
#include "app/chFrScanner.h"
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
    uart_puts("\nfull-duplex console (everything else is disabled on this build)\n"
              "          h  help\n"
              "          U  duplex on/off: BK4815 receives, BK4829 transmits (forced)\n"
              "          I  duplex TX-chip AF mode   H  TX keeps the AF DAC (0x30 = 0xC3FE)\n"
              "          g  duplex PC13 at TX: leave / low / high\n"
              "          O  duplex band-path mode   z  4815->4829 audio bridge\n"
              "          N  TX tone test on the transmit VFO (2 s, no microphone)\n"
              "          E  duplex self-test: 4 TX variants, RX-survival verdict\n"
              "          o  route probe: is the 4815 downlink on the 4829's mic input?\n"
              "          7  guided RX diagnostic (timed steps, listening prompts)\n"
              "          T  automated duplex audio test (beep state + level sampling)\n"
              "          y  swap mode: BK4829 RX + BK4815 TX\n"
              "          i/J/d  read-only state capture (app receive scenarios)\n");
}

/* ---- Duplex test mode ('U') ---------------------------------------------
 *
 * Satellite-style full duplex for the satellite use case: the BK4815 keeps
 * receiving on the receive VFO while the BK4829 transmits on the transmit VFO,
 * with the stock's per-chip register split (docs/ra89r_fullduplex.md, "What
 * the repeat entry does to the audio registers").  The read-backs are printed
 * here so the radio can be judged over the UART alone. */

static const char *xcvr_name(rf_xcvr_t c)
{
    return c == RF_XCVR_BK4815 ? "BK4815" : "BK4829";
}

static void print_mhz(uint32_t f10)
{
    uart_printf("%u.%05u", (unsigned)(f10 / 100000u), (unsigned)(f10 % 100000u));
}

static void duplex_status(void)
{
    const rf_xcvr_t rxc = SETTINGS_GetVfoTransceiver(gEeprom.RX_VFO);
    const rf_xcvr_t txc = SETTINGS_GetVfoTransceiver(gEeprom.TX_VFO);

    uart_puts("[dup] RX VFO"); uart_printf("%u", (unsigned)gEeprom.RX_VFO);
    uart_printf(" %s @ ", xcvr_name(rxc));
    print_mhz(gRxVfo->freq_config_RX.Frequency);
    uart_printf(" MHz | TX VFO%u %s @ ", (unsigned)gEeprom.TX_VFO, xcvr_name(txc));
    print_mhz(gTxVfo->freq_config_TX.Frequency);
    uart_printf(" MHz\n[dup] af: %s\n[dup] path: %s\n[dup] tx 0x30: %s\n[dup] pc13: %s\n",
                tx_duplex_af_name(), tx_duplex_path_name(),
                tx_duplex_af_dac_name(), tx_duplex_pc13_name());
}

static void duplex_tick(uint32_t now)
{
    static uint32_t last;
    static bool last_tx;

    if (!tx_duplex_active() && !tx_swap()) {
        last_tx = false;
        return;
    }
    if ((uint32_t)(now - last) < 500u)
        return;
    last = now;

    const bool tx = tx_active();

    if (tx != last_tx) {
        last_tx = tx;
        uart_printf("\n[dup] %s\n", tx ? "TX keyed (BK4829) -- BK4815 RX must continue"
                                       : "TX released");
    }

    if (tx) {
        /* A transmitting BK4829 does not answer register reads reliably (the
         * values do not match the writes and fluctuate between samples), so
         * print what tx_start() programmed instead. */
        const uint32_t f = tx_last_frequency();

        if (tx_swap()) {
            uart_printf("[swap] tx=1 sql=%d rssi4829=%u | 4815 wrote 0x0C=F823 0x6D=E718 "
                        "0x73=8000 0x11=5800 0x12=0148 0x49=1A02 f=%u.%05u | "
                        "4829 RX 0x47=%04X 0x30=%04X\n",
                        rx_squelch_open() ? 1 : 0, (unsigned)BK4819_GetRSSI(),
                        (unsigned)(f / 100000u), (unsigned)(f % 100000u),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_47),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_30));
            return;
        }

        uart_printf("[dup] tx=1 sql=%d rssi4815=%u | 4815 0x0C=%04X 0x49=%04X 0x75=%04X | "
                    "TX wrote 0x47=%04X 0x30=%04X 0x50=%04X 0x40=%04X 0x36=%04X 0x33=%04X "
                    "0x7D=%04X f=%u.%05u\n",
                    rx_squelch_open() ? 1 : 0,
                    (unsigned)bk4815_read_rssi(),
                    (unsigned)bk4815_read_reg(0x0Cu), (unsigned)bk4815_read_reg(0x49u),
                    (unsigned)bk4815_read_reg(0x75u),
                    (unsigned)tx_duplex_af_value(), (unsigned)tx_duplex_reg30_value(),
                    (unsigned)TX_REG50_UNMUTE, (unsigned)0x3700u,
                    (unsigned)pa_last_reg36(), (unsigned)pa_last_reg33(),
                    (unsigned)TX_REG7D_POWER,
                    (unsigned)(f / 100000u), (unsigned)(f % 100000u));
    } else {
        if (tx_swap()) {
            uart_printf("[swap] tx=0 sql=%d rssi4829=%u | 4815 0x0C=%04X 0x49=%04X | "
                        "4829 0x47=%04X 0x30=%04X 0x38=%04X 0x39=%04X\n",
                        rx_squelch_open() ? 1 : 0, (unsigned)BK4819_GetRSSI(),
                        (unsigned)bk4815_read_reg(0x0Cu),
                        (unsigned)bk4815_read_reg(0x49u),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_47),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_30),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_38),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_39));
        } else {
            uart_printf("[dup] tx=0 sql=%d rssi4815=%u 4815 0x0C=%04X 0x49=%04X 0x75=%04X "
                        "0x71=%04X 0x72=%04X 0x6D=%04X | "
                        "4829 0x47=%04X 0x30=%04X 0x50=%04X 0x36=%04X 0x33=%04X 0x7D=%04X "
                        "0x38=%04X 0x39=%04X pc13=%u\n",
                        rx_squelch_open() ? 1 : 0,
                        (unsigned)bk4815_read_rssi(),
                        (unsigned)bk4815_read_reg(0x0Cu), (unsigned)bk4815_read_reg(0x49u),
                        (unsigned)bk4815_read_reg(0x75u), (unsigned)bk4815_read_reg(0x71u),
                        (unsigned)bk4815_read_reg(0x72u), (unsigned)bk4815_read_reg(0x6Du),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_47),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_30),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_50),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_36),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_33),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_7D),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_38),
                        (unsigned)BK4819_ReadRegister(BK4819_REG_39),
                        audio_path_is_on() ? 1u : 0u);
        }
    }
}

/* Automated duplex self-test ('E'): forces the test configuration (RX VFO on
 * the BK4815, the other VFO on the BK4829), runs a deterministic tone
 * transmission for each variant, and measures the receive lane's RSSI through
 * it.  Restores the previous bindings/duplex state afterwards; the panel is
 * frozen while it runs (~15 s) because the console blocks the app loop. */
static void duplex_selftest(void)
{
    static const struct {
        uint8_t af;
        uint8_t path;
        bool    tr;
    } variants[] = {
        { 0, 0, false },   /* stock-like: TX AF muted, full band path */
        { 1, 0, false },   /* TX AF normal: the shared-node audio stays up */
        { 0, 1, false },   /* PA0/PA1 left as the receive path had them */
        { 0, 0, true  },   /* also write the BK4815 T/R at transmit entry */
    };

    const uint8_t   rx_vfo = gEeprom.RX_VFO;
    const uint8_t   tx_vfo = (uint8_t)(1u - rx_vfo);
    const rf_xcvr_t saved_rx = SETTINGS_GetVfoTransceiver(rx_vfo);
    const rf_xcvr_t saved_tx = SETTINGS_GetVfoTransceiver(tx_vfo);
    const uint8_t   saved_txvfo = gEeprom.TX_VFO;
    const bool      saved_dup = tx_duplex_active();
    const uint32_t  rx_f = gEeprom.VfoInfo[rx_vfo].freq_config_RX.Frequency;
    uint32_t        idle_sum = 0;
    uint32_t        idle_avg;
    unsigned        i;
    unsigned        v;

    uart_puts("\n[selftest] duplex self-test: forcing the test configuration\n"
              "  (bindings restored afterwards; the panel is frozen ~15 s)\n");

    SETTINGS_SetVfoTransceiver(rx_vfo, RF_XCVR_BK4815);
    SETTINGS_SetVfoTransceiver(tx_vfo, RF_XCVR_BK4829);
    gEeprom.TX_VFO = tx_vfo;
    tx_set_duplex(true, true, true);
    SETTINGS_FixupVfoPointers();

    /* Put the receive chip on its VFO independently of the previous binding,
     * with its demodulator set -- the poller is blocked for the whole test. */
    bk4815_set_frequency(rx_f, false);
    bk4815_write_reg(0x75u, (rx_f >= PA_BAND_SPLIT) ? 0x0Au : 0x11u);
    bk4815_enter_rx();
    rx_4815_set_applied();

    uart_printf("[selftest] RX VFO%u BK4815 @ ", (unsigned)rx_vfo);
    print_mhz(rx_f);
    uart_printf(" MHz | TX VFO%u BK4829 @ ", (unsigned)tx_vfo);
    print_mhz(gTxVfo->freq_config_TX.Frequency);
    uart_puts(" MHz\n[selftest] put a carrier on the RX frequency; listen on the TX one\n");

    for (i = 0; i < 5u; i++) {
        idle_sum += bk4815_read_rssi();
        systick_delay_ms(200);
    }
    idle_avg = idle_sum / 5u;

    if (idle_avg < 30u)
        uart_printf("[selftest] idle rssi4815=%u: no/weak downlink carrier -- the numbers\n"
                    "  below are not a receive verdict\n", (unsigned)idle_avg);
    else if (idle_avg < 50u)
        uart_printf("[selftest] idle rssi4815=%u (modest carrier; judge against the floor:\n"
                    "  ~11 in the 145.5-TX layout, ~41 in the 446-TX layout)\n",
                    (unsigned)idle_avg);
    else
        uart_printf("[selftest] idle rssi4815=%u (carrier present)\n", (unsigned)idle_avg);

    for (v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
        uint32_t min = 0xFFFFFFFFu;
        uint32_t sum = 0;

        tx_duplex_set_af(variants[v].af);
        tx_duplex_set_path(variants[v].path);
        tx_duplex_set_bk4815_tr(variants[v].tr);

        tx_start(gTxVfo->freq_config_TX.Frequency,
                 gTxVfo->TXP_CalculatedSetting, TX_SOURCE_TONE);
        for (i = 0; i < 20u; i++) {
            const uint16_t r = bk4815_read_rssi();

            if (r < min)
                min = r;
            sum += r;
            systick_delay_ms(100);
        }
        tx_stop();
        systick_delay_ms(300);

        uart_printf("[selftest] af=%u path=%u tr=%u | idle=%u | tx min=%u avg=%u | ",
                    (unsigned)variants[v].af, (unsigned)variants[v].path,
                    variants[v].tr ? 1u : 0u, (unsigned)idle_avg,
                    (unsigned)min, (unsigned)(sum / 20u));
        if (idle_avg < 30u)
            uart_puts("(carrier too weak)\n");
        else if (min + 4u >= idle_avg)
            uart_puts("RX SURVIVED TX\n");
        else
            uart_puts("RX LOST DURING TX\n");
    }

    tx_stop();
    tx_set_duplex(false, false, false);
    SETTINGS_SetVfoTransceiver(rx_vfo, saved_rx);
    SETTINGS_SetVfoTransceiver(tx_vfo, saved_tx);
    gEeprom.TX_VFO = saved_txvfo;
    SETTINGS_FixupVfoPointers();
    tx_duplex_set_bk4815_tr(false);
    if (saved_dup)
        tx_set_duplex(true,
                      SETTINGS_GetVfoTransceiver(gEeprom.RX_VFO) == RF_XCVR_BK4815,
                      SETTINGS_GetVfoTransceiver(gEeprom.TX_VFO) == RF_XCVR_BK4829);

    uart_puts("[selftest] done -- bindings and duplex state restored\n");
}

/* ---- Guided receive diagnostic ('7') ------------------------------------
 *
 * The question: "the receiver is broken -- which part?"  The test walks the
 * operator through fixed, timed steps, samples the receive chain at each step,
 * evaluates the result itself and prints every register it reads.  It never
 * transmits.
 *
 * The console blocks the app loop, so the receive gates the poller would open
 * (`0x49` on the BK4815, `0x47`/`0x30` on the BK4829, the PC13 audio path) are
 * opened by the test itself: a gate that is shut because nothing polled must
 * not be reported as "dead audio".  The listening prompts are answered with
 * 1/2/3 and become part of the verdict -- the firmware cannot hear the speaker,
 * but it can say which measurement plus which answer means what. */

#define RXDIAG_ANSWER_MS 30000u

/* One key from `accept`, echoed, or -1 after `ms` milliseconds. */
static int rxdiag_ask(const char *accept, unsigned ms)
{
    const uint32_t start = systick_millis();

    while ((uint32_t)(systick_millis() - start) < ms) {
        const int c = uart_getc();

        if (c >= 0) {
            for (const char *p = accept; *p != '\0'; p++) {
                if (*p == (char)c) {
                    uart_printf("%c\n", (char)c);
                    return c;
                }
            }
            continue;
        }
        systick_delay_ms(20);
    }
    uart_puts("(no answer)\n");
    return -1;
}

/* A visible countdown the operator can cut short with ENTER. */
static void rxdiag_countdown(unsigned seconds)
{
    uart_puts("[rxdiag] press ENTER to start now, or wait: ");
    for (unsigned s = 0; s < seconds; s++) {
        for (unsigned k = 0; k < 50u; k++) {
            const int c = uart_getc();

            if (c == '\r' || c == '\n') {
                uart_puts(" start\n");
                return;
            }
            systick_delay_ms(20);
        }
        uart_puts(".");
    }
    uart_puts(" start\n");
}

static char rxdiag_char(int c)
{
    return c < 0 ? '-' : (char)c;
}

/* The RSSI of whichever chip is currently the receive source. */
static uint16_t rxdiag_rssi(void)
{
    return rx_audio_is_4815() ? bk4815_read_rssi() : BK4819_GetRSSI();
}

/* Sample the receive RSSI for `seconds`, printing a running min/max.  When the
 * source is the BK4815 it also samples the chip's own audio-level meters, which
 * answer "is its demod producing audio at all?" without needing the speaker:
 * REG73 bits 14:07 are the read-only RX audio level and REG74 is the RX signal
 * strength before the volume control (datasheet §1.3/REG73/74). */
static uint32_t rxdiag_measure(unsigned seconds, uint16_t *lo, uint16_t *hi,
                               uint32_t *lvl73_mean, uint32_t *lvl74_mean)
{
    const uint32_t n = (uint32_t)seconds * 4u;
    uint32_t sum = 0, l73 = 0, l74 = 0;

    *lo = 0xFFFFu;
    *hi = 0u;
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t r = rxdiag_rssi();

        sum += r;
        if (r < *lo) *lo = r;
        if (r > *hi) *hi = r;

        uint16_t a73 = 0, a74 = 0;

        if (rx_audio_is_4815()) {
            a73 = (uint16_t)((bk4815_read_reg(0x49u) >> 7) & 0xFFu);
            a74 = (uint16_t)(bk4815_read_reg(0x4Au) & 0x7FFFu);
            l73 += a73;
            l74 += a74;
        }

        if ((i % 4u) == 3u) {
            uart_printf("\r[rxdiag]   rssi %3u  min %3u  max %3u  lvl73 %3u  lvl74 %4u ",
                        (unsigned)r, (unsigned)*lo, (unsigned)*hi,
                        (unsigned)a73, (unsigned)a74);
        }
        systick_delay_ms(250);
    }
    uart_puts("\n");
    *lvl73_mean = rx_audio_is_4815() ? (l73 + n / 2u) / n : 0u;
    *lvl74_mean = rx_audio_is_4815() ? (l74 + n / 2u) / n : 0u;
    return (sum + n / 2u) / n;
}

/* Open the receive chain the way an open squelch would. */
static void rxdiag_open_gates(void)
{
    if (rx_audio_is_4815()) {
        bk4815_enter_rx();          /* the stock's RX set, incl. 0x6D = 0x6618 */
        /* The 4815's own AF gate at the maximum volume (REG73 nibble), so a
         * quiet-but-working downlink cannot be mistaken for noise. */
        bk4815_write_reg(BK4815_REG_AF, 0x1A0Fu);
    }
    BK4819_RX_TurnOn();             /* 0x37 = 0x9F1F, 0x30 = 0xBFF1 */
    BK4819_SetAF(BK4819_AF_FM);     /* 0x47 = 0x6142: the shared node alive */
    audio_path_drive(1);            /* PC13 high */
}

static void rxdiag_dump(const char *when)
{
    const uint32_t f = gRxVfo->freq_config_RX.Frequency;

    uart_printf("[rxdiag] %s\n", when);
    uart_printf("[rxdiag]   source %s | VFO %u %u.%05u MHz | duplex %u | pc13 %u | squelch %s\n",
                rx_audio_is_4815() ? "BK4815" : "BK4829",
                (unsigned)gEeprom.RX_VFO,
                (unsigned)(f / 100000u), (unsigned)(f % 100000u),
                tx_duplex_active() ? 1u : 0u,
                audio_path_is_on() ? 1u : 0u,
                rx_squelch_open() ? "open" : "closed");
    uart_printf("[rxdiag]   4815: 0x0C=%04X 0x49=%04X 0x6D=%04X 0x75=%04X 0x71=%04X 0x72=%04X\n",
                (unsigned)bk4815_read_reg(0x0Cu), (unsigned)bk4815_read_reg(0x49u),
                (unsigned)bk4815_read_reg(0x6Du), (unsigned)bk4815_read_reg(0x75u),
                (unsigned)bk4815_read_reg(0x71u), (unsigned)bk4815_read_reg(0x72u));
    uart_printf("[rxdiag]   4829: 0x47=%04X 0x30=%04X 0x13=%04X 0x33=%04X 0x36=%04X 0x37=%04X\n",
                (unsigned)BK4819_ReadRegister(BK4819_REG_47),
                (unsigned)BK4819_ReadRegister(BK4819_REG_30),
                (unsigned)BK4819_ReadRegister(BK4819_REG_13),
                (unsigned)BK4819_ReadRegister(BK4819_REG_33),
                (unsigned)BK4819_ReadRegister(BK4819_REG_36),
                (unsigned)BK4819_ReadRegister(BK4819_REG_37));
    uart_printf("[rxdiag]   expected: 4815 0x0C=0A03, 0x6D=6618, 0x49 bit15=0 | "
                "4829 0x30=BFF1 (BDF1 = own AF DAC off), 0x47=6142, 0x37=9F1F | pc13=1\n");
}

/* Register-level expectations, as warnings rather than a verdict. */
static void rxdiag_warn(void)
{
    const uint16_t r6d = bk4815_read_reg(0x6Du);
    const uint16_t r30 = BK4819_ReadRegister(BK4819_REG_30);
    const uint16_t r47 = BK4819_ReadRegister(BK4819_REG_47);

    if (r6d != 0x6618u)
        uart_printf("[rxdiag] WARN: 4815 0x6D=%04X, the receive value is 6618 (%s)\n",
                    (unsigned)r6d, r6d == 0xE718u ? "the transmit PA value!" : "not the stock's");
    if (r30 != 0xBFF1u && r30 != 0xBDF1u)
        uart_printf("[rxdiag] WARN: 4829 0x30=%04X, the receive word is BFF1 (BDF1 with its own AF DAC off)\n",
                    (unsigned)r30);
    if ((r47 >> 8 & 0xFu) == 0u)
        uart_printf("[rxdiag] WARN: 4829 0x47=%04X has the AF output muted (AFD 0)\n",
                    (unsigned)r47);
    if (!audio_path_is_on())
        uart_puts("[rxdiag] WARN: PC13 (the audio-path line) is low\n");
}

static void rxdiag_run(void)
{
    if (tx_active()) {
        uart_puts("\n[rxdiag] release PTT first\n");
        return;
    }

    const uint32_t f = gRxVfo->freq_config_RX.Frequency;

    uart_puts("\n[rxdiag] guided receive diagnostic -- it never transmits\n");
    uart_puts("[rxdiag] before starting, on the radio:\n");
    uart_puts("[rxdiag]   1) leave the radio in the configuration that fails (normal\n"
              "[rxdiag]      reception, or the duplex test after U)\n");
    uart_puts("[rxdiag]   2) put radio A on the receiver's frequency below, ready to key\n");
    uart_puts("[rxdiag]   3) you will be asked to listen and press 1, 2 or 3\n");
    uart_printf("[rxdiag] receiver now: %s, VFO %u, %u.%05u MHz, duplex %u\n",
                rx_audio_is_4815() ? "BK4815" : "BK4829",
                (unsigned)gEeprom.RX_VFO,
                (unsigned)(f / 100000u), (unsigned)(f % 100000u),
                tx_duplex_active() ? 1u : 0u);
    rxdiag_dump("state before the test");
    rxdiag_countdown(20u);

    rxdiag_open_gates();
    uart_puts("[rxdiag] (the console blocks the app's poller, so the test opens the\n"
              "[rxdiag]  receive gates itself, the 4815 AF at maximum volume)\n");
    rxdiag_warn();

    uart_puts("[rxdiag] step 1/7: do NOT transmit yet -- quiet baseline (6 s)\n");
    uint16_t lo, hi;
    uint32_t idle73, idle74, car73, car74;
    const uint32_t idle = rxdiag_measure(6u, &lo, &hi, &idle73, &idle74);
    uart_printf("[rxdiag]   idle rssi mean %u (min %u, max %u) | 4815 audio lvl73 %u lvl74 %u\n",
                (unsigned)idle, (unsigned)lo, (unsigned)hi,
                (unsigned)idle73, (unsigned)idle74);

    uart_puts("[rxdiag] step 2/7: KEY RADIO A on the receiver frequency NOW (12 s)\n");
    const uint32_t car = rxdiag_measure(12u, &lo, &hi, &car73, &car74);
    const bool heard = car >= idle + 8u;
    uart_printf("[rxdiag]   carrier rssi mean %u (min %u, max %u) | 4815 audio lvl73 %u lvl74 %u -> %s\n",
                (unsigned)car, (unsigned)lo, (unsigned)hi,
                (unsigned)car73, (unsigned)car74,
                heard ? "CARRIER SEEN" : "NO CARRIER SEEN");

    /* The 4815's own level readouts are printed for the record.  On this part
     * they proved noisy (they do not track the carrier), so the listening
     * answers, not these numbers, drive the verdict. */
    if (rx_audio_is_4815()) {
        uart_printf("[rxdiag]   audio-level readout (informational): lvl73 %u -> %u, "
                    "lvl74 %u -> %u\n",
                    (unsigned)idle73, (unsigned)car73,
                    (unsigned)idle74, (unsigned)car74);
    }

    rxdiag_dump("state with the carrier");
    rxdiag_warn();

    int answer = -1;

    if (!heard) {
        uart_puts("[rxdiag] the RSSI did not rise, so the receiver does not see radio A.\n"
                  "[rxdiag] keep radio A keyed and answer:\n"
                  "[rxdiag]   1 = I hear it anyway (so only the RSSI path is wrong)\n"
                  "[rxdiag]   3 = I hear nothing\n");
        answer = rxdiag_ask("13", RXDIAG_ANSWER_MS);
    } else {
        uart_puts("[rxdiag] step 3/7: keep radio A transmitting and LISTEN to this radio:\n"
                  "[rxdiag]   1 = I hear radio A's audio (voice/music)\n"
                  "[rxdiag]   2 = I hear noise only\n"
                  "[rxdiag]   3 = I hear nothing\n"
                  "[rxdiag] answer 1/2/3 (30 s): ");
        answer = rxdiag_ask("123", RXDIAG_ANSWER_MS);
    }

    const char *verdict = "inconclusive";
    int after_retune = -1;
    int again = -1;
    int muted = -1;
    int dac_off = -1;

    if (answer == '1') {
        verdict = heard
            ? "receive path OK in this configuration (RF, gates and audio)"
            : "the RSSI did not move but the audio is there -- the audio path is fine and the RSSI reading is the odd one";
    } else if (answer < 0) {
        verdict = "inconclusive: no answer to the listening prompt (data above stands)";
    } else if (!heard) {
        verdict = "RF: the receiver does not see radio A -- tuning/front-end, not audio";
    } else {
        /* Step 4: rewrite the frequency word -- the coordinator owns it, and a
         * stale word would leave the part off-channel. */
        uart_puts("[rxdiag] step 4/7: rewriting the 4815's frequency word (in case its\n"
                  "[rxdiag] tuning is stale) -- listen again:\n"
                  "[rxdiag]   1 = audio now   2 = still noise   3 = still nothing\n"
                  "[rxdiag] answer 1/2/3 (30 s): ");
        bk4815_set_frequency(f, false);
        bk4815_write_reg(BK4815_REG_AF, 0x1A0Fu);
        systick_delay_ms(500);
        after_retune = rxdiag_ask("123", RXDIAG_ANSWER_MS);

        if (after_retune == '1') {
            verdict = "rewriting the 4815's frequency word brought the audio back: the part was off-channel (stale tuning), not its demod -- a coordinator/tuning bug in the 4815 lane";
        } else {
            /* Step 5: re-assert the receive state. */
            uart_puts("[rxdiag] step 5/7: re-asserting the whole receive state -- listen:\n"
                      "[rxdiag]   1 = audio now   2 = still noise   3 = still nothing\n"
                      "[rxdiag] answer 1/2/3 (30 s): ");
            rxdiag_open_gates();
            systick_delay_ms(500);
            again = rxdiag_ask("123", RXDIAG_ANSWER_MS);

            if (again == '1') {
                verdict = "audio recovered by re-asserting the receive state (a drifted demod/AF/gate)";
            } else {
                /* Step 6: mute the idle BK4829 and its demod noise. */
                uart_puts("[rxdiag] step 6/7: muting the idle BK4829's AF (the other VFO's\n"
                          "[rxdiag] demod noise) -- listen:\n"
                          "[rxdiag]   1 = audio now   2 = noise still   3 = silence\n"
                          "[rxdiag] answer 1/2/3 (30 s): ");
                BK4819_SetAF(BK4819_AF_MUTE);
                if (rx_audio_is_4815())
                    bk4815_write_reg(BK4815_REG_AF, 0x1A0Fu);
                systick_delay_ms(500);
                muted = rxdiag_ask("123", RXDIAG_ANSWER_MS);

                /* Step 7: reopen that stage with its own DAC off. */
                uart_puts("[rxdiag] step 7/7: reopening the BK4829's AF stage (the 4815's\n"
                          "[rxdiag] audio needs it) with the BK4829's own AF DAC off\n"
                          "[rxdiag] (0x30 = BDF1) -- listen:\n"
                          "[rxdiag]   1 = audio now   2 = noise still   3 = silence\n"
                          "[rxdiag] answer 1/2/3 (30 s): ");
                BK4819_WriteRegister(BK4819_REG_30, 0xBDF1u);
                BK4819_SetAF(BK4819_AF_FM);
                if (rx_audio_is_4815())
                    bk4815_write_reg(BK4815_REG_AF, 0x1A0Fu);
                systick_delay_ms(500);
                dac_off = rxdiag_ask("123", RXDIAG_ANSWER_MS);

                if (muted == '1')
                    verdict = "the idle BK4829's demod noise was the only audio: muting that chip (console z off) leaves the downlink audible";
                else if (dac_off == '1')
                    verdict = "the idle BK4829's AF DAC noise was masking the 4815 audio: stage open + 0x30 = BDF1 is the 4815-lane configuration";
                else if (dac_off == '3')
                    verdict = "the 4815's audio is absent: with the 4829's stage open and its DAC off nothing is on the node -- the 4815's demod/front-end, not the gates";
                else if (muted == '3')
                    verdict = "the 4815's audio needs the BK4829's AF stage open (shared-node loading)";
                else if (muted == '2' || dac_off == '2')
                    verdict = "noise survives with the 4829 muted/DAC off -- the noise is the 4815's own demod (the known demod state)";
                else
                    verdict = "inconclusive (no answer at the later prompt)";
            }
        }
    }

    uart_printf("[rxdiag] answers: initial %c, after retune %c, after re-assert %c, "
                "4829 muted %c, 4829 DAC off %c\n",
                rxdiag_char(answer), rxdiag_char(after_retune), rxdiag_char(again),
                rxdiag_char(muted), rxdiag_char(dac_off));
    uart_printf("\n[rxdiag] VERDICT: %s\n", verdict);
    rxdiag_dump("final state");
    uart_puts("[rxdiag] done -- the app's poller re-drives the gates on its next pass\n");
}

/* ---- Duplex configuration helpers (shared by 'U' and 'T') --------------- */

static bool      s_dup_saved_valid;
static rf_xcvr_t s_dup_saved_rx;
static rf_xcvr_t s_dup_saved_tx;
static uint8_t   s_dup_saved_txvfo;
static uint8_t   s_dup_saved_rxvfo;

/* Force the satellite configuration: the selected VFO receives on the BK4815,
 * the other VFO transmits on the BK4829. */
static void duplex_force_on(void)
{
    const uint8_t rx_vfo = gEeprom.RX_VFO;
    const uint8_t tx_vfo = (uint8_t)(1u - rx_vfo);

    s_dup_saved_rx    = SETTINGS_GetVfoTransceiver(rx_vfo);
    s_dup_saved_tx    = SETTINGS_GetVfoTransceiver(tx_vfo);
    s_dup_saved_txvfo = gEeprom.TX_VFO;
    s_dup_saved_rxvfo = rx_vfo;
    s_dup_saved_valid = true;

    SETTINGS_SetVfoTransceiver(rx_vfo, RF_XCVR_BK4815);
    SETTINGS_SetVfoTransceiver(tx_vfo, RF_XCVR_BK4829);
    gEeprom.TX_VFO = tx_vfo;
    tx_set_duplex(true, true, true);
    SETTINGS_FixupVfoPointers();

    /* Put the receive chip on its VFO now, not on the first coordinator
     * pass: the user may PTT straight away.  The demodulator set goes with
     * it -- a tuned 4815 demodulates nothing until the stock's RX set is
     * applied, and the console blocks the poller that would apply it on the
     * first squelch opening. */
    bk4815_set_frequency(gRxVfo->freq_config_RX.Frequency, false);
    bk4815_write_reg(0x75u,
                     (gRxVfo->freq_config_RX.Frequency >= PA_BAND_SPLIT)
                         ? 0x0Au : 0x11u);
    bk4815_enter_rx();
    rx_4815_set_applied();
}

static void duplex_force_off(void)
{
    tx_set_duplex(false, false, false);
    if (s_dup_saved_valid) {
        SETTINGS_SetVfoTransceiver(s_dup_saved_rxvfo, s_dup_saved_rx);
        SETTINGS_SetVfoTransceiver((uint8_t)(1u - s_dup_saved_rxvfo), s_dup_saved_tx);
        gEeprom.TX_VFO = s_dup_saved_txvfo;
        s_dup_saved_valid = false;
    } else {
        gEeprom.TX_VFO = gEeprom.RX_VFO;
    }
    SETTINGS_FixupVfoPointers();
}

/* ---- Automated duplex audio test ('T') ---------------------------------- */

/* Sample the receive side for `seconds`: RSSI plus the 4815's audio-level
 * meters (the same readout the guided diagnostic uses). */
static uint32_t duptest_measure(unsigned seconds, uint16_t *lo, uint16_t *hi,
                                uint32_t *l73, uint32_t *l74)
{
    const uint32_t n = (uint32_t)seconds * 4u;
    uint32_t sum = 0, a73 = 0, a74 = 0;

    *lo = 0xFFFFu;
    *hi = 0u;
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t r = bk4815_read_rssi();

        sum += r;
        if (r < *lo) *lo = r;
        if (r > *hi) *hi = r;

        a73 += (uint16_t)((bk4815_read_reg(0x49u) >> 7) & 0xFFu);
        a74 += (uint16_t)(bk4815_read_reg(0x4Au) & 0x7FFFu);
        systick_delay_ms(250);
    }
    *l73 = (a73 + n / 2u) / n;
    *l74 = (a74 + n / 2u) / n;
    return (sum + n / 2u) / n;
}

/* One command: force duplex, wait for the downlink carrier, key the "beep"
 * state (AF DAC alive), measure the 4815's level through the transmission, and
 * ask the one question the radio cannot answer (what was audible). */
static void duplex_audio_test(void)
{
    uint16_t lo, hi, tx_min, tx_max;
    uint32_t rssi_idle, rssi_tx, l73_idle, l74_idle, l73_tx, l74_tx;
    unsigned i;

    uart_puts("\n[duptest] automated duplex audio test (beep state)\n");
    if (!tx_duplex_active())
        duplex_force_on();
    if (!tx_duplex_active()) {
        uart_puts("[duptest] duplex setup failed\n");
        return;
    }

    /* The 2026-10-17 self-test: a UHF transmit desenses a VHF receive lane
     * (floor 17 -> 41), the reverse layout holds -- warn when the split is the
     * known-bad way round so a "beep only" verdict is not mistaken for an
     * audio-route failure. */
    if (gRxVfo->freq_config_RX.Frequency < gTxVfo->freq_config_TX.Frequency)
        uart_puts("[duptest] note: VHF downlink / UHF uplink is the layout that "
                  "desenses the receive lane\n");

    uart_puts("[duptest] waiting up to 20 s for the downlink carrier on ");
    uart_printf("%u.%05u MHz -- key radio A now\n",
                (unsigned)(gRxVfo->freq_config_RX.Frequency / 100000u),
                (unsigned)(gRxVfo->freq_config_RX.Frequency % 100000u));
    {
        unsigned best = 0;

        for (i = 0; i < 100u; i++) {
            const uint16_t r = bk4815_read_rssi();

            if (r > best)
                best = r;
            if (r >= 30u)
                break;
            if ((i % 5u) == 4u)
                uart_printf("[duptest]   waiting... rssi %u (best %u)\n",
                            (unsigned)r, best);
            systick_delay_ms(200);
        }
        if (i == 100u) {
            uart_printf("[duptest] no carrier (best rssi %u) -- key radio A and retry\n",
                        best);
            return;
        }
    }

    /* The console blocks the app's poller: open the receive gates here and
     * make sure the demodulator set is applied (re-asserting is cheap, and a
     * duplex state entered before this run may have left it knocked out). */
    bk4815_enter_rx();
    rx_4815_set_applied();
    bk4815_set_af(true);
    BK4819_SetAF(BK4819_AF_FM);
    audio_path_drive(1);
    systick_delay_ms(300);

    rssi_idle = duptest_measure(2, &lo, &hi, &l73_idle, &l74_idle);
    uart_printf("[duptest] idle: rssi %u (min %u max %u) lvl73 %u lvl74 %u\n",
                (unsigned)rssi_idle, (unsigned)lo, (unsigned)hi,
                (unsigned)l73_idle, (unsigned)l74_idle);

    tx_duplex_set_af(1);          /* Normal AF (0x47 = 0x6142): the node stays
                                   * unloaded -- mode 0 mutes the downlink */
    /* The AF DAC (and so the beep) is whatever 'H' last set: the mix/level
     * question needs both states, so the test must not force one. */
    uart_printf("[duptest] keying the microphone on %u.%05u MHz (%s, %s -- 'H' toggles the beep, 'I' the TX AF)\n",
                (unsigned)(gTxVfo->freq_config_TX.Frequency / 100000u),
                (unsigned)(gTxVfo->freq_config_TX.Frequency % 100000u),
                tx_duplex_af_name(), tx_duplex_af_dac_name());
    tx_start(gTxVfo->freq_config_TX.Frequency,
             gTxVfo->TXP_CalculatedSetting, TX_SOURCE_MIC);

    rssi_tx = duptest_measure(3, &lo, &hi, &l73_tx, &l74_tx);
    tx_min = lo;
    tx_max = hi;
    uart_printf("[duptest] tx:   rssi %u (min %u max %u) lvl73 %u lvl74 %u\n",
                (unsigned)rssi_tx, (unsigned)lo, (unsigned)hi,
                (unsigned)l73_tx, (unsigned)l74_tx);
    uart_printf("[duptest] TX wrote 0x47=%04X 0x30=%04X (AF DAC %s)\n",
                (unsigned)tx_duplex_af_value(),
                (unsigned)tx_duplex_reg30_value(),
                tx_duplex_af_dac() ? "on" : "off");

    tx_stop();
    systick_delay_ms(500);

    uart_puts("[duptest] listening question: during the transmission, what did you hear?\n"
              "          1 = radio A's audio (under/over the beep)\n"
              "          2 = the beep only   3 = silence\n"
              "[duptest] answer 1/2/3 (15 s): ");
    const int a = rxdiag_ask("123", 15000u);
    uart_printf("[duptest] answer %c\n", rxdiag_char(a));

    /* The 0x49/0x4A level meters do not track the carrier (the guided
     * diagnostic measured them as noise), so the verdict weighs the ear answer
     * against the only metric that does: the carrier's own RSSI during the TX,
     * against its idle value (the self-test's ±4 counts).  Three lane states:
     * held (min stayed with the carrier), collapsed (the carrier left
     * entirely), flickered (the carrier seen at full level but dropping to the
     * noise floor -- an intermittent block). */
    {
        const bool with_carrier = (rssi_idle >= 8u);
        const bool min_held  = !with_carrier ||
                               ((int32_t)tx_min  >= (int32_t)rssi_idle - 4);
        const bool max_held  = !with_carrier ||
                               ((int32_t)tx_max  >= (int32_t)rssi_idle - 4);
        const char *lane = min_held ? "held"
                         : max_held ? "flickered"
                                    : "collapsed";

        uart_printf("[duptest] downlink lane: idle %u, during TX min %u max %u -- %s\n",
                    (unsigned)rssi_idle, (unsigned)tx_min, (unsigned)tx_max, lane);

        if (a == '1' && min_held)
            uart_puts("[duptest] VERDICT: receive audio is audible during TX -- duplex audio works\n");
        else if (a == '1')
            uart_puts("[duptest] VERDICT: audio heard, but the downlink RSSI did not hold "
                      "through the TX -- treat with suspicion, run again\n");
        else if (a == '2' && min_held)
            uart_puts("[duptest] VERDICT: beep only with the lane held -- the beep masks the "
                      "downlink (mix/level); try 'H' off to drop the beep\n");
        else if (a == '2' && max_held)
            uart_puts("[duptest] VERDICT: beep only with the lane flickering -- the TX "
                      "intermittently blocks the receive lane; rerun with radio A keyed "
                      "continuously, then try 'O' 1\n");
        else if (a == '2')
            uart_puts("[duptest] VERDICT: beep only because the TX blocks the receive lane "
                      "(a band-layout desense, not an audio gate)\n");
        else if (a == '3' && min_held)
            uart_puts("[duptest] VERDICT: silent with the lane held -- an audio gate is shut, "
                      "not RF (check 0x47/0x49/PC13)\n");
        else
            uart_puts("[duptest] VERDICT: silent with the downlink RSSI degraded -- the "
                      "transmission disturbs the receive lane\n");
    }
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
        if (SETTINGS_FetchChannelScanDisplayInfo(gEeprom.RX_VFO, 0, &info))
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

/* The scan mode the SetScn menu shows (scan_mode_t). */
static const char *scan_mode_name(void)
{
    switch (gSetting_set_scn) {
        case SCAN_MODE_FAST_BOTH: return "FAST BOTH";
        case SCAN_MODE_FAST:      return "FAST";
        default:                  return "NORMAL";
    }
}

/* A controlled Default-vs-Both range-scan comparison, run by the console's 'J'.
 * It forces the selected RX VFO onto a frequency channel over one shared range,
 * then runs each mode for SCAN_BENCH_MS, pumping the same app loop the main loop
 * runs, and reports probes over elapsed wall time.  The timing is wall-clock, so
 * the display and bit-banged bus cost are included, as they are when scanning by
 * hand.  It restores the saved VFO, range and scan mode before returning. */
#define SCAN_BENCH_MS 3000u

/* Its caller was the 'J' state capture, which is read-only now (the benchmark
 * leaves the radio retuned).  Kept compiled for the scan work it exists for. */
static void scan_rate_benchmark(void) __attribute__((unused));

static void scan_rate_benchmark(void)
{
    const uint32_t step  = gRxVfo->StepFrequency ? gRxVfo->StepFrequency : 2500u;
    const uint32_t start = FREQUENCY_RoundToStep(gRxVfo->freq_config_RX.Frequency, (uint16_t)step);
    const uint32_t stop  = start + 100000u;   /* 1 MHz, in 10 Hz units */
    static const char *const names[2] = { "FAST     ", "FAST BOTH" };
    scan_bench_result_t res[2];
    unsigned m;

    uart_printf("\nscan benchmark: %u.%05u .. %u.%05u MHz, step %u.%01u kHz, %u ms per mode\n",
                (unsigned)(start / 100000u), (unsigned)(start % 100000u),
                (unsigned)(stop / 100000u), (unsigned)(stop % 100000u),
                (unsigned)(step / 100u), (unsigned)((step % 100u) / 10u),
                (unsigned)SCAN_BENCH_MS);
    uart_printf("  ScnRev=%u  SetScn=%s\n",
                (unsigned)gEeprom.SCAN_RESUME_MODE, scan_mode_name());

    CHFRSCANNER_BenchmarkArm(start, stop);
    for (m = 0; m < 2u; m++) {
        uint32_t t0;

        CHFRSCANNER_BenchmarkRun((uint8_t)m);
        t0 = systick_millis();
        while ((uint32_t)(systick_millis() - t0) < SCAN_BENCH_MS) {
            APP_Update();
            APP_TimeSlice10ms();
        }
        CHFRSCANNER_BenchmarkFinish(&res[m]);
        uart_printf("  %s probes=%u elapsed=%u ms  %u probes/s\n",
                    names[m], (unsigned)res[m].probes, (unsigned)res[m].elapsed_ms,
                    (unsigned)res[m].probes_per_second);
    }
    CHFRSCANNER_BenchmarkDisarm();

    if (res[0].probes_per_second > 0u) {
        const uint32_t pct = (res[1].probes_per_second * 100u) / res[0].probes_per_second;

        uart_printf("  FAST BOTH / FAST = %u.%02ux  (%s)\n",
                    (unsigned)(pct / 100u), (unsigned)(pct % 100u),
                    (res[1].probes_per_second > res[0].probes_per_second)
                        ? "faster" : "not faster");
    } else {
        uart_puts("  FAST measured 0 probes/s: check squelch and the range\n");
    }
}

/* The console's 'J': the live dual-RF state, without re-tuning anything.  Shows
 * the per-VFO transceiver choice, the resolved roles, which chip supplies the
 * receive audio, the squelch state, and the BK4815's own registers/meters, then
 * samples its 0x44 RSSI.  This is the one that reflects what the running radio
 * is actually doing. */
static void dual_rf_diag(void)
{
    const rf_xcvr_t a = SETTINGS_GetVfoTransceiver(0u);
    const rf_xcvr_t b = SETTINGS_GetVfoTransceiver(1u);
    const bool      a_4829 = (a != RF_XCVR_BK4815);
    const bool      b_4829 = (b != RF_XCVR_BK4815);
    const rf_dual_roles_t roles = rf_dual_choose(a_4829, b_4829, gEeprom.RX_VFO);
    const uint16_t  af  = bk4815_read_reg(BK4815_REG_AF);

    uart_puts("\ndual RF\n");
    uart_printf("  TrVfoA=%u TrVfoB=%u   TX_VFO=%u RX_VFO=%u\n",
                (unsigned)a, (unsigned)b,
                (unsigned)gEeprom.TX_VFO, (unsigned)gEeprom.RX_VFO);
    {
        scan_dual_stats_t st;

        CHFRSCANNER_GetScanDualStats(&st);
        uart_printf("  SetScn=%s (%u)   scan-source override=%u   scan dual active=%d hit=%u\n",
                    scan_mode_name(), (unsigned)gSetting_set_scn,
                    (unsigned)rx_scan_source_override(),
                    (int)st.active, (unsigned)st.selected_hit);
        uart_printf("  scan probes=%u elapsed=%u ms rate=%u candidates/s\n",
                    (unsigned)st.rate.candidates, (unsigned)st.rate.elapsed_ms,
                    (unsigned)st.rate.candidates_per_second);
        uart_printf("    lane BK4829: cand=%u last=%u.%05u MHz rssi=%u\n",
                    (unsigned)st.candidates[0],
                    (unsigned)(st.last_frequency_10hz[0] / 100000u),
                    (unsigned)(st.last_frequency_10hz[0] % 100000u),
                    (unsigned)st.last_rssi[0]);
        uart_printf("    lane BK4815: cand=%u last=%u.%05u MHz rssi=%u\n",
                    (unsigned)st.candidates[1],
                    (unsigned)(st.last_frequency_10hz[1] / 100000u),
                    (unsigned)(st.last_frequency_10hz[1] % 100000u),
                    (unsigned)st.last_rssi[1]);
    }
    uart_printf("  tuned: BK4829<-VFO %d   BK4815<-VFO %d   bk4815_active=%d\n",
                (int)roles.bk4829_vfo, (int)roles.bk4815_vfo,
                (int)rf_dual_bk4815_active());
    uart_printf("  audio source: %s   squelch %s   rssi 0x%03X\n",
                rx_audio_is_4815() ? "BK4815" : "BK4829",
                rx_squelch_open() ? "OPEN" : "closed", (unsigned)rx_rssi());
    uart_printf("  VFO A %u.%05u MHz   VFO B %u.%05u MHz\n",
                (unsigned)(gEeprom.VfoInfo[0].freq_config_RX.Frequency / 100000u),
                (unsigned)(gEeprom.VfoInfo[0].freq_config_RX.Frequency % 100000u),
                (unsigned)(gEeprom.VfoInfo[1].freq_config_RX.Frequency / 100000u),
                (unsigned)(gEeprom.VfoInfo[1].freq_config_RX.Frequency % 100000u));
    uart_printf("  TX power: TX_VFO OUTPUT_POWER=%u  TXP_CalculatedSetting=%u  (menu 'Power'; USER global=%u)\n",
                (unsigned)gTxVfo->OUTPUT_POWER, (unsigned)gTxVfo->TXP_CalculatedSetting,
                (unsigned)gSetting_set_pwr);
    uart_printf("  BK4829: 0x38=0x%04X 0x39=0x%04X -> %u.%05u MHz  0x47(AF)=0x%04X 0x48(gain)=0x%04X 0x50=0x%04X 0x67(RSSI)=0x%04X 0x33=0x%04X\n",
                (unsigned)BK4819_ReadRegister(0x38), (unsigned)BK4819_ReadRegister(0x39),
                (unsigned)((((uint32_t)BK4819_ReadRegister(0x39) << 16) | BK4819_ReadRegister(0x38)) / 100000u),
                (unsigned)((((uint32_t)BK4819_ReadRegister(0x39) << 16) | BK4819_ReadRegister(0x38)) % 100000u),
                (unsigned)BK4819_ReadRegister(0x47), (unsigned)BK4819_ReadRegister(0x48),
                (unsigned)BK4819_ReadRegister(0x50),
                (unsigned)BK4819_ReadRegister(0x67),
                (unsigned)BK4819_ReadRegister(0x33));
    uart_printf("  BK4815: 0x04=0x%04X 0x0C=0x%04X 0x49=0x%04X (AF %s)\n",
                (unsigned)bk4815_read_reg(0x04), (unsigned)bk4815_read_reg(0x0C),
                (unsigned)af, (af & 0x8000u) ? "MUTED" : "unmuted");
    uart_printf("          set: 0x16=0x%04X 0x2B=0x%04X 0x6D=0x%04X 0x73=0x%04X"
                "  (expect B200 403F 6618 81D0)\n",
                (unsigned)bk4815_read_reg(0x16), (unsigned)bk4815_read_reg(0x2B),
                (unsigned)bk4815_read_reg(0x6D), (unsigned)bk4815_read_reg(0x73));
    uart_printf("          0x70=0x%04X 0x71=0x%04X 0x72=0x%04X 0x75=0x%04X\n",
                (unsigned)bk4815_read_reg(0x70), (unsigned)bk4815_read_reg(0x71),
                (unsigned)bk4815_read_reg(0x72), (unsigned)bk4815_read_reg(0x75));
    uart_printf("          0x43(SNR)=0x%04X 0x44(RSSI)=0x%04X -> %u  (marks open 0x%02X close 0x%02X)\n",
                (unsigned)bk4815_read_reg(0x43), (unsigned)bk4815_read_reg(0x44),
                (unsigned)(bk4815_read_reg(0x44) & 0x7fu),
                (unsigned)RX4815_SQUELCH_OPEN_MARK, (unsigned)RX4815_SQUELCH_CLOSE_MARK);
    uart_printf("  swap=%d  duplex=%d  tx active=%d  source=%s\n",
                (int)tx_swap(), (int)tx_duplex_active(), (int)tx_active(),
                tx_source() == TX_SOURCE_TONE ? "tone" : "mic");

    /* The write log: the last BK4815 accesses, most recent first.  This is the
     * state capture for the "second reception is noise" scenario -- whichever
     * code wrote the corrupted demod set (0x16 = 0x3200) has to be in here. */
    uart_printf("  4815 writes: %u total, last:\n", bk4815_log_total());
    {
        uint32_t ms;
        uint8_t  reg;
        uint16_t value;
        bool     block;
        unsigned back;

        for (back = 0; back < 24u; back++) {
            if (!bk4815_log_entry(back, &ms, &reg, &value, &block))
                break;
            if (block)
                uart_printf("    %10u ms  block @[0x%02X] len=%u\n",
                            (unsigned)ms, (unsigned)reg, (unsigned)value);
            else
                uart_printf("    %10u ms  0x%02X = 0x%04X\n",
                            (unsigned)ms, (unsigned)reg, (unsigned)value);
        }
    }

    /* The 0x44 sampling and the scan benchmark need a live signal and leave
     * the radio disturbed; 'J' is used as a state capture (first vs second
     * reception), so it stays read-only. */
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

/* Console 'T': what the transmit path is actually doing, without keying it.
 * Transmit is PTT's (or the bench's) -- this is the read-out: the PA bias PWM
 * compare in force, the power the app would ask for, the chip band/path pin
 * ('x' changes it), and the TX registers (`0x43` is the TX filter, `0x75` the
 * BK4815's band, `0x0C` its T/R state). */
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
    /* Apply the per-VFO transceiver choice: if a VFO selects the BK4815, tune
     * it for that VFO alongside the BK4829 primary. */
    rf_dual_refresh();
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
#ifdef ENABLE_CAT
        cat_input_service(now);
        int c = cat_console_getc();
#else
        int c = uart_getc();
#endif

        if (c >= 0) {
            char ch = (char)c;

            /* echo and remember the last characters on the display */
            if (ch == '\r' || ch == '\n') {
                uart_puts("\n");
            } else {
                uart_printf("%c", ch);
            }

            /* Full-duplex console: everything unrelated is gated off on this
             * build, except the read-only diagnostics ('i', 'J', 'd') -- they
             * write nothing, which makes them the state capture for the
             * app-mode receive scenarios.  The cases stay compiled (they
             * share helpers) but the writers stay unreachable. */
            if (ch != '\r' && ch != '\n' &&
                strchr("hUIHgOzEo7TNyiJd", ch) == NULL) {
                uart_puts("\nfull-duplex console -- 'h' lists the commands\n");
                continue;
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
            case 'J':
                dual_rf_diag();
                break;
            case 'A':
                rx_force_bk4815_af(!rx_force_bk4815_af_on());
                uart_printf("\nBK4815 AF forced %s\n",
                            rx_force_bk4815_af_on() ? "ON (squelch bypassed)" : "off");
                break;
            case 'U':
                if (!tx_duplex_active()) {
                    duplex_force_on();
                    uart_puts("\nduplex ON (config forced): BK4815 receives, BK4829 transmits\n"
                              "on the other VFO at PTT. Press U again to restore the bindings.\n");
                    duplex_status();
                } else {
                    duplex_force_off();
                    uart_puts("\nduplex OFF (bindings restored)\n");
                }
                break;
            case 'I':
                tx_duplex_next_af();
                uart_printf("\nduplex TX-chip AF -> %s\n", tx_duplex_af_name());
                break;
            case 'O':
                tx_duplex_next_path();
                uart_printf("\nduplex band path -> %s\n", tx_duplex_path_name());
                break;
            case 'N': {
                /* Deterministic transmit: the chip's own tone on the transmit
                 * VFO, independent of the microphone -- tells "the PA/TX chain
                 * does not radiate" apart from "the mic path is wrong". */
                const uint32_t f = gTxVfo->freq_config_TX.Frequency;

                uart_printf("\nTX tone test on %u.%05u MHz for 2 s (duplex %d)\n",
                            (unsigned)(f / 100000u), (unsigned)(f % 100000u),
                            tx_duplex_active() ? 1 : 0);
                tx_start(f, gTxVfo->TXP_CalculatedSetting, TX_SOURCE_TONE);
                systick_delay_ms(2000);
                tx_stop();
                uart_puts("done -- listen on the uplink frequency\n");
                break;
            }
            case 'E':
                duplex_selftest();
                break;
            case 'H':
                tx_duplex_set_af_dac(!tx_duplex_af_dac());
                uart_printf("\nduplex %s\n", tx_duplex_af_dac_name());
                break;
            case 'g': {
                const uint8_t mode = (uint8_t)((tx_duplex_pc13() + 1u) % 3u);

                tx_duplex_set_pc13(mode);
                uart_printf("\nduplex %s\n", tx_duplex_pc13_name());
                break;
            }
            case 'z':
                rx_set_4815_bridge(!rx_4815_bridge());
                uart_printf("\n4815->4829 audio bridge %s\n",
                            rx_4815_bridge() ? "ON (the 4829 AF opens with the 4815 squelch)"
                                             : "OFF (4829 muted: its noise cannot mask 4815 audio)");
                break;
            case 'o': {
                /* Route probe (docs/ra89r_fullduplex.md, "V52 static
                 * follow-up"): does the BK4829's microphone input carry the
                 * BK4815's receive audio?  Needs duplex mode (U) so the receive
                 * lane is the 4815.  The speaker is muted, the 4815's own AF is
                 * opened, and the 4829's mic path runs as at transmit entry --
                 * without the PA, so this neither keys nor radiates.
                 *
                 * The operator's own voice proves the reading first: a muted
                 * 4815 AF or an unpowered mic path would otherwise look
                 * exactly like "no route". */
                if (!tx_duplex_active()) {
                    uart_puts("\nroute probe needs duplex mode: send U first (it forces the\n"
                              "receive VFO onto the BK4815)\n");
                    break;
                }
                if (!tx_route_probe_begin()) {
                    uart_puts("\nroute probe refused: duplex is not RX=4815 / TX=4829\n");
                    break;
                }

                const uint32_t rf =
                    gEeprom.VfoInfo[gEeprom.RX_VFO].freq_config_RX.Frequency;

                uart_printf("\n[route] downlink %u.%05u MHz: speaker muted, 4815 AF open\n",
                            (unsigned)(rf / 100000u), (unsigned)(rf % 100000u));

                /* 1) quiet level: no carrier, nobody talking */
                uart_puts("[route] phase 1/4: stay quiet and do NOT transmit (2 s)\n");

                uint32_t amp_sum = 0, rssi_sum = 0;

                for (unsigned i = 0; i < 8u; i++) {
                    systick_delay_ms(250);
                    amp_sum  += tx_route_probe_vox_amp();
                    rssi_sum += bk4815_read_rssi();
                }
                const uint32_t quiet     = (amp_sum + 4u) / 8u;
                const uint32_t rssi_idle = (rssi_sum + 4u) / 8u;

                /* 2) microphone control: the reading must follow the voice */
                uint32_t voice = 0;
                bool     mic_ok = false;

                for (unsigned round = 0; round < 2u && !mic_ok; round++) {
                    if (round == 1u) {
                        /* The TX word did not respond: try the stock's VOX mic
                         * word before declaring the reading unusable. */
                        tx_route_probe_mic_mode(1);
                    }
                    uart_printf("[route] phase 2/4: talk into the radio's microphone (4 s), "
                                "%s\n", round ? "stock VOX mic word" : "TX mic word");

                    uint32_t v = 0;

                    for (unsigned i = 0; i < 16u; i++) {
                        systick_delay_ms(250);
                        v += tx_route_probe_vox_amp();
                    }
                    voice  = (v + 8u) / 16u;
                    mic_ok = voice > quiet + 16u;
                }

                uart_printf("[route] mic control: quiet %u, voice %u -- %s (%s)\n",
                            (unsigned)quiet, (unsigned)voice,
                            mic_ok ? "responsive" : "NO RESPONSE",
                            tx_route_probe_mic_mode_get() ? "stock VOX word" : "TX word");

                if (!mic_ok) {
                    tx_route_probe_end();
                    uart_puts("[route] inconclusive: 0x64 did not follow the microphone in either\n"
                              "[route] mic mode, so it cannot say where the 4815's audio goes\n");
                    break;
                }

                /* 3) carrier: same reading, with and without the downlink */
                uart_printf("[route] idle rssi4815=%u | phase 3/4: key radio A on the\n"
                            "[route] downlink now, do not talk (12 s)\n",
                            (unsigned)rssi_idle);

                uint32_t sum_on = 0, n_on = 0, sum_off = 0, n_off = 0;

                for (unsigned i = 0; i < 48u; i++) {
                    const uint32_t rssi = bk4815_read_rssi();
                    const uint16_t amp  = tx_route_probe_vox_amp();
                    const bool     on   = rssi >= rssi_idle + 8u;

                    if (on) { sum_on += amp; n_on++; }
                    else    { sum_off += amp; n_off++; }

                    if ((i % 4u) == 0u) {
                        uart_printf("[route] t=%us rssi4815=%u amp=%u carrier=%u "
                                    "4815 0x0C=%04X\n",
                                    (unsigned)(i / 4u + 1u), (unsigned)rssi,
                                    (unsigned)amp, on ? 1u : 0u,
                                    (unsigned)bk4815_read_reg(0x0Cu));
                    }
                    systick_delay_ms(250);
                }
                tx_route_probe_end();

                const uint32_t mean_on  = n_on  ? sum_on  / n_on  : 0u;
                const uint32_t mean_off = n_off ? sum_off / n_off : 0u;

                uart_printf("[route] mic amp (0x64): carrier %u (n=%u) | no carrier %u (n=%u)\n",
                            (unsigned)mean_on, (unsigned)n_on,
                            (unsigned)mean_off, (unsigned)n_off);

                if (n_on < 6u || n_off < 6u) {
                    uart_puts("[route] inconclusive: key radio A for part of the window, repeat\n");
                } else if (mean_on > mean_off + mean_off / 2u + 8u) {
                    uart_puts("[route] VERDICT route 2: the 4815's audio IS on the BK4829's mic\n"
                              "[route] input -- a BK4829 transmission relays the downlink on the\n"
                              "[route] air, so a monitor-only satellite uplink needs a hardware change\n");
                } else {
                    uart_puts("[route] VERDICT route 1: no 4815 audio on the BK4829's mic input;\n"
                              "[route] the downlink is only summed at the amplifier, so I=1 with\n"
                              "[route] H=off keeps it audible during a BK4829 transmission\n");
                }
                break;
            }
            case 'y': {
                /* Swap mode: the stock's arrangement -- BK4829 receives (its AF
                 * stays valid, so the downlink is audible) and the BK4815
                 * transmits with the stock's 4815 TX set. */
                static bool      sv_valid;
                static rf_xcvr_t sv_rx;
                static rf_xcvr_t sv_tx;
                static uint8_t   sv_txvfo;
                static uint8_t   sv_rxvfo;

                if (!tx_swap()) {
                    const uint8_t rx_vfo = gEeprom.RX_VFO;
                    const uint8_t tx_vfo = (uint8_t)(1u - rx_vfo);

                    sv_rx      = SETTINGS_GetVfoTransceiver(rx_vfo);
                    sv_tx      = SETTINGS_GetVfoTransceiver(tx_vfo);
                    sv_txvfo   = gEeprom.TX_VFO;
                    sv_rxvfo   = rx_vfo;
                    sv_valid   = true;

                    tx_set_duplex(false, false, false);
                    SETTINGS_SetVfoTransceiver(rx_vfo, RF_XCVR_BK4829);
                    SETTINGS_SetVfoTransceiver(tx_vfo, RF_XCVR_BK4815);
                    gEeprom.TX_VFO = tx_vfo;
                    tx_set_swap(true);
                    SETTINGS_FixupVfoPointers();
                    rx_set_frequency(gRxVfo->freq_config_RX.Frequency);  /* the 4829 */

                    uart_puts("\nswap ON: BK4829 receives, BK4815 transmits on PTT\n"
                              "the 4829 is never written during a transmission\n");
                    uart_printf("  RX VFO%u BK4829 @ %u.%05u MHz | TX VFO%u BK4815 @ %u.%05u MHz\n",
                                (unsigned)rx_vfo,
                                (unsigned)(gRxVfo->freq_config_RX.Frequency / 100000u),
                                (unsigned)(gRxVfo->freq_config_RX.Frequency % 100000u),
                                (unsigned)tx_vfo,
                                (unsigned)(gTxVfo->freq_config_TX.Frequency / 100000u),
                                (unsigned)(gTxVfo->freq_config_TX.Frequency % 100000u));
                } else {
                    tx_set_swap(false);
                    if (sv_valid) {
                        SETTINGS_SetVfoTransceiver(sv_rxvfo, sv_rx);
                        SETTINGS_SetVfoTransceiver((uint8_t)(1u - sv_rxvfo), sv_tx);
                        gEeprom.TX_VFO = sv_txvfo;
                        sv_valid = false;
                    }
                    SETTINGS_FixupVfoPointers();
                    uart_puts("\nswap OFF (bindings restored)\n");
                }
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
            case '7':
                rxdiag_run();
                break;
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
                duplex_audio_test();
                break;
            case 'x': {
                /* The TX band/path pin (`0x33` bit 0x40 VHF / 0x20 UHF).  AUTO
                 * is the stock's rule; the other modes force one so the radio
                 * can settle which pin the PA actually needs. */
                uint8_t mode = (uint8_t)((pa_tx_path_mode() + 1u) % PA_TX_PATH_MODES);

                pa_set_tx_path_mode(mode);
                uart_printf("\nTX path pin: mode %u -- %s (band bits 0x%04X)\n",
                            (unsigned)mode, pa_tx_path_name(),
                            (unsigned)pa_tx_path_bits());
                uart_puts("  hold PTT and press 'T' to read 0x33 and the PA duty back\n");
                break;
            }
            case 'w':
                /* The stock TX register sweep: the K1 chain omits these, so step
                 * them while PTT is held and watch the power meter. */
                tx_variant_next();
                uart_printf("\nTX variant: %u -- %s\n",
                            (unsigned)tx_variant(), tx_variant_name());
                if (tx_active())
                    tx_variant_apply();
                uart_puts("  hold PTT, press 'w' to step, and watch the meter\n");
                break;
            case 'Y': {
                /* The PA bias PWM compare: the one transmit level worth tuning
                 * by ear or S-meter now that the amplifier works.  The steps
                 * run past the stock's clamp (ARR/2 = 719, the 50 % the app's
                 * own power setting reaches) so the full range can be swept. */
                static const uint16_t steps[] = { 64, 128, 192, 256, 384, 512, 719 };
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

        duplex_tick(now);

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
