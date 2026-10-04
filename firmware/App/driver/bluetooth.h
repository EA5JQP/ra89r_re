/* Jieli Bluetooth audio module on USART3 (PB10 TX / PB11 RX, AF2, 115200 8N1).
 *
 * The stock firmware talks to the module with a line-based AT command set and
 * parses +OK / +ERROR / +IM_* event lines -- see docs/ra89r_bluetooth.md for
 * the function addresses every claim here comes from.  This header is
 * deliberately free of any MCU include: the command table and the response
 * parser are pure logic and are host-tested in `tools/test_bluetooth.c`.  The
 * USART3 hardware half is `driver/bluetooth.c`, compiled out when
 * BLUETOOTH_HOST_TEST is defined (the same split as `beeper.h`/`beeper.c`).
 *
 * The command strings below are the stock's own constants, byte for byte,
 * including the trailing CRLF; the numeric enum values are the stock's own
 * command-list indices (`FUN_0802213c`'s switch), kept so a table dump can be
 * compared against the image.
 */
#ifndef DRIVER_BLUETOOTH_H
#define DRIVER_BLUETOOTH_H

#include <stdbool.h>
#include <stdint.h>

/* The fixed command set.  `BT_CMD_COUNT` is the size of the stock's switch. */
typedef enum {
    BT_CMD_GMR             = 0x00,  /* AT+GMR?           -- query firmware version */
    BT_CMD_BAUD_1          = 0x01,  /* AT+BAUD=1 */
    BT_CMD_BAUD_2          = 0x02,  /* AT+BAUD=2 */
    BT_CMD_SLEEP_ON        = 0x03,  /* AT+SLEEP=ON */
    BT_CMD_SLEEP_OFF       = 0x04,  /* AT+SLEEP=OFF */
    BT_CMD_POWEROFF        = 0x05,  /* AT+POWEROFF */
    BT_CMD_RST             = 0x06,  /* AT+RST */
    BT_CMD_CONN_STATE_Q    = 0x07,  /* AT+CONN_STATE?    -- query connection */
    BT_CMD_SPKGAIN_Q       = 0x08,  /* AT+SPKGAIN? */
    BT_CMD_MICGAIN_Q       = 0x09,  /* AT+MICGAIN? */
    BT_CMD_MICGAIN         = 0x0a,  /* AT+MICGAIN=<v>    -- parameterised */
    BT_CMD_SPKGAIN         = 0x0b,  /* AT+SPKGAIN=<v>    -- parameterised */
    BT_CMD_WRITE_NAME      = 0x0c,  /* AT+WRITE_NAME=<v> -- parameterised */
    BT_CMD_BLE_LOCAL_Q     = 0x0d,  /* AT+BLE_LOCAL?     -- query BLE address/name */
    BT_CMD_LAST_RING_Q     = 0x0e,  /* AT+LAST_RING? */
    BT_CMD_BLE_SLAVE_ON    = 0x0f,  /* AT+BLE_SLAVE=ON */
    BT_CMD_BLE_SLAVE_OFF   = 0x10,  /* AT+BLE_SLAVE=OFF */
    BT_CMD_BLE_MASTER_ON   = 0x11,  /* AT+BLE_MASTER=ON */
    BT_CMD_BLE_MASTER_OFF  = 0x12,  /* AT+BLE_MASTER=OFF */
    BT_CMD_BLE_SCANATCN_ON = 0x13,  /* AT+BLE_SCANATCN=ON  -- scan and auto-connect */
    BT_CMD_BLE_SCANATCN_OFF= 0x14,  /* AT+BLE_SCANATCN=OFF */
    BT_CMD_BLE_SCAN_ON     = 0x15,  /* AT+BLE_SCAN=ON */
    BT_CMD_BLE_SCAN_OFF    = 0x16,  /* AT+BLE_SCAN=OFF */
    BT_CMD_BLE_DISCN       = 0x17,  /* AT+BLE_DISCN      -- disconnect */
    BT_CMD_BLE_CONN_LAST   = 0x18,  /* AT+BLE_CONN_LAST  -- reconnect last BLE peer */
    BT_CMD_RING_CONN       = 0x19,  /* AT+RING_CONN=<v>  -- parameterised */
    BT_CMD_BT_LOCAL_Q      = 0x1a,  /* AT+BT_LOCAL?      -- query BT address/name */
    BT_CMD_LAST_EAR_Q      = 0x1b,  /* AT+LAST_EAR?      -- query last earpiece */
    BT_CMD_BT_EMITTER      = 0x1c,  /* AT+BT=EMITTER */
    BT_CMD_BT_RECEIVER     = 0x1d,  /* AT+BT=RECEIVER */
    BT_CMD_BT_SCAN_ON      = 0x1e,  /* AT+BT_SCAN=ON */
    BT_CMD_BT_SCAN_OFF     = 0x1f,  /* AT+BT_SCAN=OFF */
    BT_CMD_BT_SCANATCN_ON  = 0x20,  /* AT+BT_SCANATCN=ON -- scan and auto-connect */
    BT_CMD_BT_SCANATCN_OFF = 0x21,  /* AT+BT_SCANATCN=OFF */
    BT_CMD_BT_DISCN        = 0x22,  /* AT+BT_DISCN       -- disconnect */
    BT_CMD_BT_CONN_LAST    = 0x23,  /* AT+BT_CONN_LAST   -- reconnect last earpiece */
    BT_CMD_EAR_CONN        = 0x24,  /* AT+EAR_CONN=<v>   -- parameterised */
    BT_CMD_BT_PAIRCLR      = 0x25,  /* AT+BT_PAIRCLR     -- clear the pairing list */
    BT_CMD_BT_CALL_ON      = 0x26,  /* AT+BT_CALL=ON     -- route call audio */
    BT_CMD_BT_CALL_OFF     = 0x27,  /* AT+BT_CALL=OFF */
    BT_CMD_COUNT           = 0x28,
} bt_cmd_t;

