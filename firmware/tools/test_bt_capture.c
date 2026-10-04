/* Regression test for the USART3 diagnostic capture loop.
 *
 * A response arrives as separate UART frames with idle polling between them.
 * The capture must keep polling throughout the window and must not report or
 * print a byte inline (which would block for longer than a UART frame).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/bt_capture.h"

struct fake_uart {
    unsigned polls;
    unsigned reads;
    unsigned clock_calls;
    const uint8_t *reply;
    unsigned reply_len;
};

static uint32_t fake_millis(void *context)
{
    struct fake_uart *fake = context;

    return fake->clock_calls++ / 50u;
}

static uint32_t fake_status(void *context)
{
    struct fake_uart *fake = context;
    uint32_t status = 0;

    fake->polls++;
    /* Frames become ready on distinct polls, with idle polls between. */
    if (fake->reads < fake->reply_len && fake->polls == fake->reads * 8u + 1u)
        status |= BT_CAPTURE_RXNE;
    if (fake->polls == 17u)
        status |= 1u << 1; /* framing-error flag, retained for diagnostics */
    return status;
}

static uint8_t fake_read_data(void *context)
{
    struct fake_uart *fake = context;

    return fake->reply[fake->reads++];
}

int main(void)
{
    static const uint8_t reply[] = { '+', 'O', 'K', '\r', '\n' };
    uint8_t buffer[sizeof reply];
    bt_capture_result_t result;
    struct fake_uart fake = { 0, 0, 0, reply, sizeof reply };
    const bt_capture_io_t io = {
        &fake, fake_millis, fake_status, fake_read_data
    };

    memset(&result, 0, sizeof result);
    bt_capture_collect(&io, buffer, sizeof buffer, 2u, &result);

    if (result.stored != sizeof reply || result.dropped != 0u ||
        memcmp(buffer, reply, sizeof reply) != 0 ||
        (result.errors & (1u << 1)) == 0u) {
        fprintf(stderr, "capture failed: stored=%u dropped=%u errors=0x%X\n",
                result.stored, result.dropped, (unsigned)result.errors);
        return 1;
    }

    puts("1 capture test passed: full delayed reply and error flags retained");
    return 0;
}
