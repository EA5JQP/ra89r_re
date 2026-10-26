/* The K1's UART surface on this board.
 *
 * Port adaptation (from the validated ra89r UART).  The K5V3 file talks its
 * own LL fork (LL_USART_InitTypeDef, DMA structs) and carries its board's
 * pins (PA9/PA10).  This board has no USB and the console is USART1 on the
 * kenwood jack (PB6/PB7, 115200 8N1 -- bootloader on the same jack at 9600),
 * so the body is the register-level driver of the old port and only the three
 * K1 names are mapped on top of it.
 *
 * `UART_Send` is blocking-copy into the K1's DMA buffer shape; the old port's
 * write path already queues through a 128-byte ring (see f4hwn/board/uart
 * headers), which is what the K1's app/uart.c protocol expects.
 */
#include "driver/uart.h"

#include <string.h>

/* The K1 app/uart.c protocol's DMA-shaped buffer; on this board the console
 * writes through the old-port 128-byte ring, so this is the K1 surface's hold
 * (upstream driver/uart.h line 27 declares it). */
uint8_t UART_DMA_Buffer[256];

#include "board.h"
#include "uart_k1_port.h"   /* the old-port surface (uart_init/uart_write) */

void UART_Init(void)
{
    /* 115200 8N1, the jack the bootloader probes at 9600 (tools/FLASHING.md).
     * `BOARD_UART` is the USART1 instance named in board.h. */
    uart_init(115200u);
}

void UART_Send(const void *pBuffer, uint32_t Size)
{
    uart_write((const char *)pBuffer, Size);
}

void UART_LogSend(const void *pBuffer, uint32_t Size)
{
    /* Same port: there is no second channel to keep the log off. */
    uart_write((const char *)pBuffer, Size);
}
