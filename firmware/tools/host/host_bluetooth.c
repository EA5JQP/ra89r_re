/* Host stand-in for the Bluetooth transport (see driver/bluetooth.h).
 *
 * The preview builds the BT service (`App/app/bt.c`) and the real transport
 * (`App/driver/bluetooth.c`) with -DBLUETOOTH_HOST_TEST, so the transport's
 * USART3 half is compiled out; this supplies the one symbol it still needs.
 * The preview never actually transmits. */
#include <stdint.h>

void bluetooth_hw_write(const uint8_t *data, unsigned len)
{
    (void)data;
    (void)len;
}
