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
#define BT_RESET_TICKS  300u    /* 3 s at the 10 ms tick */
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

    default:
        break;
    }
}

void bt_service_tick(void)
{
    if (s_state == BT_STATE_RESET) {
        if (++s_ticks >= BT_RESET_TICKS) {
#ifndef BLUETOOTH_HOST_TEST
            bluetooth_power(false);     /* give up: hold the module in reset */
#endif
            s_state = BT_STATE_OFF;
            s_enabled = false;
            s_ticks = 0;
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

void bt_poll(void)
{
    bluetooth_poll();
    bt_service_tick();
}
#endif /* !BLUETOOTH_HOST_TEST */
