/* Diagnostic console that works before SystemInit() runs.
 *
 * Called from Reset_Handler (see Core/startup_py32f403xx.s) so that anything
 * that hangs or resets before main() -- SystemInit, .data/.bss init, the C
 * runtime -- still leaves a trace on the UART.  It assumes the reset-default
 * clock (HSI 8 MHz) and is replaced by uart_init() in main(). */
#ifndef DRIVER_EARLY_H
#define DRIVER_EARLY_H

void early_boot_banner(void);

#endif /* DRIVER_EARLY_H */
