/* Host test for the Jieli Bluetooth AT driver, without a radio.
 *
 * The USART3 layer can only be proven on the radio (does the module answer, and
 * on which pins).  What *can* be pinned here is the protocol: that the exact
 * bytes the stock's own constants put on the wire are the bytes this driver
 * puts on the wire, and that every response the stock's parser recognises is
 * classified the same way.  `bluetooth_hw_write()` is replaced with a recorder,
 * and `bluetooth.c` is built with -DBLUETOOTH_HOST_TEST so its USART3 half is
 * not compiled in.
 *
 *   gcc -std=c11 -I App -I App/driver -DBLUETOOTH_HOST_TEST \
 *       tools/test_bluetooth.c App/driver/bluetooth.c -o /tmp/test_bluetooth
 *   /tmp/test_bluetooth
 *
 * It is not a substitute for the radio: it cannot tell whether the module
 * answers at all, only that we ask correctly and parse correctly.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/bluetooth.h"

/* ---------------------------------------------------------- transport stub --- */

static uint8_t  wire[256];
static unsigned wire_len;

void bluetooth_hw_write(const uint8_t *data, unsigned len)
{
    unsigned i;

    for (i = 0; i < len && wire_len < sizeof wire; i++)
        wire[wire_len++] = data[i];
}

static void wire_reset(void) { wire_len = 0; }

/* ------------------------------------------------------------- checks ------ */

static unsigned checks, failed;

static void check(int ok, const char *what)
{
    checks++;
    if (ok)
        printf("ok    %s\n", what);
    else {
        failed++;
        printf("FAIL  %s\n", what);
    }
}

/* The stock's command strings, from the image (0x08022344ff). */
static const char *const expected[BT_CMD_COUNT] = {
    [BT_CMD_GMR]              = "AT+GMR?\r\n",
    [BT_CMD_BAUD_1]           = "AT+BAUD=1\r\n",
    [BT_CMD_BAUD_2]           = "AT+BAUD=2\r\n",
    [BT_CMD_SLEEP_ON]         = "AT+SLEEP=ON\r\n",
    [BT_CMD_SLEEP_OFF]        = "AT+SLEEP=OFF\r\n",
    [BT_CMD_POWEROFF]         = "AT+POWEROFF\r\n",
    [BT_CMD_RST]              = "AT+RST\r\n",
    [BT_CMD_CONN_STATE_Q]     = "AT+CONN_STATE?\r\n",
    [BT_CMD_SPKGAIN_Q]        = "AT+SPKGAIN?\r\n",
    [BT_CMD_MICGAIN_Q]        = "AT+MICGAIN?\r\n",
    [BT_CMD_BLE_LOCAL_Q]      = "AT+BLE_LOCAL?\r\n",
    [BT_CMD_LAST_RING_Q]      = "AT+LAST_RING?\r\n",
    [BT_CMD_BLE_SLAVE_ON]     = "AT+BLE_SLAVE=ON\r\n",
    [BT_CMD_BLE_SLAVE_OFF]    = "AT+BLE_SLAVE=OFF\r\n",
    [BT_CMD_BLE_MASTER_ON]    = "AT+BLE_MASTER=ON\r\n",
    [BT_CMD_BLE_MASTER_OFF]   = "AT+BLE_MASTER=OFF\r\n",
    [BT_CMD_BLE_SCANATCN_ON]  = "AT+BLE_SCANATCN=ON\r\n",
    [BT_CMD_BLE_SCANATCN_OFF] = "AT+BLE_SCANATCN=OFF\r\n",
    [BT_CMD_BLE_SCAN_ON]      = "AT+BLE_SCAN=ON\r\n",
    [BT_CMD_BLE_SCAN_OFF]     = "AT+BLE_SCAN=OFF\r\n",
    [BT_CMD_BLE_DISCN]        = "AT+BLE_DISCN\r\n",
    [BT_CMD_BLE_CONN_LAST]    = "AT+BLE_CONN_LAST\r\n",
    [BT_CMD_BT_LOCAL_Q]       = "AT+BT_LOCAL?\r\n",
    [BT_CMD_LAST_EAR_Q]       = "AT+LAST_EAR?\r\n",
    [BT_CMD_BT_EMITTER]       = "AT+BT=EMITTER\r\n",
    [BT_CMD_BT_RECEIVER]      = "AT+BT=RECEIVER\r\n",
    [BT_CMD_BT_SCAN_ON]       = "AT+BT_SCAN=ON\r\n",
    [BT_CMD_BT_SCAN_OFF]      = "AT+BT_SCAN=OFF\r\n",
    [BT_CMD_BT_SCANATCN_ON]   = "AT+BT_SCANATCN=ON\r\n",
    [BT_CMD_BT_SCANATCN_OFF]  = "AT+BT_SCANATCN=OFF\r\n",
    [BT_CMD_BT_DISCN]         = "AT+BT_DISCN\r\n",
    [BT_CMD_BT_CONN_LAST]     = "AT+BT_CONN_LAST\r\n",
    [BT_CMD_BT_PAIRCLR]       = "AT+BT_PAIRCLR\r\n",
    [BT_CMD_BT_CALL_ON]       = "AT+BT_CALL=ON\r\n",
    [BT_CMD_BT_CALL_OFF]      = "AT+BT_CALL=OFF\r\n",
};

