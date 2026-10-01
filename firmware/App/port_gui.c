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
#include "driver/led.h"
#include "driver/rx.h"
#include "driver/tx.h"
#include "driver/st7565.h"
#include "frequencies.h"
#include "functions.h"
#include "misc.h"
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

/* Where the menu was opened from, so EXIT can go back to it. */
static GUI_DisplayType_t s_menu_return = DISPLAY_MAIN;

void port_gui_screen(GUI_DisplayType_t screen)
{
    /* The application repaints when gUpdateDisplay is set (app/app.c calls
     * GUI_DisplayScreen), so the port only has to ask for the screen. */
    gScreenToDisplay = screen;
    gUpdateDisplay = true;
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
 * chip sequence -- see docs/ra89r_rfpath.md.  The K1 state is set too, so the
 * screens show TX and the status line follows. */
static void port_gui_ptt(bool down)
{
    if (down == tx_active())
        return;

    if (down) {
        FUNCTION_Select(FUNCTION_TRANSMIT);
        tx_start(gTxVfo->freq_config_TX.Frequency,
                 gTxVfo->TXP_CalculatedSetting, TX_SOURCE_MIC);
    } else {
        tx_stop();
        FUNCTION_Select(FUNCTION_RECEIVE);
    }
    gUpdateDisplay = true;
}

void port_gui_poll(void)
{
    KEY_Code_t key = KEYBOARD_GetKey();
    bool       ptt;

    /* Every key but PTT is the K1's own business: app/app.c's CheckKeys(),
     * called from APP_TimeSlice10ms(), routes them to MAIN_/MENU_/
     * SCANNER_ProcessKeys with the K1's press/hold/repeat semantics.  Only PTT
     * is handled here, because it drives the measured transmit chain. */
    ptt = (key == KEY_PTT) || (key == KEY_PTT2);
    port_gui_ptt(ptt);
}

/* The K1 refreshes its screens from its app loop: the status line, the signal
 * read-out and the clock change on their own.  This repaints on a timer as well
 * as on a screen change.  The panel is bit-banged, so it is deliberately slow. */
#define PORT_GUI_REFRESH_MS 500u

void port_gui_tick(uint32_t now_ms)
{
    static bool squelch_open;

    /* The receiver's own poll: the squelch marks are the stock's, and the RSSI
     * the screen reads comes from the chip (BK4819_GetRSSI). */
    if (rx_ready())
        rx_poll();

    /* The K1 application tunes the chip directly (radio.c), but the port's own
     * tick is the only place that tracks the RX frequency, so retune here too:
     * `rx_set_frequency()` writes the BK4829's 0x38/0x39 and re-applies the band
     * path (PA0/PA1, the BK4815 band register 0x75 and the chip's RX path bit)
     * when the GUI frequency moves.  This makes the configured RX frequency
     * authoritative even on a path that skips RADIO_SetupRegisters(). */
    {
        static uint32_t last_freq;
        uint32_t freq = (gRxVfo != 0) ? gRxVfo->pRX->Frequency : 0u;

        if (freq != last_freq) {
            last_freq = freq;
            if (freq != 0u)
                rx_set_frequency(freq);
        }
    }

    if (rx_ready() && rx_squelch_open() != squelch_open) {
        squelch_open = rx_squelch_open();

        /* The K1 app's whole receive path keys off `g_SquelchLost`:
         * CheckForIncoming() promotes FOREGROUND to INCOMING on it, and
         * HandleIncoming() reverts to FOREGROUND without it.  It is normally set
         * by the chip's squelch interrupt, which this port never arms -- its
         * rx_init() leaves REG_3F at BK4819_Init()'s 0 -- so the flag stayed
         * false and setting FUNCTION_INCOMING was undone on the next slice.  That
         * is why 0x67 could say "open" (console 'S') with no screen, status or
         * LED change.  Publish the polled result instead, and mirror the level
         * CheckRadioInterrupts() would have put on the green LED. */
        g_SquelchLost = squelch_open;
        if (!tx_active())
            led_set(squelch_open ? LED_GREEN : LED_OFF);
        gUpdateDisplay = true;
        gUpdateStatus  = true;
    }

    /* The application repaints on its own (gUpdateDisplay); this tick only
     * needs to keep the receiver polled, so it stays cheap. */
    (void)now_ms;
    (void)PORT_GUI_REFRESH_MS;
}
