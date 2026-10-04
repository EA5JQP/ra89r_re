/* Bluetooth service -- see app/bt.h and docs/ra89r_bluetooth_design.md.
 *
 * The command sequence is the stock's (`FUN_08007FD0` on `+IM_READY`); the
 * advance rules are the stock parser's (`+OK` advances only when one command
 * remains or the command is one of MICGAIN/SPKGAIN/BT_SCANATCN=ON/BT_CONN_LAST;
 * `+ERROR` only for the mode commands).  A per-command timeout keeps a silent
 * module from stalling the queue forever.
 */
#include "app/bt.h"

#include <string.h>

#define BT_QUEUE_MAX    8u
#define BT_RESET_TICKS  200u    /* 2 s at the 10 ms tick */
#define BT_RESET_RETRIES 5u     /* re-pulse PD0 and listen again, like the stock */
#define BT_CMD_TICKS    500u    /* 5 s per command before forcing progress */
#define BT_TEXT_MAX     32u

static bt_state_t s_state = BT_STATE_OFF;
static bool       s_enabled;
static uint8_t    s_mode;
static char       s_name[16];
static bool       s_name_set;

static char       s_version[BT_TEXT_MAX];
static char       s_local[BT_TEXT_MAX];

static bt_cmd_t   s_queue[BT_QUEUE_MAX];
static unsigned   s_qlen;
static unsigned   s_qidx;
static unsigned   s_ticks;

static bool       s_connected;
static unsigned   s_retries;    /* PD0 re-pulses while waiting for +IM_READY */

/* Devices the module reported as `+IM_EARDEV` since the scan started.  The
 * payload is `<address>,<name>,<rssi>` (e.g. `7BA245EBBE2C,Ear (stick),-68`):
 * the address is what `AT+EAR_CONN` takes, the name is what we show. */
#define BT_FOUND_MAX 8u
#define BT_FOUND_LEN 40u
#define BT_NAME_LEN  24u
static char       s_found[BT_FOUND_MAX][BT_FOUND_LEN];       /* the whole payload */
static char       s_found_name[BT_FOUND_MAX][BT_NAME_LEN];   /* the name field */
static unsigned   s_found_n;

/* Copy the second comma-separated field (the device name) out of an
 * `+IM_EARDEV` payload. */
static void eardev_name(const char *payload, unsigned len, char *out, unsigned cap)
{
    unsigned i, start = 0u, field = 0u;

    out[0] = '\0';
    if (payload == 0)
        return;
    for (i = 0; i <= len; i++) {
        if (i == len || payload[i] == ',') {
            if (field == 1u) {
                unsigned n = i - start;
                if (n >= cap)
                    n = cap - 1u;
                memcpy(out, payload + start, n);
                out[n] = '\0';
                return;
            }
            field++;
            start = i + 1u;
        }
    }
}

static void text_copy(char *dst, unsigned cap, const char *src, unsigned len)
{
    if (len >= cap)
        len = cap - 1u;
    if (len != 0u && src != 0)
        memcpy(dst, src, len);
    dst[len] = '\0';
}

static void send_one(bt_cmd_t cmd)
{
    if (cmd == BT_CMD_WRITE_NAME)
        bluetooth_send_param(cmd, s_name);
    else
        bluetooth_send_cmd(cmd);
}

static void queue_clear(void)
{
    s_qlen = 0;
    s_qidx = 0;
}

static void queue_push(bt_cmd_t cmd)
{
    if (s_qlen < BT_QUEUE_MAX)
        s_queue[s_qlen++] = cmd;
}

static void queue_send(void)
{
    s_ticks = 0;
    if (s_qidx < s_qlen)
        send_one(s_queue[s_qidx]);
    else
        s_state = BT_STATE_IDLE;
}

static void queue_advance(void)
{
    if (s_qidx < s_qlen)
        s_qidx++;
    queue_send();
}

static bool ok_advances(bt_cmd_t cmd)
{
    return cmd == BT_CMD_MICGAIN || cmd == BT_CMD_SPKGAIN
        || cmd == BT_CMD_BT_SCANATCN_ON || cmd == BT_CMD_BT_CONN_LAST;
}

static bool error_advances(bt_cmd_t cmd)
{
    return cmd == BT_CMD_BT_EMITTER || cmd == BT_CMD_BT_RECEIVER
        || cmd == BT_CMD_BLE_MASTER_ON || cmd == BT_CMD_BLE_SLAVE_ON;
}

