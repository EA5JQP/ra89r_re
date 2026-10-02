/* Newlib stubs for the port.
 *
 * The imported K1 code formats text with sprintf/snprintf (external/printf is
 * mapped to newlib -- see App/external/printf/printf.h), which is the first
 * thing in this firmware to pull in newlib's stdio.  That drags in the
 * syscall layer, so it is provided here:
 *
 *   * _sbrk fails on purpose: nothing in the application allocates (the K1
 *     format strings are all integer/string conversions, so newlib never needs
 *     a buffer), and a failing heap turns a stray malloc into a clean failure
 *     instead of silently eating SRAM.
 *   * _write goes to the UART console, so anything that does end up writing to
 *     stdout (a stray printf, newlib's own diagnostics) is visible on the
 *     console instead of vanishing.
 */
#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "driver/uart.h"

void *_sbrk(ptrdiff_t increment)
{
    (void)increment;
    errno = ENOMEM;
    return (void *)-1;
}

int _write(int file, char *ptr, int len)
{
    int i;

    (void)file;
    for (i = 0; i < len; i++)
        uart_putc((char)ptr[i]);
    return len;
}

int _read(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;
    return 0;
}

int _close(int file)
{
    (void)file;
    return -1;
}

int _fstat(int file, struct stat *st)
{
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file)
{
    (void)file;
    return 1;
}

off_t _lseek(int file, off_t offset, int whence)
{
    (void)file;
    (void)offset;
    (void)whence;
    return 0;
}
