/* Timed, non-blocking-output capture for the Bluetooth UART diagnostic. */
#include "driver/bt_capture.h"

void bt_capture_collect(const bt_capture_io_t *io, uint8_t *buffer,
                        unsigned capacity, uint32_t duration_ms,
                        bt_capture_result_t *result)
{
    uint32_t start;

    if (!result)
        return;
    result->stored = 0;
    result->dropped = 0;
    result->errors = 0;

    if (!io || !io->millis || !io->status || !io->read_data ||
        (!buffer && capacity != 0u))
        return;

    start = io->millis(io->context);
    while ((uint32_t)(io->millis(io->context) - start) < duration_ms) {
        uint32_t status = io->status(io->context);

        result->errors |= status & BT_CAPTURE_ERRORS;
        if (status & BT_CAPTURE_RXNE) {
            uint8_t byte = io->read_data(io->context);

            if (result->stored < capacity)
                buffer[result->stored++] = byte;
            else
                result->dropped++;
        }
    }
}
