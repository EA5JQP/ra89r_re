/* Bluetooth debug console -- see app/bt_debug.h and docs/ra89r_bt_debug.md. */
#include "app/bt_debug.h"

#include <stdio.h>
#include <string.h>

#include "app/bt.h"
#include "driver/bluetooth.h"

#define BT_DEBUG_LINE_MAX   96u
#define BT_DEBUG_TIMEOUT_MS 5000u

static bt_debug_io_t s_io;
static bool          s_active;
static char          s_line[BT_DEBUG_LINE_MAX];
static unsigned      s_len;
static uint32_t      s_started;
static bool          s_trace;

/* The last PTT/TX picture reported, so the trace prints only transitions. */
static bool     s_ptt_known;
static bool     s_last_headset, s_last_radio, s_last_tx;

static uint32_t now_ms(void)
{
    return s_io.millis ? s_io.millis() : 0u;
}

static bool radio_ptt(void) { return s_io.radio_ptt ? s_io.radio_ptt() : false; }
static bool tx_active(void) { return s_io.tx_active ? s_io.tx_active() : false; }
static uint8_t ptt_type(void) { return s_io.ptt_type ? s_io.ptt_type() : 0u; }

/* Print `BT <ms> <kind> <body>\n`. */
static void emit(char kind, const char *body)
{
    char buf[BT_DEBUG_LINE_MAX + 48u];

    if (!s_io.out)
        return;
    snprintf(buf, sizeof buf, "BT %lu %c %s\n", (unsigned long)now_ms(), kind, body);
    s_io.out(buf);
}

static const char *state_name(bt_state_t st)
{
    switch (st) {
    case BT_STATE_OFF:       return "Off";
    case BT_STATE_RESET:     return "Reset";
    case BT_STATE_CONFIG:    return "Config";
    case BT_STATE_IDLE:      return "Ready";
    case BT_STATE_SCAN:      return "Scan";
    case BT_STATE_CONNECT:   return "Connect";
    case BT_STATE_CONNECTED: return "Linked";
    }
    return "?";
}

/* The transport's trace hook: one line per module line or sent command. */
static void on_trace(char dir, const char *text, unsigned len, bt_event_t ev)
{
    char body[BT_DEBUG_LINE_MAX + 24u];
    unsigned n = len;

    if (!s_trace)
        return;
    if (n > BT_DEBUG_LINE_MAX - 1u)
        n = BT_DEBUG_LINE_MAX - 1u;
    if (dir == '<')
        snprintf(body, sizeof body, "%.*s [%s]", (int)n, text, bt_event_name(ev));
    else
        snprintf(body, sizeof body, "%.*s", (int)n, text);
    emit(dir, body);
}

static void report_ptt(void)
{
    char body[96];
    const bool headset = bt_ptt_down();
    const bool radio   = radio_ptt();
    const uint8_t type = ptt_type();

    snprintf(body, sizeof body, "headset=%u radio=%u type=%u allow=%u tx=%u",
             (unsigned)headset, (unsigned)radio, (unsigned)type,
             (unsigned)bt_ptt_source_active(radio, headset, type),
             (unsigned)tx_active());
    emit('P', body);
}

static void status(void)
{
    char body[160];

    snprintf(body, sizeof body,
             "en=%u state=%s link=%u sco=%u resume=%u ptt=%u radio_ptt=%u "
             "type=%u allow=%u tx=%u bt_tx=%u found=%u rx=%u ver=%s",
             (unsigned)bt_enabled(), state_name(bt_state()),
             (unsigned)bt_connected(), (unsigned)bt_call_active(),
             (unsigned)bt_call_resume_pending(), (unsigned)bt_ptt_down(),
             (unsigned)radio_ptt(), (unsigned)ptt_type(),
             (unsigned)bt_ptt_source_active(radio_ptt(), bt_ptt_down(), ptt_type()),
             (unsigned)tx_active(), (unsigned)bt_radio_tx_active(),
             bt_found_count(), s_io.rx_bytes ? s_io.rx_bytes() : 0u,
             bt_version()[0] ? bt_version() : "-");
    emit('S', body);
}

