/* preview_k1.c -- render the ported K1 screens on a PC.
 *
 * The VFO screen is the K1's own ui/main.c (UI_DisplayMain) compiled against
 * the port's headers, buffers and settings layer; this tool sets the state up,
 * calls it, and prints the panel as text.  Page 0 is the status line,
 * pages 1..7 are gFrameBuffer[0..6].
 *
 * It links the whole application core, not just the screens: port_state_init()
 * runs the K1's real boot sequence and the key loop runs the K1's real
 * CheckKeys(), so the codeplug decoder and the key routing are exercised too.
 * AGENTS.md, "Offline checks", has the current command.
 */
#include <stdio.h>

#include "app/app.h"
#include "driver/backlight.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "host_hw.h"
#include "misc.h"
#include "port_codeplug.h"
#include "port_state.h"
#include "radio.h"
#include "settings.h"
#include "ui/main.h"
#include "port_gui.h"
#include "port_storage.h"
#include "ui/menu.h"

/* State assertions this preview checks (the panel rendering is eyeballed). */
static int failures;

/* One pass of the firmware's own loop (firmware/App/main.c): the application
 * state machine and panel, the port's transmit check, then the 10 ms slice --
 * which is where the K1's CheckKeys() lives and therefore the only place a key
 * does anything.  The preview has to run all three or it is only pressing keys
 * at a radio that is not listening. */
static void step(void)
{
    APP_Update();
    port_gui_poll();
    APP_TimeSlice10ms();
}

/* A key event, as the radio sees it: hold the key for enough 10 ms slices that
 * CheckKeys()'s debounce (key_debounce_10ms) passes, then release it and give
 * the release the same treatment. */
static void press(KEY_Code_t key)
{
    unsigned int i;

    host_set_key(key);
    for (i = 0; i < 4; i++)
        step();
    host_set_key(KEY_INVALID);
    for (i = 0; i < 4; i++)
        step();
}

static void render(const char *title)
{
    unsigned x;
    unsigned y;

    printf("\n=== %s ===\n    +", title);
    for (x = 0; x < LCD_WIDTH; x++)
        putchar('-');
    printf("+\n");

    for (y = 0; y < LCD_HEIGHT; y++) {
        const uint8_t *page = (y < 8) ? gStatusLine : gFrameBuffer[(y >> 3) - 1];
        printf("%3u |", y);
        for (x = 0; x < LCD_WIDTH; x++)
            putchar((page[x] >> (y & 7u)) & 1u ? '#' : ' ');
        printf("|\n");
    }

    printf("    +");
    for (x = 0; x < LCD_WIDTH; x++)
        putchar('-');
    printf("+\n");
}

