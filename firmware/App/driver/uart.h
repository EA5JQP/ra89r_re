/* USART1 console on the radio's programming port (PB6 TX / PB7 RX, AF2). */
#ifndef DRIVER_UART_H
#define DRIVER_UART_H

#include <stdint.h>

void uart_init(uint32_t baud);
void uart_putc(char c);
void uart_write(const char *buf, uint32_t len);
void uart_puts(const char *s);
void uart_printf(const char *fmt, ...);

/* Non-blocking receive. */
int uart_rx_ready(void);
int uart_getc(void);            /* -1 if nothing waiting */

/* Blocking receive with timeout in milliseconds, -1 on timeout. */
int uart_getc_timeout(uint32_t ms);

#endif /* DRIVER_UART_H */