/* The parameterised commands. */
struct param_case {
    bt_cmd_t    cmd;
    const char *value;
    const char *expected;
};

static const struct param_case params[] = {
    { BT_CMD_MICGAIN,    "5",    "AT+MICGAIN=5\r\n" },
    { BT_CMD_SPKGAIN,    "16",   "AT+SPKGAIN=16\r\n" },
    { BT_CMD_WRITE_NAME, "RA89R","AT+WRITE_NAME=RA89R\r\n" },
    { BT_CMD_RING_CONN,  "1",    "AT+RING_CONN=1\r\n" },
    { BT_CMD_EAR_CONN,   "AABBCCDDEEFF", "AT+EAR_CONN=AABBCCDDEEFF\r\n" },
};

/* --------------------------------------------------------------- callback --- */

static bt_event_t  cb_ev[32];
static char        cb_payload[32][64];
static unsigned    cb_n;

static void on_event(bt_event_t ev, const char *payload, unsigned len)
{
    if (cb_n >= 32)
        return;
    cb_ev[cb_n] = ev;
    if (payload && len < 64u) {
        memcpy(cb_payload[cb_n], payload, len);
        cb_payload[cb_n][len] = '\0';
    } else {
        cb_payload[cb_n][0] = '\0';
    }
    cb_n++;
}