int main(void)
{
    ST7565_Init();
    port_state_init();

    /* ---- the codeplug: what the stock's records decode to ---------------- */
    {
        unsigned int channel;

        printf("\n[codeplug] the host's synthetic stock image\n");
        printf("  screen A = %u, screen B = %u (the first two channels it has)\n",
               (unsigned)gEeprom.ScreenChannel[0], (unsigned)gEeprom.ScreenChannel[1]);

        for (channel = 0; channel < 8u; channel++) {
            ChannelScanDisplayInfo_t info;
            char name[16];

            if (!port_codeplug_scan_info(channel, &info)) {
                printf("  CH %2u  -- not used\n", channel);
                continue;
            }

            port_codeplug_name(name, sizeof name, channel);
            printf("  CH %2u  %3u.%05u MHz  tx %3u.%05u  %-6s  code rx %u/%u tx %u/%u  "
                   "step %u  power %u  bw %u  txlock %u  excl %u\n",
                   channel,
                   (unsigned)(info.rx.Frequency / 100000u), (unsigned)(info.rx.Frequency % 100000u),
                   (unsigned)(info.tx.Frequency / 100000u), (unsigned)(info.tx.Frequency % 100000u),
                   name,
                   (unsigned)info.rx.CodeType, (unsigned)info.rx.Code,
                   (unsigned)info.tx.CodeType, (unsigned)info.tx.Code,
                   (unsigned)info.stepSetting, (unsigned)info.outputPower,
                   (unsigned)info.channelBandwidth, (unsigned)info.txLock,
                   (unsigned)port_codeplug_excluded(channel));
        }

        printf("  VFO A: %3u.%05u MHz, bandwidth %u, power %u, TX_LOCK %u\n",
               (unsigned)(gRxVfo->freq_config_RX.Frequency / 100000u),
               (unsigned)(gRxVfo->freq_config_RX.Frequency % 100000u),
               (unsigned)gRxVfo->CHANNEL_BANDWIDTH, (unsigned)gRxVfo->OUTPUT_POWER,
               (unsigned)gRxVfo->TX_LOCK);
    }

    UI_DisplayMain();
    render("K1 UI_DisplayMain(): as the codeplug configured it (CH-01, 144.9750)");

    gRxVfo->freq_config_RX.Frequency = 14575000u;
    gRxVfo->freq_config_TX.Frequency = 14575000u;
    UI_DisplayMain();
    render("K1 UI_DisplayMain(): 145.7500 MHz, channel, status line");

    /* The menu's entry sequence (app/main.c): build the view, then draw it.
     * ENABLE_FEAT_F4HWN_MENU_CAT is off (the K1's default preset), so this is
     * the flat list. */
    UI_MENU_BuildView();
    UI_DisplayMenu();
    render("K1 UI_DisplayMenu(): the menu list, first item");

    gMenuCursor = 3;
    UI_DisplayMenu();
    render("K1 UI_DisplayMenu(): the menu list, fourth item");

    gRxVfo->freq_config_RX.Frequency = 43350000u;
    gRxVfo->freq_config_TX.Frequency = 43350000u;
    UI_DisplayMain();
    render("K1 UI_DisplayMain(): 433.5000 MHz");

    /* ---- storage: the settings blob on the external flash --------------- */
    {
        uint32_t bad = 0;
        bool saved, loaded;

        gEeprom.SQUELCH_LEVEL = 7;
        gEeprom.CHANNEL_DISPLAY_MODE = 2;
        saved = port_storage_save_settings();

        gEeprom.SQUELCH_LEVEL = 0;
        gEeprom.CHANNEL_DISPLAY_MODE = 0;
        loaded = port_storage_load_settings();

        printf("\n[storage] save=%d load=%d -> squelch=%u chdisp=%u (expected 7/2)\n",
               saved, loaded, (unsigned)gEeprom.SQUELCH_LEVEL,
               (unsigned)gEeprom.CHANNEL_DISPLAY_MODE);
        printf("[storage] write test=%d\n", port_storage_write_test(&bad));

        /* Put the state back so the screens below render as before (a plain
         * PORT_SettingsDefaults() would clear the VFO pointers those screens
         * dereference -- port_state_init() re-establishes them). */
        port_state_init();
    }

    /* ---- the boot screen and the status line ---------------------------- */
    port_gui_init();
    port_gui_welcome();
    render("K1 UI_DisplayWelcome(): the ported boot screen");

    /* ---- the key loop: the K1's own CheckKeys() driving the screens ------- */
    /* No hand-set frequency: the state is whatever the codeplug boot left, so
     * the lines below say what the keys actually do. */
    port_gui_init();
    printf("\n[keys] start: screen A = %u, VFO A %u.%05u MHz\n",
           (unsigned)gEeprom.ScreenChannel[0],
           (unsigned)(gRxVfo->freq_config_RX.Frequency / 100000u),
           (unsigned)(gRxVfo->freq_config_RX.Frequency % 100000u));

    /* Two things must not happen on the 10 ms slice, and both once did: a flash
     * sector erase (a whole-blob save flushed on every slice) and a backlight
     * countdown decrement (the fade step mistaken for the timeout).  A key that
     * changes the channel may ask for *one* deferred save; the idle slices that
     * follow must ask for none. */
    {
        unsigned int erases;

        BACKLIGHT_TurnOn();
        erases = host_flash_erase_count();
        press(KEY_UP);                 /* the K1's UP/DOWN key action ... */
        printf("[keys] after KEY_UP: screen A = %u, VFO A %u.%05u MHz\n",
               (unsigned)gEeprom.ScreenChannel[0],
               (unsigned)(gRxVfo->freq_config_RX.Frequency / 100000u),
               (unsigned)(gRxVfo->freq_config_RX.Frequency % 100000u));
        printf("[keys] flash erases for the channel change: %u (one deferred save)\n",
               host_flash_erase_count() - erases);
        printf("[keys] backlight countdown: %u of %u (unchanged by the 10 ms slices)\n",
               (unsigned)gBacklightCountdown_500ms,
               (unsigned)(1u + (gEeprom.BACKLIGHT_TIME * 5u) * 2u));
        render("CheckKeys(): after a KEY_UP press (see the [keys] line above)");

        erases = host_flash_erase_count();
        {
            int i;
            for (i = 0; i < 40; i++)
                step();
        }
        printf("[keys] flash erases over 40 idle slices: %u (must be 0)\n",
               host_flash_erase_count() - erases);
    }

    /* The VFO/channel switch, which is what a radio that cannot change mode is
     * missing: the K1's path is F (the # key) and then 3. */
    {
        const uint16_t before = gEeprom.ScreenChannel[0];

        gWasFKeyPressed    = true;
        gKeyInputCountdown = key_input_timeout_500ms;
        press(KEY_3);
        printf("[keys] F+3: screen A %u (%s) -> %u (%s), VFO A %u.%05u MHz\n",
               (unsigned)before, IS_MR_CHANNEL(before) ? "channel" : "frequency",
               (unsigned)gEeprom.ScreenChannel[0],
               IS_MR_CHANNEL(gEeprom.ScreenChannel[0]) ? "channel" : "frequency",
               (unsigned)(gRxVfo->freq_config_RX.Frequency / 100000u),
               (unsigned)(gRxVfo->freq_config_RX.Frequency % 100000u));
    }

    press(KEY_MENU);                   /* ... then open the menu ... */
    printf("[keys] after KEY_MENU: screen = %u (DISPLAY_MENU = %u)\n",
           (unsigned)gScreenToDisplay, (unsigned)DISPLAY_MENU);
    render("CheckKeys(): KEY_MENU opened the menu");

    {
        const unsigned int before = gMenuCursor;
        int i;

        for (i = 0; i < 3; i++)        /* ... and walk down three items */
            press(KEY_DOWN);
        printf("[keys] after three KEY_DOWN: menu cursor %u -> %u\n",
               before, (unsigned)gMenuCursor);
    }
    render("CheckKeys(): three KEY_DOWN presses moved the cursor");

    /* The VFO switch.  This radio has no A/B key, so a short EXIT with nothing
     * to cancel is the port's binding (app/main.c MAIN_Key_EXIT ->
     * COMMON_SwitchVFOs).  The receiver must follow it: `gRxVfo` is what the
     * port's RX tick tunes, so a stale `gRxVfo` here is exactly why the radio
     * kept receiving on the previous VFO. */
    {
        unsigned int tx_before;
        uint32_t     rx_before;
        bool         ok;
        int          i;

        gScreenToDisplay = DISPLAY_MAIN;
        tx_before = gEeprom.TX_VFO;
        rx_before = gRxVfo->freq_config_RX.Frequency;

        press(KEY_EXIT);

        ok = (gEeprom.TX_VFO != tx_before)
             && (gEeprom.RX_VFO == gEeprom.TX_VFO)
             && (gRxVfo == &gEeprom.VfoInfo[gEeprom.TX_VFO]);

        printf("[vfo] %s EXIT: TX_VFO %u -> %u, RX_VFO %u, gRxVfo %u.%05u -> "
               "%u.%05u (VFO0 %u.%05u, VFO1 %u.%05u)\n",
               ok ? "ok  " : "FAIL", tx_before, (unsigned)gEeprom.TX_VFO,
               (unsigned)gEeprom.RX_VFO,
               (unsigned)(rx_before / 100000u), (unsigned)(rx_before % 100000u),
               (unsigned)(gRxVfo->freq_config_RX.Frequency / 100000u),
               (unsigned)(gRxVfo->freq_config_RX.Frequency % 100000u),
               (unsigned)(gEeprom.VfoInfo[0].freq_config_RX.Frequency / 100000u),
               (unsigned)(gEeprom.VfoInfo[0].freq_config_RX.Frequency % 100000u),
               (unsigned)(gEeprom.VfoInfo[1].freq_config_RX.Frequency / 100000u),
               (unsigned)(gEeprom.VfoInfo[1].freq_config_RX.Frequency % 100000u));
        if (!ok)
            failures++;

        /* And it must stay there: the K1's dual-watch engine would toggle the
         * receive VFO back on the next slices. */
        for (i = 0; i < 20; i++)
            step();

        ok = (gEeprom.RX_VFO == gEeprom.TX_VFO)
             && (gRxVfo == &gEeprom.VfoInfo[gEeprom.TX_VFO]);
        printf("[vfo] %s the receive VFO stays on the selected one over 20 slices "
               "(TX_VFO %u, RX_VFO %u)\n",
               ok ? "ok  " : "FAIL", (unsigned)gEeprom.TX_VFO,
               (unsigned)gEeprom.RX_VFO);
        if (!ok)
            failures++;
    }

    printf("\n%d failures\n", failures);
    return failures ? 1 : 0;
}
