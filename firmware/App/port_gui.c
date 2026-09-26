/* The port's screen and key loop.
 *
 * The K1's equivalent is app/main.c: it polls KEYBOARD_GetKey(), turns the raw
 * readings into press/hold edges, and routes them to the screen that is up --
 * MENU_ProcessKeys() for the menu, MAIN_Key_UP_DOWN() and friends for the VFO.
 * Only the second half of that exists here yet (app/menu.c came in with the
 * menu screen); this file is the small, explicit stand-in for the routing, so
 * the GUI can be driven by the radio's own keys now.  It is replaced by the
 * K1's app/main.c once the RF/audio engine behind it is in place.
 */
#include <stdbool.h>
#include <stdint.h>

#include "app/menu.h"
#include "driver/keyboard.h"
#include "driver/rx.h"
#include "driver/tx.h"
#include "driver/st7565.h"
#include "frequencies.h"
#include "functions.h"
#include "radio.h"
#include "settings.h"
#include "ui/main.h"
#include "ui/menu.h"
#include "ui/status.h"
#include "ui/welcome.h"
#include "ui/ui.h"

#define PORT_GUI_HOLD_POLLS 8u   /* polls before a key counts as held */

static KEY_Code_t s_last = KEY_INVALID;
static uint8_t    s_hold;
static bool       s_dirty = true;

/* Where the menu was opened from, so EXIT can go back to it. */
static GUI_DisplayType_t s_menu_return = DISPLAY_MAIN;

/* The port draws the two screens it has directly.  ui/ui.c's
 * UI_DisplayFunctions[] table (GUI_DisplayScreen) is the K1's way and takes
 * over as the remaining screens come in -- it is not used yet because it would
 * link every screen it lists, including the ones with no port behind them. */
static void port_gui_draw(void)
{
    /* The K1 keeps the status line separate from the screens and redraws it from
     * its app loop (app/app.c calls UI_DisplayStatus); doing it here keeps the
     * top line current on every repaint. */
    UI_DisplayStatus();

    switch (gScreenToDisplay) {
    case DISPLAY_MENU:
        UI_DisplayMenu();
        break;
    case DISPLAY_MAIN:
    default:
        UI_DisplayMain();
        break;
    }
}

static void port_gui_request(GUI_DisplayType_t screen)
{
    gRequestDisplayScreen = screen;
}

void port_gui_screen(GUI_DisplayType_t screen)
{
    gScreenToDisplay = screen;
    s_dirty = true;
}

void port_gui_init(void)
{
    port_gui_screen(DISPLAY_MAIN);
}

/* The K1's boot screen (App/main.c calls UI_DisplayWelcome once at start-up). */
void port_gui_welcome(void)
{
    UI_DisplayWelcome();
}

/* One step of the VFO's tuning step, in Hz.  The K1 keeps StepFrequency in
 * 10 Hz units (STEP_12_5kHz = 1250). */
static uint32_t port_gui_step(void)
{
    uint32_t step = gRxVfo->StepFrequency;

    if (step == 0u)
        step = 1250u;
    return step * 10u;
}

/* PTT: the radio's own measured transmit chain (driver/tx.c), not the K1's
 * chip sequence -- see ra89r_rfpath.md.  The K1 state is set too, so the
 * screens show TX and the status line follows. */
static void port_gui_ptt(bool down)
{
    if (down == tx_active())
        return;

    if (down) {
        FUNCTION_Select(FUNCTION_TRANSMIT);
        tx_start(gTxVfo->freq_config_TX.Frequency, TX_SOURCE_MIC);
    } else {
        tx_stop();
        FUNCTION_Select(FUNCTION_RECEIVE);
    }
    s_dirty = true;
}

void port_gui_poll(void)
{
    KEY_Code_t key = KEYBOARD_GetKey();
    bool       pressed = false;
    bool       held = false;
    bool       ptt;

    /* PTT works on any screen, like the K1's. */
    ptt = (key == KEY_PTT) || (key == KEY_PTT2);
    port_gui_ptt(ptt);

    if (key == KEY_INVALID) {
        s_last = KEY_INVALID;
        s_hold = 0;
    }

    /* Consume a pending screen request: the K1's screens set
     * gRequestDisplayScreen to ask to be replaced, and never clear it. */
    if (gRequestDisplayScreen != DISPLAY_INVALID) {
        if (gRequestDisplayScreen != gScreenToDisplay) {
            gScreenToDisplay = gRequestDisplayScreen;
            s_dirty = true;
        }
        gRequestDisplayScreen = DISPLAY_INVALID;
    }

    /* Every key but PTT is the K1's own business: app/app.c's CheckKeys()
     * (called from the main loop) routes them to MAIN_/MENU_/SCANNER_ProcessKeys
     * with the K1's press/hold/repeat semantics.  Only PTT is handled here,
     * because it drives the measured transmit chain. */
    (void)key;
    (void)pressed;
    (void)held;

    if (s_dirty) {
        s_dirty = false;
        port_gui_draw();
    }
}

/* The K1 refreshes its screens from its app loop: the status line, the signal
 * read-out and the clock change on their own.  This repaints on a timer as well
 * as on a screen change.  The panel is bit-banged, so it is deliberately slow. */
#define PORT_GUI_REFRESH_MS 500u

void port_gui_tick(uint32_t now_ms)
{
    static uint32_t last;
    static bool     squelch_open;

    /* The receiver's own poll: the squelch marks are the stock's, and the RSSI
     * the screen reads comes from the chip (BK4819_GetRSSI). */
    if (rx_ready())
        rx_poll();

    if (rx_ready() && rx_squelch_open() != squelch_open) {
        squelch_open = rx_squelch_open();
        FUNCTION_Select(squelch_open ? FUNCTION_INCOMING : FUNCTION_RECEIVE);
        s_dirty = true;
    }

    if ((uint32_t)(now_ms - last) < PORT_GUI_REFRESH_MS)
        return;
    last = now_ms;
    s_dirty = true;
}