/* Events the module reports, in the order the stock's parser tests them
 * (`FUN_08022974`).  `BT_EV_BINARY_FRAME` is the framed "RDTP" control path,
 * which this driver recognises but does not decode. */
typedef enum {
    BT_EV_NONE = 0,
    BT_EV_BINARY_FRAME,     /* "RDTP..." -- binary control frame, not decoded */
    BT_EV_READY,            /* "+IM_READY"          -- module booted, start pairing */
    BT_EV_OK,               /* "+OK"                -- command accepted */
    BT_EV_ERROR,            /* "+ERROR"             -- command rejected */
    BT_EV_VERSION,          /* "+IM_VERSION:<v>"    -- firmware version */
    BT_EV_BT_EMITTER,       /* "+IM_BT_EMITTER" */
    BT_EV_NAME_EQUALLY,     /* "+IM_NAME_EQUALLY" */
    BT_EV_BT_RECEIVER,      /* "+IM_BT_RECEIVER" */
    BT_EV_BLE_MASTER,       /* "+IM_BLE_MASTER" */
    BT_EV_BLE_SLAVE,        /* "+IM_BLE_SLAVE" */
    BT_EV_CONN_STATE,       /* "+IM_CONN_STATE:<v>" */
    BT_EV_SCO_CONN,         /* "+IM_SCO_CONN"       -- audio link up */
    BT_EV_CALL_CONNECTED,   /* "+IM_CALL_CONED" */
    BT_EV_CALL_DISCONNECTED,/* "+IM_CALL_DISCONED" */
    BT_EV_SCO_DISCONNECT,   /* "+IM_SCO_DISCN"      -- audio link down */
    BT_EV_EARDEV,           /* "+IM_EARDEV:<v>"     -- earpiece found */
    BT_EV_BT_SCAN_STOP,     /* "+IM_BT_SCAN_STOP" */
    BT_EV_BT_EAR_CONN,      /* "+IM_BT_EAR_CONN"    -- earpiece connected */
    BT_EV_BT_DISCONNECT,    /* "+IM_BT_DISCN" */
    BT_EV_EAR_PTT_DOWN,     /* "+IM_EAR_PTT_KEYDOWN" */
    BT_EV_EAR_PTT_UP,       /* "+IM_EAR_PTT_KEYUP" */
    BT_EV_BLE_LOCAL,        /* "+IM_BLE_LOCAL<v>"   -- BLE address/name */
    BT_EV_BT_LOCAL,         /* "+IM_BT_LOCAL<v>"    -- BT address/name */
} bt_event_t;

/* The exact string the stock sends for a fixed command, or NULL for the five
 * parameterised ones (build those with bt_build_param).  The string includes
 * the stock's own trailing "\r\n". */
const char *bt_cmd_string(bt_cmd_t cmd);

/* Build "AT+<NAME>=<value>\r\n" for one of BT_CMD_MICGAIN, BT_CMD_SPKGAIN,
 * BT_CMD_WRITE_NAME, BT_CMD_RING_CONN, BT_CMD_EAR_CONN.  Returns the length
 * written (excluding the NUL), or 0 if the command is not parameterised or the
 * buffer is too small.  Always NUL-terminates on success. */
unsigned bt_build_param(bt_cmd_t cmd, const char *value, char *buf, unsigned cap);

/* Classify one received line (without its CRLF; a leading "++" is stripped as
 * the stock does).  `*payload`, when the event carries one, points just past
 * the matched prefix and `*payload_len` is the remaining length.  Pure. */
bt_event_t bt_parse_line(const char *line, unsigned len,
                         const char **payload, unsigned *payload_len);

/* ----------------------------------------------------------------- transport
 *
 * The low-level transmit hook.  On the target this is defined in
 * `bluetooth.c`; a host test compiles that file with -DBLUETOOTH_HOST_TEST and
 * provides its own recorder. */
void bluetooth_hw_write(const uint8_t *data, unsigned len);

/* Drive the module's reset/enable line, PD0 (GPIOD mask 1).  The stock holds it
 * LOW (module in reset) until Bluetooth is enabled, then pulses it low -> high;
 * so `on` releases the module (and lets it boot and emit `+IM_READY`), `off`
 * holds it in reset.  Target only.  See docs/ra89r_bluetooth.md. */
void bluetooth_power(bool on);

/* Bring up USART3 (PB10/PB11, AF2, 115200 8N1) and release the module from
 * reset on PD0.  Target only. */
void bluetooth_init(void);

/* Send raw bytes / a fixed command / a parameterised command. */
void bluetooth_send(const char *text);
void bluetooth_send_cmd(bt_cmd_t cmd);
void bluetooth_send_param(bt_cmd_t cmd, const char *value);

/* Feed received bytes: split on CRLF, classify each line, invoke the callback.
 * Host-testable (no hardware). */
void bluetooth_set_event_cb(void (*cb)(bt_event_t ev, const char *payload,
                                       unsigned len));
void bluetooth_feed(const uint8_t *data, unsigned len);

/* Target only: drain USART3's receive register into bluetooth_feed(). */
void bluetooth_poll(void);

/* Target only: total bytes drained since boot (a diagnostic: proves the
 * module is transmitting). */
unsigned bluetooth_rx_bytes(void);

#endif /* DRIVER_BLUETOOTH_H */