/* The stock's `FUN_08007FD0`, run on `+IM_READY`. */
static void build_config_sequence(void)
{
    queue_clear();
    queue_push(BT_CMD_GMR);
    queue_push(s_mode == 0u ? BT_CMD_BT_EMITTER : BT_CMD_BT_RECEIVER);
    if (s_name_set)
        queue_push(BT_CMD_WRITE_NAME);
    queue_push(BT_CMD_BLE_LOCAL_Q);
    queue_push(BT_CMD_BT_SCANATCN_ON);
    queue_push(BT_CMD_CONN_STATE_Q);
    queue_push(BT_CMD_BT_CONN_LAST);
    s_state = BT_STATE_CONFIG;
    queue_send();
}

void bt_service_event(bt_event_t ev, const char *payload, unsigned len)
{
    switch (ev) {
    case BT_EV_READY:
        /* Only start the sequence for an enabled service that is actually
         * waiting for the module.  The module's boot banner can deliver a
         * late +IM_READY after a disable or a reset timeout, which must not
         * restart it. */
        if (s_enabled && s_state == BT_STATE_RESET)
            build_config_sequence();
        break;

    case BT_EV_OK:
        if (s_state == BT_STATE_CONFIG && s_qidx < s_qlen &&
            ((s_qidx + 1u == s_qlen) || ok_advances(s_queue[s_qidx])))
            queue_advance();
        break;

    case BT_EV_ERROR:
        if (s_state == BT_STATE_CONFIG && s_qidx < s_qlen &&
            error_advances(s_queue[s_qidx]))
            queue_advance();
        break;

    case BT_EV_VERSION:
        text_copy(s_version, sizeof s_version, payload, len);
        if (s_state == BT_STATE_CONFIG)
            queue_advance();
        break;

    case BT_EV_BT_LOCAL:
    case BT_EV_BLE_LOCAL:
        text_copy(s_local, sizeof s_local, payload, len);
        if (s_state == BT_STATE_CONFIG)
            queue_advance();
        break;

    case BT_EV_BT_EMITTER:
    case BT_EV_BT_RECEIVER:
    case BT_EV_NAME_EQUALLY:
    case BT_EV_CONN_STATE:
        /* Terminal responses to a queued command.  +IM_BT_SCAN_STOP is NOT
         * here: it is unsolicited (the boot banner carries it) and the stock
         * does not advance on it. */
        if (s_state == BT_STATE_CONFIG)
            queue_advance();
        break;

    case BT_EV_EARDEV: {                /* +IM_EARDEV: a device was found */
        unsigned n = len;

        if (s_found_n < BT_FOUND_MAX) {
            if (n > BT_FOUND_LEN - 1u)
                n = BT_FOUND_LEN - 1u;
            if (n != 0u && payload != 0)
                memcpy(s_found[s_found_n], payload, n);
            s_found[s_found_n][n] = '\0';
            eardev_name(s_found[s_found_n], n, s_found_name[s_found_n],
                        BT_NAME_LEN);
            s_found_n++;
        }
        break;
    }

    case BT_EV_BT_EAR_CONN:             /* earpiece connected */
    case BT_EV_SCO_CONN:                /* audio link up */
    case BT_EV_CALL_CONNECTED:
        s_connected = true;
        if (s_state == BT_STATE_SCAN || s_state == BT_STATE_CONNECT)
            s_state = BT_STATE_CONNECTED;
        break;

    case BT_EV_BT_DISCONNECT:
    case BT_EV_SCO_DISCONNECT:
    case BT_EV_CALL_DISCONNECTED:
        s_connected = false;
        if (s_state == BT_STATE_CONNECTED)
            s_state = BT_STATE_IDLE;
        break;

    case BT_EV_BT_SCAN_STOP:            /* scan finished */
        if (s_state == BT_STATE_SCAN)
            s_state = BT_STATE_IDLE;
        break;

    default:
        break;
    }
}

void bt_service_tick(void)
{
    if (s_state == BT_STATE_RESET) {
        if (++s_ticks >= BT_RESET_TICKS) {
            s_ticks = 0;
            if (s_retries < BT_RESET_RETRIES) {
                /* The stock re-pulses PD0 while the module has not reported
                 * ready (`FUN_080066CC`): a missed boot banner is retried. */
                s_retries++;
#ifndef BLUETOOTH_HOST_TEST
                bluetooth_power(true);
#endif
            } else {
#ifndef BLUETOOTH_HOST_TEST
                bluetooth_power(false); /* give up: hold the module in reset */
#endif
                s_state = BT_STATE_OFF;
                s_enabled = false;
            }
        }
    } else if (s_state == BT_STATE_CONFIG) {
        if (++s_ticks >= BT_CMD_TICKS)
            queue_advance();
    }
}

