/* Host test for the BT service core (App/app/bt.c).
 *
 * The module transport is the real driver/bluetooth.c compiled with
 * -DBLUETOOTH_HOST_TEST, so this exercises the actual command strings; only
 * bluetooth_hw_write() is replaced with a recorder.  No hardware.
 *
 * The service sends one command and waits for its response before the next,
 * exactly as the stock's queue does (`FUN_0802213c` + the parser's advance
 * rules), so the sequence is driven here event by event.
 *
 *   gcc -std=c11 -I App -I App/driver -DBLUETOOTH_HOST_TEST \
 *       tools/test_bt_service.c App/app/bt.c App/driver/bluetooth.c \
 *       -o /tmp/test_bt_service && /tmp/test_bt_service
 */
#include <stdio.h>
#include <string.h>

#include "app/bt.h"
#include "driver/bluetooth.h"

#define MAX_CMDS 40
static char     rec[MAX_CMDS][64];
static unsigned rec_n;

void bluetooth_hw_write(const uint8_t *data, unsigned len)
{
    if (rec_n < MAX_CMDS && len < sizeof rec[0]) {
        memcpy(rec[rec_n], data, len);
        rec[rec_n][len] = '\0';
        rec_n++;
    }
}

static void rec_reset(void) { rec_n = 0; }

static int rec_is(unsigned i, const char *want)
{
    return i < rec_n && strcmp(rec[i], want) == 0;
}

static int rec_last_is(const char *want)
{
    return rec_n > 0 && strcmp(rec[rec_n - 1u], want) == 0;
}

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("  FAIL %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
}

/* Drive the stock config sequence to completion, asserting each step. */
static void drive_config(const char *label)
{
    static const char version[] = "YBT100_FW_V01_02_012";

    bt_service_event(BT_EV_VERSION, version, (unsigned)strlen(version));
    check(rec_last_is("AT+BT=EMITTER\r\n"), "  mode after VERSION");
    bt_service_event(BT_EV_BT_EMITTER, NULL, 0);
    check(rec_last_is(label ? "AT+WRITE_NAME=RA89R\r\n" : "AT+BLE_LOCAL?\r\n"),
          "  after mode");
    if (label) {
        bt_service_event(BT_EV_NAME_EQUALLY, NULL, 0);
        check(rec_last_is("AT+BLE_LOCAL?\r\n"), "  after WRITE_NAME");
    }
    bt_service_event(BT_EV_BLE_LOCAL, "AA:BB:CC", 8u);
    check(rec_last_is("AT+BT_SCANATCN=ON\r\n"), "  after BLE_LOCAL");
    bt_service_event(BT_EV_BT_SCAN_STOP, NULL, 0);
    check(rec_last_is("AT+CONN_STATE?\r\n"), "  after SCANATCN");
    bt_service_event(BT_EV_CONN_STATE, "0", 1u);
    check(rec_last_is("AT+BT_CONN_LAST\r\n"), "  after CONN_STATE");
    bt_service_event(BT_EV_OK, NULL, 0);
    check(bt_state() == BT_STATE_IDLE, "  queue drained -> IDLE");
}

int main(void)
{
    unsigned i;

    printf("bt service\n");

    check(bt_state() == BT_STATE_OFF, "starts OFF");

    bt_set_enabled(true);
    check(bt_state() == BT_STATE_RESET, "enable -> RESET");

    rec_reset();
    bt_service_event(BT_EV_READY, NULL, 0);
    check(bt_state() == BT_STATE_CONFIG, "READY -> CONFIG");
    check(rec_n == 1u && rec_is(0, "AT+GMR?\r\n"), "READY sends AT+GMR? first");
    {
        unsigned before = rec_n;
        bt_service_event(BT_EV_NONE, NULL, 0);
        bt_service_event(BT_EV_BINARY_FRAME, NULL, 0);
        check(rec_n == before, "NONE / binary frame do not advance");
    }
    drive_config(NULL);
    check(strcmp(bt_version(), "YBT100_FW_V01_02_012") == 0, "VERSION stored");
    {
        unsigned before = rec_n;
        bt_service_event(BT_EV_NONE, NULL, 0);
        bt_service_event(BT_EV_BT_SCAN_STOP, NULL, 0);
        check(rec_n == before, "IDLE ignores stray events");
    }

    /* With a name set, AT+WRITE_NAME is inserted after the mode. */
    rec_reset();
    bt_set_name("RA89R");
    bt_service_event(BT_EV_READY, NULL, 0);
    check(rec_n == 1u && rec_is(0, "AT+GMR?\r\n"), "name: AT+GMR? first");
    drive_config("name");

    rec_reset();
    bt_set_enabled(false);
    check(bt_state() == BT_STATE_OFF, "disable -> OFF");
    check(rec_last_is("AT+BT_DISCN\r\n"), "disable sends AT+BT_DISCN");

    /* A silent module must not spin forever: the reset attempt is bounded. */
    bt_set_enabled(true);
    for (i = 0; i < 1000u; i++)
        bt_service_tick();
    check(bt_state() == BT_STATE_OFF, "silent module times out to OFF");

    printf("\n%d failed\n", fails);
    return fails ? 1 : 0;
}
