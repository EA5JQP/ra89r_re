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
#include "helper/boot.h"
#include "host_hw.h"
#include "py32f4xx.h"      /* TIM7/DMA/SYSCFG scratch, for the backlight checks */
#include "misc.h"
#include "settings.h"
#include "radio.h"
#include "settings.h"
#include "ui/main.h"
#include "driver/tx.h"
#include "driver/rx.h"
#include "ui/welcome.h"
#include "ui/ui.h"
#include "driver/py25q16.h"
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
    tx_poll_ptt();
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
    SETTINGS_InitEEPROM(); SETTINGS_LoadCalibration(); RADIO_ConfigureChannel(0, VFO_CONFIGURE_RELOAD); RADIO_ConfigureChannel(1, VFO_CONFIGURE_RELOAD); RADIO_SelectVfos(); SETTINGS_FixupVfoPointers();

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
        SETTINGS_InitEEPROM(); SETTINGS_LoadCalibration(); RADIO_ConfigureChannel(0, VFO_CONFIGURE_RELOAD); RADIO_ConfigureChannel(1, VFO_CONFIGURE_RELOAD); RADIO_SelectVfos(); SETTINGS_FixupVfoPointers();
    }

    /* ---- the boot screen and the status line ---------------------------- */
    gScreenToDisplay = DISPLAY_MAIN; gUpdateDisplay = true;
    UI_DisplayWelcome();
    render("K1 UI_DisplayWelcome(): the ported boot screen");

    /* ---- the key loop: the K1's own CheckKeys() driving the screens ------- */
    /* No hand-set frequency: the state is whatever the codeplug boot left, so
     * the lines below say what the keys actually do. */
    gScreenToDisplay = DISPLAY_MAIN; gUpdateDisplay = true;
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
        bool           ok;

        gWasFKeyPressed    = true;
        gKeyInputCountdown = key_input_timeout_500ms;
        press(KEY_3);
        printf("[keys] F+3: screen A %u (%s) -> %u (%s), VFO A %u.%05u MHz\n",
               (unsigned)before, IS_MR_CHANNEL(before) ? "channel" : "frequency",
               (unsigned)gEeprom.ScreenChannel[0],
               IS_MR_CHANNEL(gEeprom.ScreenChannel[0]) ? "channel" : "frequency",
               (unsigned)(gRxVfo->freq_config_RX.Frequency / 100000u),
               (unsigned)(gRxVfo->freq_config_RX.Frequency % 100000u));

        /* The F modifier is one-shot: the F+key combination releases it (the
         * K1's HideFKeyIcon).  A stub here left it latched until F was pressed
         * again. */
        ok = !gWasFKeyPressed;
        printf("[keys] %s F released after the F+key combination\n", ok ? "ok  " : "FAIL");
        if (!ok)
            failures++;
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

    /* The K1's boot-time key mode (helper/boot.c): PTT + SIDE1 held at power-on
     * opens the hidden menu.  The host stands in for the keypad and PB9. */
    {
        bool         ok;
        BOOT_Mode_t  m;

        host_set_key(KEY_SIDE1);
        host_set_ptt2(true);
        m = BOOT_GetMode();
        ok = (m == BOOT_MODE_F_LOCK);
        printf("[boot] %s PTT+SIDE1 -> F-lock\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        host_set_ptt2(false);
        m = BOOT_GetMode();
        ok = (m == BOOT_MODE_NORMAL);
        printf("[boot] %s SIDE1 without PTT -> normal\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        host_set_key(KEY_SIDE2);
        host_set_ptt2(true);
        m = BOOT_GetMode();
        ok = (m == BOOT_MODE_NORMAL);
        printf("[boot] %s PTT+SIDE2 -> normal (air-copy is off here)\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        host_set_key(KEY_INVALID);
        host_set_ptt2(false);
    }

    /* The K1 backlight driver: the logic is host-testable, the TIM7/DMA writes
     * land in scratch.  See docs/ra89r_led.md. */
    {
        bool ok;
        int  i;

        gEeprom.BACKLIGHT_TIME = 4;
        gEeprom.BACKLIGHT_MIN  = 1;
        gEeprom.BACKLIGHT_MAX  = 5;

        ok = value[0] == 0 && value[1] == 8 && value[5] == 48 && value[10] == 255;
        printf("[backlight] %s value[] is the F4HWN table\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        /* The keypad's ADC1 map lives in CFGR[2] bits[6:0]; the backlight uses
         * bits[14:8] and must not disturb it. */
        SYSCFG->CFGR[2] = 0x12345678u;
        BACKLIGHT_InitHardware();
        ok = (SYSCFG->CFGR[2] & 0x7Fu) == 0x78u && ((SYSCFG->CFGR[2] >> 8) & 0x7Fu) == 0x35u;
        printf("[backlight] %s InitHardware keeps CFGR[2] bits[6:0], sets [14:8]=0x35\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        ok = !BACKLIGHT_IsOn();
        printf("[backlight] %s InitHardware leaves the panel dark\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        /* The DMA must drive the pin: memory -> peripheral. */
        ok = (DMA1_Channel2->CCR & DMA_CCR_DIR) != 0;
        printf("[backlight] %s DMA is memory->peripheral (DIR set)\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        BACKLIGHT_TurnOn();
        ok = BACKLIGHT_IsOn() && gBacklightCountdown_500ms == 41u;
        printf("[backlight] %s TurnOn(TIME=4) lights it, countdown 41\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        /* Update() runs on the 10 ms slice and must not touch the 500 ms timer. */
        for (i = 0; i < 20; i++) BACKLIGHT_Update();
        ok = gBacklightCountdown_500ms == 41u;
        printf("[backlight] %s Update() leaves the countdown alone\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        BACKLIGHT_SetBrightness(5);
        for (i = 0; i < 64; i++) BACKLIGHT_Update();
        ok = BACKLIGHT_DutyOnCount() == (48u * 32u / 255u);   /* == 6 */
        printf("[backlight] %s fade to index 5 -> duty level 6\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        /* MIN=0 is a true dark idle: index 0 stops the PWM. */
        gEeprom.BACKLIGHT_MIN = 0;
        BACKLIGHT_SetBrightness(0);
        for (i = 0; i < 64; i++) BACKLIGHT_Update();
        ok = !(TIM7->CR1 & TIM_CR1_CEN);
        printf("[backlight] %s index 0 stops the PWM\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        gEeprom.BACKLIGHT_TIME = 61;
        BACKLIGHT_TurnOn();
        ok = BACKLIGHT_IsOn() && gBacklightCountdown_500ms == 0u;
        printf("[backlight] %s TIME=61 is always-on (no countdown)\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        gEeprom.BACKLIGHT_TIME = 0;
        BACKLIGHT_TurnOn();
        ok = !BACKLIGHT_IsOn();
        printf("[backlight] %s TIME=0 leaves it off\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;
    }

    printf("\n%d failures\n", failures);
    return failures ? 1 : 0;
}
