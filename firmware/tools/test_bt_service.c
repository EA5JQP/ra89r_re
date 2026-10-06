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
#include "driver/audio_path.h"
#include "driver/bluetooth.h"

bool gUpdateDisplay;

extern void bt_set_speaker_switch(bool enabled);
extern void bt_set_radio_tx_active(bool active);

static int host_audio_path = 1;
static int host_bt_rf_path;

void audio_path_drive(int on) { host_audio_path = on != 0; }
void pa_set_bt_audio(bool on) { host_bt_rf_path = on ? 1 : 0; }

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

static void test_bt_ptt_source_matrix(void)
{
    static const struct {
        bool radio;
        bool headset;
        bool bt_mode;
        bool radio_mode;
        bool both_mode;
    } cases[] = {
        { false, false, false, false, false },
        { false, true,  true,  false, true  },
        { true,  false, false, true,  true  },
        { true,  true,  true,  true,  true  }
    };
    unsigned i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        check(bt_ptt_source_active(cases[i].radio, cases[i].headset, 0u) ==
                  cases[i].bt_mode,
              "BT mode uses headset PTT only");
        check(bt_ptt_source_active(cases[i].radio, cases[i].headset, 1u) ==
                  cases[i].radio_mode,
              "Radio mode uses radio PTT only");
        check(bt_ptt_source_active(cases[i].radio, cases[i].headset, 2u) ==
                  cases[i].both_mode,
              "Both mode accepts either PTT source");
    }
    check(bt_ptt_source_active(true, false, 3u) == false,
          "invalid PTT mode fails closed to BT-only");
    check(bt_ptt_source_active(false, true, 3u) == true,
          "invalid PTT mode retains BT PTT only");
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
    bt_service_event(BT_EV_OK, NULL, 0);   /* +OK advances BT_SCANATCN=ON */
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
    test_bt_ptt_source_matrix();

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

    /* With a name set, AT+WRITE_NAME is inserted after the mode.  Re-arm the
     * service (a real module sends +IM_READY once, after its reset). */
    bt_set_enabled(false);
    bt_set_enabled(true);
    rec_reset();
    bt_set_name("RA89R");
    bt_service_event(BT_EV_READY, NULL, 0);
    check(rec_n == 1u && rec_is(0, "AT+GMR?\r\n"), "name: AT+GMR? first");
    drive_config("name");

    rec_reset();
    bt_set_enabled(false);
    check(bt_state() == BT_STATE_OFF, "disable -> OFF");
    check(rec_last_is("AT+BT_DISCN\r\n"), "disable sends AT+BT_DISCN");

    /* A silent module must not spin forever: the reset attempt is bounded
     * (a few PD0 re-pulses, then OFF). */
    bt_set_enabled(true);
    for (i = 0; i < 3000u; i++)
        bt_service_tick();
    check(bt_state() == BT_STATE_OFF, "silent module times out to OFF");

    /* --- review fixes ---------------------------------------------------- */

    /* A late +IM_READY (the module's boot banner) must not restart a service
     * that is off. */
    bt_set_enabled(false);
    rec_reset();
    bt_service_event(BT_EV_READY, NULL, 0);
    check(rec_n == 0u && bt_state() == BT_STATE_OFF,
          "READY while disabled is ignored");

    /* +IM_BT_SCAN_STOP is unsolicited; it must not advance the config queue. */
    bt_set_enabled(true);
    rec_reset();
    bt_service_event(BT_EV_READY, NULL, 0);
    check(rec_n == 1u && rec_is(0, "AT+GMR?\r\n"), "READY queues AT+GMR?");
    bt_service_event(BT_EV_BT_SCAN_STOP, NULL, 0);
    check(rec_n == 1u, "SCAN_STOP does not advance the queue");

    /* The gain/scan setters send the stock's own strings. */
    rec_reset();
    bt_set_spk_gain(2);
    check(rec_last_is("AT+SPKGAIN=8\r\n"), "spk gain 2 -> AT+SPKGAIN=8");
    bt_set_mic_gain(1);
    check(rec_last_is("AT+MICGAIN=5\r\n"), "mic gain 1 -> AT+MICGAIN=5");
    bt_set_scan(true);
    check(rec_last_is("AT+BT_SCAN=ON\r\n"), "scan on -> AT+BT_SCAN=ON");
    bt_set_scan(false);
    check(rec_last_is("AT+BT_SCAN=OFF\r\n"), "scan off -> AT+BT_SCAN=OFF");

    /* A found device arrives asynchronously while the BT pairing screen is
     * open.  The UI must be scheduled immediately, not wait for another key. */
    {
        static const char device[] = "A1B2C3D4E5F6,Test headset,-42";

        bt_start_connect();
        gUpdateDisplay = false;
        bt_service_event(BT_EV_EARDEV, device, (unsigned)strlen(device));
        check(bt_found_count() == 1u && gUpdateDisplay,
              "new pairing device schedules an immediate BT-menu refresh");
    }

    /* The earpiece PTT button (`+IM_EAR_PTT_KEYDOWN/UP`) keys the transmitter. */
    check(!bt_ptt_down(), "earpiece PTT starts up");
    bt_service_event(BT_EV_EAR_PTT_DOWN, NULL, 0);
    check(bt_ptt_down(), "EAR_PTT_KEYDOWN -> down");
    bt_service_event(BT_EV_EAR_PTT_UP, NULL, 0);
    check(!bt_ptt_down(), "EAR_PTT_KEYUP -> up");

    /* The earpiece's own button click (`+IM_EAR_SIDE_SINGLE1`) toggles it. */
    bt_service_event(BT_EV_EAR_SIDE_SINGLE, NULL, 0);
    check(bt_ptt_down(), "EAR_SIDE_SINGLE toggles PTT on");
    bt_service_event(BT_EV_EAR_SIDE_SINGLE, NULL, 0);
    check(!bt_ptt_down(), "EAR_SIDE_SINGLE toggles PTT off");

    /* Stock FUN_0801AE8C -> FUN_080075A0(0): on earpiece connect the module's
     * mic and speaker gains are set from codeplug byte 8 before the call is
     * opened.  bt_set_gain_levels() seeds the levels the app read from the
     * codeplug (or the menu). */
    bt_set_gain_levels(4, 3);
    rec_reset();
    bt_service_event(BT_EV_BT_EAR_CONN, NULL, 0);
    check(rec_n == 3u && rec_is(0, "AT+MICGAIN=8\r\n") &&
          rec_is(1, "AT+SPKGAIN=16\r\n") && rec_is(2, "AT+BT_CALL=ON\r\n"),
          "earpiece connect sets module gains then opens SCO");
    rec_reset();
    bt_service_event(BT_EV_BT_DISCONNECT, NULL, 0);
    check(rec_n == 0u && !bt_connected(),
          "BT disconnect closes the link without redundant CALL=OFF");
    bt_set_gain_levels(2, 2);

    /* The stock clears its SCO/call flag on these events but does not send
     * BT_CALL=OFF; it re-opens CALL on the next receive T/R transition. */
    bt_set_enabled(true);
    bt_service_event(BT_EV_BT_EAR_CONN, NULL, 0);
    bt_set_radio_tx_active(true);
    rec_reset();
    bt_service_event(BT_EV_SCO_DISCONNECT, NULL, 0);
    check(rec_n == 0u && bt_connected(),
          "SCO loss during TX keeps the BT link and does not send CALL=OFF");
    bt_set_radio_tx_active(false);
    check(rec_n == 1u && rec_is(0, "AT+BT_CALL=ON\r\n"),
          "TX-to-RX transition re-opens CALL after SCO disconnect");
    rec_reset();
    bt_set_radio_tx_active(true);
    bt_service_event(BT_EV_CALL_DISCONNECTED, NULL, 0);
    bt_service_event(BT_EV_SCO_DISCONNECT, NULL, 0);
    bt_service_tick();
    check(rec_n == 0u && bt_connected(),
          "paired SCO/call teardown stays pending during TX without CALL=OFF");
    bt_set_radio_tx_active(false);
    check(rec_n == 1u && rec_is(0, "AT+BT_CALL=ON\r\n"),
          "paired disconnect events produce one CALL restart on TX-to-RX");
    rec_reset();
    bt_service_event(BT_EV_BT_DISCONNECT, NULL, 0);

    /* On-radio capture: after the earpiece links, the module reports
     * +IM_SCO_CONN / +IM_SCO_DISCN / +OK over and over -- each 10 ms tick
     * re-sent CALL=ON.  A SCO loss while the radio idles in RX must not be
     * answered with a CALL=ON storm; stock reopens CALL only on the receive
     * T/R transition. */
    bt_set_enabled(true);
    bt_service_event(BT_EV_BT_EAR_CONN, NULL, 0);       /* link -> CALL=ON */
    rec_reset();
    bt_service_event(BT_EV_SCO_CONN, NULL, 0);
    bt_service_event(BT_EV_SCO_DISCONNECT, NULL, 0);
    for (i = 0; i < 50u; i++)
        bt_service_tick();
    check(rec_n == 0u && bt_connected(),
          "idle-RX SCO loss does not trigger a CALL=ON storm");
    bt_set_radio_tx_active(true);
    bt_set_radio_tx_active(false);
    check(rec_n == 1u && rec_is(0, "AT+BT_CALL=ON\r\n"),
          "receive transition reopens CALL once after idle SCO loss");
    rec_reset();
    bt_service_event(BT_EV_BT_DISCONNECT, NULL, 0);

    /* BT link enforces the user's BT-only audio route.  No later K1 audio-path
     * request may reopen the local speaker until the BT link drops. */
    bt_set_speaker_switch(false);
    check(host_audio_path == 1, "PC13 idles high before the BT link");
    bt_service_event(BT_EV_BT_EAR_CONN, NULL, 0);
    check(host_audio_path == 0, "Speak Switch off drives PC13 low while linked");
    check(host_bt_rf_path == 1, "BT link sets the stock chip audio output");
    audio_path_drive(1);
    check(host_audio_path == 1,
          "later audio-path requests are not clamped by unverified BT routing");
    audio_path_drive(0);
    bt_service_event(BT_EV_SCO_DISCONNECT, NULL, 0);
    check(host_audio_path == 0 && host_bt_rf_path == 1,
          "SCO loss alone keeps BT-link-controlled stock paths active");
    bt_service_event(BT_EV_BT_DISCONNECT, NULL, 0);
    check(host_audio_path == 1, "PC13 returns high after BT disconnect");
    check(host_bt_rf_path == 0, "BT disconnect clears the stock chip audio output");

    bt_set_speaker_switch(true);
    bt_service_event(BT_EV_BT_EAR_CONN, NULL, 0);
    check(host_audio_path == 1, "Speak Switch on retains stock PC13 level while linked");
    bt_service_event(BT_EV_BT_DISCONNECT, NULL, 0);
    bt_service_event(BT_EV_BT_EAR_CONN, NULL, 0);
    bt_set_enabled(false);
    check(host_audio_path == 1 && host_bt_rf_path == 0,
          "disabling BT restores PC13 and clears the chip BT output");

    printf("\n%d failed\n", fails);
    return fails ? 1 : 0;
}