static void dump_rx_log(void)
{
    uint8_t  log[64];
    char     body[3u * sizeof log + 1u];
    unsigned n = s_io.rx_log ? s_io.rx_log(log, sizeof log) : 0u;
    unsigned i;

    for (i = 0; i < n; i++)
        snprintf(body + 3u * i, 4u, "%02X ", (unsigned)log[i]);
    body[n ? 3u * n - 1u : 0u] = '\0';
    emit('L', n ? body : "(empty)");
}

static void help(void)
{
    emit('H', "!s status  !t1/!t0 trace  !a<AT> send  !i<line> inject  "
              "!r restart  !l rx bytes  !? help");
}

static void run(const char *cmd, unsigned len)
{
    char buf[BT_DEBUG_LINE_MAX + 2u];

    if (len == 0u) {
        help();
        return;
    }

    switch (cmd[0]) {
    case 's': case 'S':
        status();
        break;

    case 't': case 'T':
        s_trace = (len < 2u) ? !s_trace : (cmd[1] != '0');
        s_ptt_known = false;
        emit('T', s_trace ? "trace on" : "trace off");
        if (s_trace)
            status();           /* the starting point for what follows */
        break;

    case 'a': case 'A':
        /* Raw module command.  The transport's trace hook shows it as '>'
         * while tracing; confirm it here either way. */
        snprintf(buf, sizeof buf, "%.*s\r\n", (int)(len - 1u), cmd + 1);
        bluetooth_send(buf);
        emit('A', buf[0] != '\r' ? "sent" : "sent (empty)");
        break;

    case 'i': case 'I': {
        /* A module line, through the real parser and service. */
        unsigned n = len - 1u;

        memcpy(buf, cmd + 1, n);
        buf[n++] = '\n';
        emit('I', "injected");
        bluetooth_feed((const uint8_t *)buf, n);
        break;
    }

    case 'r': case 'R':
        emit('R', "restarting module");
        if (s_io.restart)
            s_io.restart();
        break;

    case 'l': case 'L':
        dump_rx_log();
        break;

    default:
        help();
        break;
    }
}

void bt_debug_init(const bt_debug_io_t *io)
{
    if (io)
        s_io = *io;
    s_active = false;
    s_len = 0;
    s_trace = false;
    s_ptt_known = false;
    bluetooth_set_trace_cb(on_trace);
}

bool bt_debug_active(void)
{
    return s_active;
}

bool bt_debug_tracing(void)
{
    return s_trace;
}

void bt_debug_feed(char c)
{
    if (!s_active) {
        if (c == '!') {
            s_active  = true;
            s_len     = 0;
            s_started = now_ms();
        }
        return;
    }

    if (c == '\r' || c == '\n') {
        s_active = false;
        s_line[s_len] = '\0';
        run(s_line, s_len);
        return;
    }
    if (c == 0x1B) {            /* ESC: drop the line */
        s_active = false;
        return;
    }
    if ((c == '\b' || c == 0x7F) && s_len != 0u) {
        s_len--;
        return;
    }
    if (s_len < sizeof s_line - 1u && (unsigned char)c >= ' ')
        s_line[s_len++] = c;
}

void bt_debug_tick(void)
{
    if (s_active && (uint32_t)(now_ms() - s_started) > BT_DEBUG_TIMEOUT_MS)
        s_active = false;       /* an unterminated line must not eat the console */

    if (s_trace) {
        const bool headset = bt_ptt_down();
        const bool radio   = radio_ptt();
        const bool tx      = tx_active();

        if (!s_ptt_known || headset != s_last_headset || radio != s_last_radio ||
            tx != s_last_tx) {
            if (s_ptt_known)    /* the first sample is the baseline, not news */
                report_ptt();
            s_ptt_known    = true;
            s_last_headset = headset;
            s_last_radio   = radio;
            s_last_tx      = tx;
        }
    }
}
