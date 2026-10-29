/* Non-blocking-output UART burst capture; see test_bt_capture.c. */
#ifndef DRIVER_BT_CAPTURE_H
#define DRIVER_BT_CAPTURE_H

#include <stdint.h>

#define BT_CAPTURE_RXNE  (1u << 5)
#define BT_CAPTURE_ERRORS ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3))

typedef struct {
    void *context;
    uint32_t (*millis)(void *context);
    uint32_t (*status)(void *context);
    uint8_t (*read_data)(void *context);
} bt_capture_io_t;

typedef struct {
    unsigned stored;
    unsigned dropped;
    uint32_t errors;
} bt_capture_result_t;

void bt_capture_collect(const bt_capture_io_t *io, uint8_t *buffer,
                        unsigned capacity, uint32_t duration_ms,
                        bt_capture_result_t *result);

#endif /* DRIVER_BT_CAPTURE_H */
