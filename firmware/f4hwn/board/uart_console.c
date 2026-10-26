#include <stdarg.h>
#include <stdint.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "driver/uart.h"

/* USART1 register layout (vendor header): SR 0x00, DR 0x04, BRR 0x08, CR1 0x0C.
 * Clock is on APB2 (RCC_APB2ENR bit 14, RCC_APB2ENR_USART1EN) and this
 * firmware runs with an APB2 prescaler of 1, so PCLK2 == SystemCoreClock.
 *
 * The stock firmware (and its bootloader, which initialises 9600) uses the
 * same USART1 on PB6/PB7 with AF2 -- see board.h. */

static void putc_raw(char c)
{
    while (!(BOARD_UART->SR & USART_SR_TXE))
        ;
    BOARD_UART->DR = (uint32_t)(uint8_t)c;
}

/* --- Receive: interrupt-driven into a ring buffer ---------------------------
 *
 * The USART has a single-byte receive register and no FIFO.  Polling it once
 * per main-loop pass drops bytes whenever a sender sends a burst faster than
 * the loop iterates -- and a K1 app pass is far longer than one byte at 115200.
 * CAT frames are exactly such a burst (5 bytes back to back), so receive is
 * interrupt-driven here and the callers drain the buffer. */
#define RX_BUF_LEN 256u
#define RX_BUF_MASK (RX_BUF_LEN - 1u)

static volatile uint8_t  rx_buf[RX_BUF_LEN];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;

void USART1_IRQHandler(void)
{
    if ((BOARD_UART->SR & USART_SR_RXNE) != 0u) {
        uint8_t  b    = (uint8_t)(BOARD_UART->DR & 0xFFu);
        uint16_t next = (uint16_t)((rx_head + 1u) & RX_BUF_MASK);

        if (next == rx_tail)                       /* full: drop the oldest */
            rx_tail = (uint16_t)((rx_tail + 1u) & RX_BUF_MASK);

        rx_buf[rx_head] = b;
        rx_head         = next;
    }
}

void uart_init(uint32_t baud)
{
    /* PB6 = USART1_TX, PB7 = USART1_RX, alternate function 2.  The stock
     * firmware configures both with a pull-up; UART idle is high, so a
     * pull-up on RX is the safe default. */
    gpio_config_af(BOARD_UART_PORT, BOARD_UART_TX_PIN | BOARD_UART_RX_PIN,
                   BOARD_UART_AF, 1u);

    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;

    /* Take the peripheral over from scratch.  The bootloader leaves USART1
     * configured its own way, and anything it left enabled -- DMA requests,
     * RTS/CTS flow control, a different stop-bit count -- keeps bytes from ever
     * reaching RXNE, i.e. the console looks alive but ignores the keyboard.
     * So: disable, clear the error/overrun flags, then configure 8N1 with no
     * flow control and no DMA. */
    BOARD_UART->CR1 = 0;
    BOARD_UART->CR2 = 0;
    BOARD_UART->CR3 = 0;
    (void)BOARD_UART->SR;                    /* clearing ORE/FE/NE/PE needs ... */
    (void)BOARD_UART->DR;                    /* ... a read of SR followed by DR */

    /* BRR keeps USARTDIV in 4.4 fixed point; with 16x oversampling
     * BRR = PCLK / baud (rounded). */
    BOARD_UART->BRR = (BOARD_APB2_HZ + (baud / 2u)) / baud;
    BOARD_UART->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;

    /* Drop anything already queued in the receive path. */
    while (BOARD_UART->SR & USART_SR_RXNE)
        (void)BOARD_UART->DR;

    /* Receive is interrupt-driven (the part has no RX FIFO): every byte lands
     * in the ring buffer, so a burst is not lost while the main loop is busy. */
    rx_head = 0;
    rx_tail = 0;
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_EnableIRQ(USART1_IRQn);
}

void uart_putc(char c)
{
    if (c == '\n')
        putc_raw('\r');
    putc_raw(c);
}

void uart_write(const char *buf, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
        uart_putc(buf[i]);
}

/* No \n -> \r\n translation: for raw binary streams (the EEPROM dump). */
void uart_write_raw(const char *buf, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
        putc_raw(buf[i]);
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void put_u32(uint32_t v, uint32_t base, int pad)
{
    char tmp[11];
    int n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v) {
            uint32_t d = v % base;
            tmp[n++] = (char)(d < 10u ? ('0' + d) : ('a' + d - 10u));
            v /= base;
        }
    }
    while (n < pad)
        tmp[n++] = '0';
    while (n--)
        uart_putc(tmp[n]);
}

void uart_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    while (*fmt) {
        int pad = 0;

        if (*fmt != '%') {
            uart_putc(*fmt++);
            continue;
        }
        fmt++;
        /* optional zero-padding width, e.g. "%08X" or "%02X" */
        if (*fmt == '0' || (*fmt >= '1' && *fmt <= '9')) {
            while (*fmt >= '0' && *fmt <= '9')
                pad = pad * 10 + (*fmt++ - '0');
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            uart_puts(s ? s : "(null)");
            break;
        }
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0) {
                uart_putc('-');
                put_u32((uint32_t)(-v), 10u, pad);
            } else {
                put_u32((uint32_t)v, 10u, pad);
            }
            break;
        }
        case 'u':
            put_u32(va_arg(ap, uint32_t), 10u, pad);
            break;
        case 'x':
            put_u32(va_arg(ap, uint32_t), 16u, pad);
            break;
        case 'X':
            put_u32(va_arg(ap, uint32_t), 16u, pad ? pad : 8);
            break;
        case '%':
            uart_putc('%');
            break;
        default:
            uart_putc('%');
            if (*fmt)
                uart_putc(*fmt);
            break;
        }
        if (*fmt)
            fmt++;
    }
    va_end(ap);
}

int uart_rx_ready(void)
{
    return rx_head != rx_tail;
}

int uart_getc(void)
{
    uint8_t b;

    if (rx_head == rx_tail)
        return -1;

    b       = rx_buf[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1u) & RX_BUF_MASK);
    return (int)b;
}

int uart_getc_timeout(uint32_t ms)
{
    uint32_t start = systick_millis();

    /* The bytes are already buffered by the receive interrupt, so this only
     * waits for the buffer to have something. */
    for (;;) {
        int c = uart_getc();
        if (c >= 0)
            return c;
        if ((uint32_t)(systick_millis() - start) > ms)
            return -1;
    }
}
