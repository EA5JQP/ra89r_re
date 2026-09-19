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

void uart_init(uint32_t baud)
{
    /* PB6 = USART1_TX, PB7 = USART1_RX, alternate function 2.  The stock
     * firmware configures both with a pull-up; UART idle is high, so a
     * pull-up on RX is the safe default. */
    gpio_config_af(BOARD_UART_PORT, BOARD_UART_TX_PIN | BOARD_UART_RX_PIN,
                   BOARD_UART_AF, 1u);

    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;

    /* BRR keeps USARTDIV in 4.4 fixed point; with 16x oversampling
     * BRR = PCLK / baud (rounded). */
    BOARD_UART->BRR = (BOARD_APB2_HZ + (baud / 2u)) / baud;

    BOARD_UART->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;

    /* Flush anything the (possibly reset) peer left in the shift register. */
    while (BOARD_UART->SR & USART_SR_RXNE)
        (void)BOARD_UART->DR;
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
        if (*fmt != '%') {
            uart_putc(*fmt++);
            continue;
        }
        fmt++;
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
                put_u32((uint32_t)(-v), 10u, 0);
            } else {
                put_u32((uint32_t)v, 10u, 0);
            }
            break;
        }
        case 'u':
            put_u32(va_arg(ap, uint32_t), 10u, 0);
            break;
        case 'x':
            put_u32(va_arg(ap, uint32_t), 16u, 0);
            break;
        case 'X':
            put_u32(va_arg(ap, uint32_t), 16u, 8);
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
    return (BOARD_UART->SR & USART_SR_RXNE) != 0u;
}

int uart_getc(void)
{
    if (!uart_rx_ready())
        return -1;
    return (int)(BOARD_UART->DR & 0xFFu);
}

int uart_getc_timeout(uint32_t ms)
{
    int c = uart_getc();

    while (ms-- > 0u) {
        if (c >= 0)
            return c;
        systick_delay_ms(1u);
        c = uart_getc();
    }
    return c;
}