int main(void)
{
    unsigned i;
    char buf[80];

    printf("Jieli Bluetooth AT driver host test\n");

    /* 1. The fixed command table, byte for byte. */
    for (i = 0; i < BT_CMD_COUNT; i++) {
        char what[64];
        const char *got = bt_cmd_string((bt_cmd_t)i);

        if (expected[i]) {
            snprintf(what, sizeof what, "cmd 0x%02x string", i);
            check(got && strcmp(got, expected[i]) == 0, what);
        } else {
            snprintf(what, sizeof what, "cmd 0x%02x is parameterised", i);
            check(got == 0, what);
        }
    }

    /* 2. The parameterised builders. */
    for (i = 0; i < sizeof params / sizeof params[0]; i++) {
        char what[64];
        unsigned n = bt_build_param(params[i].cmd, params[i].value, buf, sizeof buf);

        snprintf(what, sizeof what, "build %s", params[i].expected);
        check(n == strlen(params[i].expected) &&
              strcmp(buf, params[i].expected) == 0, what);
    }
    check(bt_build_param(BT_CMD_GMR, "x", buf, sizeof buf) == 0,
          "non-parameterised build returns 0");
    check(bt_build_param(BT_CMD_WRITE_NAME, "0123456789012345678901234567890123456789"
                         "0123456789012345678901234567890123456789",
                         buf, sizeof buf) == 0,
          "oversized build returns 0");

    /* 3. The response parser, every event the stock recognises. */
    {
        static const struct { const char *line; bt_event_t ev; } cases[] = {
            { "RDTP\xfe\xfe\xee\xef\xe0", BT_EV_BINARY_FRAME },
            { "+IM_READY",                BT_EV_READY },
            { "+OK",                      BT_EV_OK },
            { "+ERROR",                   BT_EV_ERROR },
            { "+IM_VERSION:V1.2.3",       BT_EV_VERSION },
            { "+IM_BT_EMITTER",           BT_EV_BT_EMITTER },
            { "+IM_NAME_EQUALLY",         BT_EV_NAME_EQUALLY },
            { "+IM_BT_RECEIVER",          BT_EV_BT_RECEIVER },
            { "+IM_BLE_MASTER",           BT_EV_BLE_MASTER },
            { "+IM_BLE_SLAVE",            BT_EV_BLE_SLAVE },
            { "+IM_CONN_STATE:1",         BT_EV_CONN_STATE },
            { "+IM_SCO_CONN",             BT_EV_SCO_CONN },
            { "+IM_CALL_CONED",           BT_EV_CALL_CONNECTED },
            { "+IM_CALL_DISCONED",        BT_EV_CALL_DISCONNECTED },
            { "+IM_SCO_DISCN",            BT_EV_SCO_DISCONNECT },
            { "+IM_EARDEV:AA:BB",         BT_EV_EARDEV },
            { "+IM_BT_SCAN_STOP",         BT_EV_BT_SCAN_STOP },
            { "+IM_BT_EAR_CONN",          BT_EV_BT_EAR_CONN },
            { "+IM_BT_DISCN",             BT_EV_BT_DISCONNECT },
            { "+IM_EAR_PTT_KEYDOWN",      BT_EV_EAR_PTT_DOWN },
            { "+IM_EAR_PTT_KEYUP",        BT_EV_EAR_PTT_UP },
            { "+IM_BLE_LOCAL112233",      BT_EV_BLE_LOCAL },
            { "+IM_BT_LOCAL445566",       BT_EV_BT_LOCAL },
            { "garbage",                  BT_EV_NONE },
        };

        for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            const char *pl = 0;
            unsigned pll = 0;
            bt_event_t ev = bt_parse_line(cases[i].line,
                                          (unsigned)strlen(cases[i].line), &pl, &pll);
            char what[80];

            snprintf(what, sizeof what, "parse '%s'", cases[i].line);
            check(ev == cases[i].ev, what);
            (void)pl;
            (void)pll;
        }
    }

    /* Payload offsets for the four events that carry one. */
    {
        const char *pl = 0;
        unsigned pll = 0;

        bt_parse_line("+IM_VERSION:V1.2.3", (unsigned)strlen("+IM_VERSION:V1.2.3"),
                      &pl, &pll);
        check(pl && pll == 6 && memcmp(pl, "V1.2.3", 6) == 0,
              "VERSION payload starts after the ':'");
        bt_parse_line("+IM_CONN_STATE:1", (unsigned)strlen("+IM_CONN_STATE:1"),
                      &pl, &pll);
        check(pl && pll == 1 && pl[0] == '1', "CONN_STATE payload");
        bt_parse_line("+IM_EARDEV:AA:BB", (unsigned)strlen("+IM_EARDEV:AA:BB"),
                      &pl, &pll);
        check(pl && pll == 5 && memcmp(pl, "AA:BB", 5) == 0, "EARDEV payload");
    }

    /* The "++" escape: one '+' is stripped before matching. */
    {
        const char *pl = 0;
        unsigned pll = 0;
        check(bt_parse_line("++IM_READY", 10, &pl, &pll) == BT_EV_READY,
              "'++' line strips one '+'");
    }

    /* 4. The wire path: send_cmd / send_param emit exactly the stock bytes. */
    wire_reset();
    bluetooth_send_cmd(BT_CMD_BT_SCAN_ON);
    check(wire_len == strlen("AT+BT_SCAN=ON\r\n") &&
          memcmp(wire, "AT+BT_SCAN=ON\r\n", wire_len) == 0,
          "send_cmd puts the exact bytes on the wire");

    wire_reset();
    bluetooth_send_param(BT_CMD_WRITE_NAME, "RA89R");
    check(wire_len == strlen("AT+WRITE_NAME=RA89R\r\n") &&
          memcmp(wire, "AT+WRITE_NAME=RA89R\r\n", wire_len) == 0,
          "send_param puts the exact bytes on the wire");

    /* 5. The receive path: CRLF splitting across feed() calls. */
    bluetooth_set_event_cb(on_event);
    cb_n = 0;
    bluetooth_feed((const uint8_t *)"+OK\r", 4);           /* split mid-line */
    bluetooth_feed((const uint8_t *)"\n+IM_READY\r\n", 12);
    bluetooth_feed((const uint8_t *)"+IM_VERSION:V9\r\n", 16);
    check(cb_n == 3, "three lines delivered");
    check(cb_ev[0] == BT_EV_OK && cb_ev[1] == BT_EV_READY &&
          cb_ev[2] == BT_EV_VERSION, "line events in order");
    check(strcmp(cb_payload[2], "V9") == 0, "version payload through the callback");

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