void bt_set_enabled(bool on)
{
    if (on == s_enabled && s_state != BT_STATE_OFF)
        return;

    s_enabled = on;
    s_ticks = 0;

    if (on) {
        s_retries = 0;
#ifndef BLUETOOTH_HOST_TEST
        bluetooth_power(true);
#endif
        s_state = BT_STATE_RESET;
    } else {
        bluetooth_send_cmd(BT_CMD_BT_DISCN);
        queue_clear();
#ifndef BLUETOOTH_HOST_TEST
        bluetooth_power(false);        /* hold the module in reset */
#endif
        s_state = BT_STATE_OFF;
    }
}

bool bt_enabled(void)
{
    return s_enabled;
}

void bt_set_mode(uint8_t mode)
{
    s_mode = (uint8_t)(mode & 1u);
}

void bt_set_name(const char *name)
{
    if (name != 0 && name[0] != '\0') {
        text_copy(s_name, sizeof s_name, name, (unsigned)strlen(name));
        s_name_set = true;
    } else {
        s_name[0] = '\0';
        s_name_set = false;
    }
}

/* The stock's own gain value strings (`FUN_0802286c` mic, `FUN_080226d8`
 * speaker); the level is an index into these. */
static const char *const bt_mic_gain_str[] = { "0", "5", "6", "7", "8" };
static const char *const bt_spk_gain_str[] = { "0", "4", "8", "16", "23", "31" };

unsigned bt_spk_gain_levels(void)
{
    return (unsigned)(sizeof bt_spk_gain_str / sizeof bt_spk_gain_str[0]);
}

unsigned bt_mic_gain_levels(void)
{
    return (unsigned)(sizeof bt_mic_gain_str / sizeof bt_mic_gain_str[0]);
}

void bt_set_scan(bool on)
{
    bluetooth_send_cmd(on ? BT_CMD_BT_SCAN_ON : BT_CMD_BT_SCAN_OFF);
}

void bt_set_spk_gain(uint8_t level)
{
    if ((unsigned)level < bt_spk_gain_levels())
        bluetooth_send_param(BT_CMD_SPKGAIN, bt_spk_gain_str[level]);
}

void bt_set_mic_gain(uint8_t level)
{
    if ((unsigned)level < bt_mic_gain_levels())
        bluetooth_send_param(BT_CMD_MICGAIN, bt_mic_gain_str[level]);
}

/* Pairing: the stock's Pairing item queues `AT+BT_SCAN=ON`; the module then
 * reports found earpieces as `+IM_EARDEV:<text>`. */
void bt_start_connect(void)
{
    s_found_n = 0;
    s_state = BT_STATE_SCAN;
    bluetooth_send_cmd(BT_CMD_BT_SCAN_ON);
}

void bt_stop_connect(void)
{
    bluetooth_send_cmd(BT_CMD_BT_SCAN_OFF);
    if (s_state == BT_STATE_SCAN)
        s_state = BT_STATE_IDLE;
}

bool bt_connected(void)
{
    return s_connected;
}

unsigned bt_found_count(void)
{
    return s_found_n;
}

const char *bt_found_dev(unsigned i)
{
    if (i >= s_found_n)
        return "";
    return (s_found_name[i][0] != '\0') ? s_found_name[i] : s_found[i];
}

void bt_connect_dev(unsigned i)
{
    if (i < s_found_n)
        bluetooth_send_param(BT_CMD_EAR_CONN, s_found[i]);
}

bt_state_t bt_state(void)
{
    return s_state;
}

const char *bt_version(void)
{
    return s_version;
}

const char *bt_local_addr(void)
{
    return s_local;
}

#ifndef BLUETOOTH_HOST_TEST
static void bt_on_event(bt_event_t ev, const char *payload, unsigned len)
{
    bt_service_event(ev, payload, len);
}

void bt_init(void)
{
    bluetooth_init();
    bluetooth_set_event_cb(bt_on_event);
}

/* Drain USART3 only.  Call this on every pass of the main loop: the module's
 * boot banner is ~100 bytes sent in ~9 ms, so draining once per 10 ms slice
 * lets the 1-byte USART buffer overrun and drops `+IM_READY`.  The state
 * machine's clock is `bt_service_tick()`, which stays on the 10 ms slice. */
void bt_poll(void)
{
    bluetooth_poll();
}
#endif /* !BLUETOOTH_HOST_TEST */
