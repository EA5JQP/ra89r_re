/* Host test for the Bluetooth debug console (App/app/bt_debug.c).
 *
 * Runs the real BT service (App/app/bt.c) and transport parser
 * (App/driver/bluetooth.c, -DBLUETOOTH_HOST_TEST), and checks the `!` line
 * protocol tools/ra89r_bt.py relies on: status, trace of received/sent lines
 * with their parser event, raw send, inject, and the PTT/TX transition line.
 *
 *   gcc -std=c11 -Wall -Wextra -I App -I App/driver -DBLUETOOTH_HOST_TEST \
 *       tools/test_bt_debug.c App/app/bt_debug.c App/app/bt.c \
 *       App/driver/bluetooth.c -o /tmp/test_bt_debug && /tmp/test_bt_debug
 */
#include <stdio.h>
#include <string.h>

#include "app/bt.h"
#include "app/bt_debug.h"
#include "driver/bluetooth.h"

bool gUpdateDisplay;

void audio_path_drive(int on) { (void)on; }
void pa_set_bt_audio(bool on) { (void)on; }

/* What the module was sent. */
static char     sent[512];
void bluetooth_hw_write(const uint8_t *data, unsigned len)
{
    strncat(sent, (const char *)data, len < sizeof sent - strlen(sent) - 1u
                                      ? len : sizeof sent - strlen(sent) - 1u);
}

/* What the console printed. */
static char out[4096];
static void host_out(const char *text)
{
    strncat(out, text, sizeof out - strlen(out) - 1u);
}

static uint32_t clock_ms = 1000u;
static bool     radio_key;
static bool     tx_on;
static uint8_t  type;
static unsigned restarts;

static uint32_t host_millis(void) { return clock_ms; }
static bool     host_radio_ptt(void) { return radio_key; }
static bool     host_tx_active(void) { return tx_on; }
static uint8_t  host_ptt_type(void) { return type; }
static void     host_restart(void) { restarts++; }

static int fails;

static void check(int ok, const char *what)
{
    printf("[bt_debug] %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        printf("    console was:\n%s", out);
        fails++;
    }
}

static void line(const char *text)
{
    while (*text)
        bt_debug_feed(*text++);
    bt_debug_feed('\r');
}

static void module_says(const char *text)
{
    bluetooth_feed((const uint8_t *)text, (unsigned)strlen(text));
}

static void reset_io(void)
{
    out[0] = '\0';
    sent[0] = '\0';
}

int main(void)
{
    const bt_debug_io_t io = {
        .out = host_out, .millis = host_millis, .radio_ptt = host_radio_ptt,
        .tx_active = host_tx_active, .ptt_type = host_ptt_type,
        .restart = host_restart,
    };

    bt_debug_init(&io);
    bluetooth_set_event_cb(bt_service_event);
    bt_set_enabled(true);

    /* A '!' line is collected, and does not leak into the letter switch. */
    bt_debug_feed('!');
    check(bt_debug_active(), "'!' starts a debug line");
    bt_debug_feed('s');
    bt_debug_feed('\n');
    check(!bt_debug_active(), "CR/LF ends it");
    check(strstr(out, "BT 1000 S en=1 state=Reset") != NULL,
          "!s prints a 'BT <ms> S' status line");

    /* Trace: received lines carry their parser event, unknown ones NONE. */
    reset_io();
    line("!t1");
    check(bt_debug_tracing() && strstr(out, "BT 1000 T trace on") != NULL,
          "!t1 turns tracing on");
    module_says("+IM_BT_EAR_CONN\r\n");
    check(strstr(out, "BT 1000 < +IM_BT_EAR_CONN [BT_EAR_CONN]") != NULL,
          "a received line is traced with its event");
    check(strstr(out, "BT 1000 > AT+BT_CALL=ON") != NULL,
          "the service's reply (CALL=ON) is traced as sent");
    module_says("+IM_EAR_SIDE_SINGLE0\r\n");
    check(strstr(out, "< +IM_EAR_SIDE_SINGLE0 [NONE]") != NULL,
          "an unparsed line shows [NONE]");
    module_says("\r\n");
    check(strstr(out, "< \n") == NULL && strstr(out, "<  [") == NULL,
          "a blank CRLF line is not traced");

    /* PTT transitions: the first tick is the baseline, then changes print. */
    bt_debug_tick();
    reset_io();
    bt_debug_tick();
    check(out[0] == '\0', "no PTT line without a change");
    line("!i+IM_EAR_PTT_KEYDOWN");
    check(strstr(out, "BT 1000 I injected") != NULL &&
          strstr(out, "< +IM_EAR_PTT_KEYDOWN [EAR_PTT_DOWN]") != NULL,
          "!i feeds the line through the parser");
    check(bt_ptt_down(), "an injected KEYDOWN sets the headset PTT flag");
    bt_debug_tick();
    check(strstr(out, "P headset=1 radio=0 type=0 allow=1 tx=0") != NULL,
          "a headset PTT change prints the PTT decision");
    reset_io();
    type = 1u;                     /* Local only: the headset is filtered out */
    tx_on = false;
    line("!s");
    check(strstr(out, "ptt=1 radio_ptt=0 type=1 allow=0") != NULL,
          "status shows the PTT type filtering the headset");
    type = 0u;
    tx_on = true;
    reset_io();
    bt_debug_tick();
    check(strstr(out, "P headset=1 radio=0 type=0 allow=1 tx=1") != NULL,
          "a TX change prints too");

    /* Raw send. */
    reset_io();
    line("!aAT+GMR?");
    check(strcmp(sent, "AT+GMR?\r\n") == 0, "!a sends the text plus CRLF");
    check(strstr(out, "> AT+GMR?") != NULL && strstr(out, "A sent") != NULL,
          "!a is traced and confirmed");

    /* Restart, help, ESC, timeout. */
    reset_io();
    line("!r");
    check(restarts == 1u && strstr(out, "R restarting") != NULL,
          "!r calls the restart hook");
    reset_io();
    line("!?");
    check(strstr(out, "BT 1000 H ") != NULL, "!? prints help");
    reset_io();
    bt_debug_feed('!');
    bt_debug_feed('s');
    bt_debug_feed(0x1B);
    check(!bt_debug_active() && out[0] == '\0', "ESC drops a line unrun");
    bt_debug_feed('!');
    clock_ms += 6000u;
    bt_debug_tick();
    check(!bt_debug_active(), "an unterminated line times out");

    line("!t0");
    reset_io();
    module_says("+IM_SCO_CONN\r\n");
    check(strstr(out, "< +IM_SCO_CONN") == NULL, "!t0 stops the trace");

    printf("%d failed\n", fails);
    return fails != 0;
}
