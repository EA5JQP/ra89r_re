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
#include <string.h>

#include "app/app.h"
#include "app/action.h"
#include "app/chFrScanner.h"
#include "app/fm.h"
#include "app/menu.h"
#include "audio.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "helper/boot.h"
#include "helper/battery.h"
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
#include "driver/scheduler.h"
#include "ui/menu.h"
#include "ui/status.h"

/* State assertions this preview checks (the panel rendering is eyeballed). */
static int failures;

/* One pass of the firmware's own loop (firmware/App/main.c): the application
 * state machine and panel, the port's transmit check, then the 10 ms slice --
 * which is where the K1's CheckKeys() lives and therefore the only place a key
 * does anything.  The preview has to run all three or it is only pressing keys
 * at a radio that is not listening. */
static void step(void)
{
    host_systick_advance(10u);
    APP_Update();
    tx_poll_ptt();
    APP_TimeSlice10ms();
    /* The radio's SysTick handler runs the K1's countdowns every 10 ms; the
     * scan step depends on them (see driver/scheduler.c). */
    scheduler_tick_10ms();
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

    /* ---- the image: the import must have given the K1 real channels -------
     *
     * The radio boots onto the codeplug's first channel, and the runtime now
     * reads the records the one-time import wrote (0x9000).  If the image is
     * empty, RADIO_ConfigureChannel falls back to gEeprom.FreqChannel (a
     * frequency channel, band default frequency) and COMMON_SwitchVFOMode then
     * has nothing to switch to -- the "stuck on F3, no new frequency" fault.
     * This check is why the host models the image region (tools/host/host_hw.c). */
    {
        bool ok = IS_MR_CHANNEL(gEeprom.ScreenChannel[0]) &&
                  gEeprom.ScreenChannel[0] == 0u;

        printf("\n[image] %s boot lands on %s channel %u, RX %u.%05u MHz "
               "(the import's record, read back through the K1 runtime)\n",
               ok ? "ok  " : "FAIL",
               IS_MR_CHANNEL(gEeprom.ScreenChannel[0]) ? "memory" : "frequency",
               (unsigned)gEeprom.ScreenChannel[0],
               (unsigned)(gRxVfo->freq_config_RX.Frequency / 100000u),
               (unsigned)(gRxVfo->freq_config_RX.Frequency % 100000u));
        if (!ok) failures++;
    }

    /* ---- the codeplug: what the stock's records decode to ---------------- */
    {
        unsigned int channel;

        printf("\n[codeplug] the host's synthetic stock image\n");
        printf("  screen A = %u, screen B = %u (the first two channels it has)\n",
               (unsigned)gEeprom.ScreenChannel[0], (unsigned)gEeprom.ScreenChannel[1]);

        for (channel = 0; channel < 8u; channel++) {
            ChannelScanDisplayInfo_t info;
            char name[16];

            if (!codeplug_scan_info(channel, &info)) {
                printf("  CH %2u  -- not used\n", channel);
                continue;
            }

            codeplug_name(name, sizeof name, channel);
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
                   (unsigned)codeplug_excluded(channel));
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

    /* SysInf, as the menu draws it: page 0 is the identity (the port now shows
     * the pack there too) and page 2 is the dedicated BATTERY page.  A plausible
     * pack so the lines are visible. */
    gBatteryVoltageAverage = 828;
    gBatteryDisplayLevel   = 6;
    gIsInSubMenu           = true;
    gMenuCursor            = UI_MENU_GetViewPos(MENU_VOL);
    gSubMenuSelection      = 0;
    UI_DisplayMenu();
    render("SysInf page 0 (identity + battery)");
    gSubMenuSelection      = 2;
    UI_DisplayMenu();
    render("SysInf page 2 (BATTERY)");
    gIsInSubMenu           = false;

    /* One item per VFO, each setting its own VFO explicitly. */
    {
        bool found_a = false, found_b = false;
        unsigned mi;
        bool labels_ok = strcmp(gSubMenu_RF[0], "4829") == 0 &&
                         strcmp(gSubMenu_RF[1], "4815") == 0;
        for (mi = 0; MenuList[mi].name[0] != '\0'; mi++) {
            if (MenuList[mi].menu_id == MENU_RF_A) found_a = true;
            if (MenuList[mi].menu_id == MENU_RF_B) found_b = true;
        }
        printf("[dual] %s TrVfoA and TrVfoB menu items exist\n",
               (found_a && found_b) ? "ok  " : "FAIL");
        if (!(found_a && found_b)) failures++;
        printf("[dual] %s transceiver menu offers only 4829 and 4815\n",
               labels_ok ? "ok  " : "FAIL");
        if (!labels_ok) failures++;
    }
    gIsInSubMenu = true;
    gMenuCursor  = UI_MENU_GetViewPos(MENU_RF_A);
    gSubMenuSelection = 0;              /* BK4829 */
    MENU_AcceptSetting();
    gMenuCursor  = UI_MENU_GetViewPos(MENU_RF_B);
    gSubMenuSelection = 1;              /* BK4815 */
    MENU_AcceptSetting();
    {
        bool ok = SETTINGS_GetVfoTransceiver(0u) == RF_XCVR_BK4829 &&
                  SETTINGS_GetVfoTransceiver(1u) == RF_XCVR_BK4815;
        printf("[dual] %s RF A=4829 RF B=4815 (A=%u B=%u)\n",
               ok ? "ok  " : "FAIL", (unsigned)SETTINGS_GetVfoTransceiver(0u),
               (unsigned)SETTINGS_GetVfoTransceiver(1u));
        if (!ok) failures++;
    }
    gIsInSubMenu = false;
    {
        bool ok;

        /* An older settings blob stored AUTO as 0 in the reserved bytes;
         * with Auto removed, that legacy value must default to BK4829. */
        SETTINGS_SetVfoTransceiver(0u, (rf_xcvr_t)0);
        ok = SETTINGS_GetVfoTransceiver(0u) == RF_XCVR_BK4829;
        printf("[dual] %s legacy/invalid RF value 0 resolves to BK4829\n",
               ok ? "ok  " : "FAIL");
        if (!ok) failures++;
    }

    /* SetScn is the single scan mode: NORMAL / FAST / FAST BOTH.  The row must
     * offer exactly those labels, map the menu index to the mode, re-open
     * showing the value, and persist. */
    {
        bool found = false;
        unsigned mi;
        uint8_t id = 0;
        const uint8_t before = gSetting_set_scn;

        for (mi = 0; MenuList[mi].name[0] != '\0'; mi++) {
            if (strcmp(MenuList[mi].name, "SetScn") == 0) {
                found = true;
                id = MenuList[mi].menu_id;
            }
        }
        printf("[scan] %s the SetScn menu row exists\n", found ? "ok  " : "FAIL");
        if (!found) failures++;

        if (found) {
            const bool labels_ok =
                strcmp(gSubMenu_SET_SCN[0], "NORMAL") == 0 &&
                strcmp(gSubMenu_SET_SCN[1], "FAST") == 0 &&
                strcmp(gSubMenu_SET_SCN[2], "FAST BOTH") == 0;
            printf("[scan] %s SetScn offers NORMAL, FAST and FAST BOTH\n",
                   labels_ok ? "ok  " : "FAIL");
            if (!labels_ok) failures++;

            gIsInSubMenu = true;
            gMenuCursor  = UI_MENU_GetViewPos(id);

            {
                const uint8_t want[3] = { SCAN_MODE_NORMAL, SCAN_MODE_FAST,
                                          SCAN_MODE_FAST_BOTH };
                unsigned k;
                bool ok = true;

                for (k = 0; k < 3u; k++) {
                    gSubMenuSelection = (uint8_t)k;
                    MENU_AcceptSetting();
                    if (gSetting_set_scn != want[k])
                        ok = false;
                }
                printf("[scan] %s SetScn index 0/1/2 selects NORMAL/FAST/FAST BOTH\n",
                       ok ? "ok  " : "FAIL");
                if (!ok) failures++;
            }

            gSubMenuSelection = SCAN_MODE_FAST_BOTH;
            MENU_ShowCurrentSetting();
            {
                const bool ok = gSubMenuSelection == SCAN_MODE_FAST_BOTH;
                printf("[scan] %s reopening SetScn shows its mode\n", ok ? "ok  " : "FAIL");
                if (!ok) failures++;
            }

            gSetting_set_scn = before;
            gIsInSubMenu = false;
        }
    }

    {
        const unsigned int vfo = gEeprom.RX_VFO;
        unsigned i;
        bool moved = false;
        uint32_t f0;

        gEeprom.ScreenChannel[vfo] = FREQ_CHANNEL_FIRST;
        gEeprom.FreqChannel[vfo]   = FREQ_CHANNEL_FIRST;
        gRxVfo->freq_config_RX.Frequency = 14550000u;
        gRxVfo->StepFrequency            = 2500u;   /* 25 kHz */
        gScanStateDir                    = SCAN_OFF;

        ACTION_Scan(false);                          /* start */
        f0 = gRxVfo->freq_config_RX.Frequency;
        for (i = 0; i < 300u && !moved; i++) {
            step();
            moved = (gRxVfo->freq_config_RX.Frequency != f0);
        }
        printf("[scan] %s the scan steps the receive frequency (%u -> %u)\n",
               moved ? "ok  " : "FAIL", (unsigned)f0,
               (unsigned)gRxVfo->freq_config_RX.Frequency);
        if (!moved) failures++;

        {
            scan_dual_stats_t stats;
            bool ok;

            (void)CHFRSCANNER_GetScanDualStats(&stats);
            ok = stats.rate.candidates > 0u && stats.rate.elapsed_ms >= 10u &&
                 stats.rate.candidates_per_second > 0u;
            printf("[scan] %s scan-rate diagnostic counts probes over elapsed time "
                   "(n=%u elapsed=%u ms rate=%u/s)\n",
                   ok ? "ok  " : "FAIL", (unsigned)stats.rate.candidates,
                   (unsigned)stats.rate.elapsed_ms,
                   (unsigned)stats.rate.candidates_per_second);
            if (!ok) failures++;
        }

        if (gScanStateDir != SCAN_OFF)
            ACTION_Scan(false);                      /* stop */
    }

    /* The dual-scan benchmark must arm one shared range, run a mode, report a
     * probe count over elapsed time, and restore the saved scan mode. */
    {
        const uint8_t before = gSetting_set_scn;
        const uint32_t saved_start = gScanRangeStart;
        scan_bench_result_t res;
        unsigned i;
        bool ok;

        gSetting_set_scn = SCAN_MODE_FAST;
        CHFRSCANNER_BenchmarkArm(14500000u, 14600000u);
        CHFRSCANNER_BenchmarkRun(1u);          /* FAST BOTH */
        for (i = 0; i < 30u; i++)
            step();
        CHFRSCANNER_BenchmarkFinish(&res);
        CHFRSCANNER_BenchmarkDisarm();

        ok = res.probes > 0u && res.elapsed_ms > 0u && res.probes_per_second > 0u;
        printf("[scan] %s benchmark reports probes over time (n=%u elapsed=%u ms rate=%u/s)\n",
               ok ? "ok  " : "FAIL", (unsigned)res.probes,
               (unsigned)res.elapsed_ms, (unsigned)res.probes_per_second);
        if (!ok) failures++;

        ok = gSetting_set_scn == before;
        printf("[scan] %s benchmark restores the saved scan mode\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        ok = gScanRangeStart == saved_start;
        printf("[scan] %s benchmark restores the scan range\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;
    }

    /* A found signal must resume the scan, not end it.  SCAN_RESUME_MODE == 0 is
     * the K1's "stop scanning" mode (app/app.c's end-of-RX path calls
     * CHFRSCANNER_Stop()); the port's default must not be it, or the first
     * carrier ends the scan.  The K1's own fallback is 14. */
    {
        const bool ok = gEeprom.SCAN_RESUME_MODE != 0;

        printf("[scan] %s the default ScnRev resumes, not stops (SCAN_RESUME_MODE=%u)\n",
               ok ? "ok  " : "FAIL", (unsigned)gEeprom.SCAN_RESUME_MODE);
        if (!ok) failures++;
    }

    /* The dual-lane candidate source: it must preserve the K1 order and tag
     * every item with an increasing ordinal, so the two lanes can interleave
     * without dropping or duplicating a step. */
    {
        const unsigned vfo = gEeprom.RX_VFO;
        scan_candidate_t c;
        bool range_ok = true;
        bool list_ok  = true;
        unsigned i;

        /* Frequency range: 145.0000 .. 145.1000 MHz in 25 kHz steps, forward.
         * Start one step below so the first candidate is 145.0000. */
        gEeprom.ScreenChannel[vfo] = FREQ_CHANNEL_FIRST;
        gEeprom.FreqChannel[vfo]   = FREQ_CHANNEL_FIRST;
        gRxVfo->freq_config_RX.Frequency = 14497500u;
        gRxVfo->StepFrequency            = 2500u;
        gScanRangeStart = 14500000u;
        gScanRangeStop  = 14510000u;
        gScanStateDir   = SCAN_FWD;

        {
            uint32_t prev     = 0;
            uint32_t ordinal0 = 0;
            unsigned n        = 0;

            for (i = 0; i < 5u; i++) {
                if (!CHFRSCANNER_NextCandidate(&c)) { range_ok = false; break; }
                if (c.is_memory_channel)            { range_ok = false; break; }
                if (i == 0u) {
                    ordinal0 = c.ordinal;
                    if (c.frequency_10hz != 14500000u) range_ok = false;
                } else {
                    if (c.frequency_10hz != prev + 2500u) range_ok = false;
                    if (c.ordinal != ordinal0 + i)        range_ok = false;
                }
                if (scan_dual_assign_candidate(&c) !=
                    ((c.ordinal & 1u) ? SCAN_LANE_BK4815 : SCAN_LANE_BK4829))
                    range_ok = false;
                prev = c.frequency_10hz;
                n++;
            }
            if (n != 5u) range_ok = false;   /* five in-range steps */

            /* The next step wraps back to the start. */
            if (!CHFRSCANNER_NextCandidate(&c) || c.frequency_10hz != 14500000u)
                range_ok = false;
        }

        /* Memory list: drive the same cursor in channel mode and check that
         * each eligible channel is visited exactly once per pass, in K1 order,
         * with the below-split channel staying on the BK4829. */
        gScanRangeStart = 0;
        gEeprom.SCAN_LIST_DEFAULT = MR_CHANNELS_LIST + 1;   /* ALL */
        gEeprom.SCAN_LIST_ENABLED = false;
        gNextMrChannel = 0;
        gScanStateDir  = SCAN_FWD;

        {
            uint16_t seen[8];
            unsigned n        = 0;
            uint32_t ordinal0 = 0;
            bool     wrapped  = false;

            for (i = 0; i < 8u && !wrapped; i++) {
                unsigned j;

                if (!CHFRSCANNER_NextCandidate(&c)) { list_ok = false; break; }
                if (!c.is_memory_channel)           { list_ok = false; break; }

                for (j = 0; j < n; j++) {
                    if (seen[j] == c.channel) { wrapped = true; break; }
                }
                if (wrapped)
                    break;

                if (i == 0u)
                    ordinal0 = c.ordinal;
                else if (c.ordinal != ordinal0 + i)
                    list_ok = false;

                if (scan_dual_assign_candidate(&c) !=
                    (c.frequency_10hz > SCAN_DUAL_BK4815_MIN_FREQUENCY_10HZ
                         ? ((c.ordinal & 1u) ? SCAN_LANE_BK4815 : SCAN_LANE_BK4829)
                         : SCAN_LANE_BK4829))
                    list_ok = false;

                if (n < 8u)
                    seen[n] = c.channel;
                n++;
            }

            if (n != 5u) list_ok = false;   /* the fixture's five channels, once each */
        }

        printf("[scan] %s the candidate source keeps K1 range order and ordinals\n",
               range_ok ? "ok  " : "FAIL");
        if (!range_ok) failures++;
        printf("[scan] %s the candidate source walks each list channel once\n",
               list_ok ? "ok  " : "FAIL");
        if (!list_ok) failures++;

        gScanRangeStart = 0;
        gScanStateDir   = SCAN_OFF;
    }

    /* A paused dual-scan hit pins the receive route to the chip that found it;
     * stopping the scan must restore the saved per-VFO route. */
    {
        rx_set_scan_source_override(RX_SCAN_SOURCE_BK4815);
        CHFRSCANNER_Stop();
        {
            const bool ok = rx_scan_source_override() == RX_SCAN_SOURCE_DEFAULT;
            printf("[scan] %s scan stop clears the RX scan-source override\n",
                   ok ? "ok  " : "FAIL");
            if (!ok) failures++;
        }
    }

    /* Resuming after a hit is also a fresh pass: the override must not leak
     * into the next sweep. */
    {
        rx_set_scan_source_override(RX_SCAN_SOURCE_BK4829);
        CHFRSCANNER_ContinueScanning();
        {
            const bool ok = rx_scan_source_override() == RX_SCAN_SOURCE_DEFAULT;
            printf("[scan] %s scan resume clears the RX scan-source override\n",
                   ok ? "ok  " : "FAIL");
            if (!ok) failures++;
        }
    }

    /* The status bar with a charged pack: the icon must show bars, and (with
     * gSetting_battery_text = 2) the percentage beside it. */
    UI_DisplayStatus();
    render("status bar: battery icon, level 6");

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
        saved = storage_save_settings();

        gEeprom.SQUELCH_LEVEL = 0;
        gEeprom.CHANNEL_DISPLAY_MODE = 0;
        loaded = storage_load_settings();

        printf("\n[storage] save=%d load=%d -> squelch=%u chdisp=%u (expected 7/2)\n",
               saved, loaded, (unsigned)gEeprom.SQUELCH_LEVEL,
               (unsigned)gEeprom.CHANNEL_DISPLAY_MODE);
        printf("[storage] write test=%d\n", storage_write_test(&bad));

        /* The per-VFO RF transceiver choice is part of the port's extra blob,
         * so it needs the full save path (SETTINGS_SaveSettings), not the bare
         * gEeprom save, and a reload to read it back. */
        {
            bool ok;

            SETTINGS_SetVfoTransceiver(0u, RF_XCVR_BK4829);
            SETTINGS_SetVfoTransceiver(1u, RF_XCVR_BK4815);
            SETTINGS_SaveSettings();
            SETTINGS_SetVfoTransceiver(0u, RF_XCVR_BK4829);
            SETTINGS_SetVfoTransceiver(1u, RF_XCVR_BK4829);
            SETTINGS_InitEEPROM();
            ok = SETTINGS_GetVfoTransceiver(0u) == RF_XCVR_BK4829 &&
                 SETTINGS_GetVfoTransceiver(1u) == RF_XCVR_BK4815;
            printf("[dual] %s per-VFO transceiver survives save/load (A=%u B=%u)\n",
                   ok ? "ok  " : "FAIL", (unsigned)SETTINGS_GetVfoTransceiver(0u),
                   (unsigned)SETTINGS_GetVfoTransceiver(1u));
            if (!ok) failures++;
        }

        /* The scan mode (the SetScn menu) lives in the same extra blob. */
        {
            bool ok;

            gSetting_set_scn = SCAN_MODE_FAST_BOTH;
            SETTINGS_SaveSettings();
            gSetting_set_scn = SCAN_MODE_NORMAL;
            SETTINGS_InitEEPROM();
            ok = gSetting_set_scn == SCAN_MODE_FAST_BOTH;
            printf("[scan] %s SetScn FAST BOTH survives save/load\n", ok ? "ok  " : "FAIL");
            if (!ok) failures++;
        }

        /* An older (version 2) extra blob has no scan byte: its fields must
         * survive and the scan mode must fall back to FAST.  Build one from a
         * real save by truncating the current payload to the v2 prefix and
         * stamping the old version. */
        {
            uint8_t v2[STORAGE_EXTRA_MAX];
            uint8_t snap_before[7 * 2 * 16];
            uint8_t snap_after[7 * 2 * 16];
            const uint32_t v2_size = 4u + 2u + 2u + (7u * 2u * 16u)
#ifdef ENABLE_FMRADIO
                                     + (FM_CHANNELS_MAX * 2u)
#endif
                                     ;
            bool ok = true;

            SETTINGS_SetVfoTransceiver(0u, RF_XCVR_BK4829);
            SETTINGS_SetVfoTransceiver(1u, RF_XCVR_BK4815);
            gSetting_set_scn = SCAN_MODE_FAST_BOTH;
#ifdef ENABLE_FMRADIO
            gFM_Channels[0] = 0x1234u;
            gFM_Channels[1] = 0x4321u;
#endif
            SETTINGS_SaveSettings();
            codeplug_freq_snapshot(snap_before, sizeof snap_before);

            memset(v2, 0, sizeof v2);
            if (!storage_get_extra(v2, v2_size))
                ok = false;
            v2[4] = 2u;   /* version, little-endian */
            v2[5] = 0u;
            if (!storage_set_extra(v2, v2_size))
                ok = false;
            if (!storage_save_settings())
                ok = false;

            /* Clear the in-RAM frequency snapshot so a failed migration shows,
             * then load the old blob. */
            {
                uint8_t zero[7 * 2 * 16];
                memset(zero, 0, sizeof zero);
                codeplug_freq_restore(zero, sizeof zero);
            }
            SETTINGS_InitEEPROM();
            codeplug_freq_snapshot(snap_after, sizeof snap_after);

            ok = ok &&
                 gSetting_set_scn == SCAN_MODE_FAST &&
                 SETTINGS_GetVfoTransceiver(0u) == RF_XCVR_BK4829 &&
                 SETTINGS_GetVfoTransceiver(1u) == RF_XCVR_BK4815 &&
                 memcmp(snap_before, snap_after, sizeof snap_before) == 0;
#ifdef ENABLE_FMRADIO
            ok = ok && gFM_Channels[0] == 0x1234u && gFM_Channels[1] == 0x4321u;
#endif
            printf("[scan] %s a v2 extra blob migrates (fields kept, scan mode FAST)\n",
                   ok ? "ok  " : "FAIL");
            if (!ok) failures++;
        }

        /* A version-3 blob carried the old ScTrMd byte (0 Default, 1 Both) in
         * the same slot; version 3 must map it to FAST / FAST BOTH. */
        {
            uint8_t v3[STORAGE_EXTRA_MAX];
            const uint32_t v2_size = 4u + 2u + 2u + (7u * 2u * 16u)
#ifdef ENABLE_FMRADIO
                                     + (FM_CHANNELS_MAX * 2u)
#endif
                                     ;
            const uint32_t v4_size = v2_size + 4u;   /* the mode byte + padding */
            bool ok = true;

            gSetting_set_scn = SCAN_MODE_NORMAL;
            SETTINGS_SaveSettings();

            memset(v3, 0, sizeof v3);
            if (!storage_get_extra(v3, v4_size))
                ok = false;
            v3[4] = 3u;          /* version 3 */
            v3[5] = 0u;
            v3[v2_size] = 1u;    /* old ScTrMd = Both */
            if (!storage_set_extra(v3, v4_size))
                ok = false;
            if (!storage_save_settings())
                ok = false;

            gSetting_set_scn = SCAN_MODE_NORMAL;
            SETTINGS_InitEEPROM();
            ok = ok && gSetting_set_scn == SCAN_MODE_FAST_BOTH;
            printf("[scan] %s a v3 extra blob maps old ScTrMd Both to FAST BOTH\n",
                   ok ? "ok  " : "FAIL");
            if (!ok) failures++;
        }

        /* Put the state back so the screens below render as before (a plain
         * SettingsDefaults() would clear the VFO pointers those screens
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

    /* ---- a frequency entered in a VFO must stick -------------------------
     *
     * Typing a frequency in a frequency channel makes the K1 set the VFO's
     * frequency, request a channel save and then reconfigure.  The reconfigure
     * reloads a frequency channel through MR_GetChannelAttributes(); if the
     * channel has no attribute entry (0xFFFF) RADIO_ConfigureChannel treats it
     * as invalid and resets it to the band's lower frequency -- which is what
     * made a typed frequency snap back to 137.000.  SETTINGS_SaveChannel now
     * writes the attributes, so the entered value must survive. */
    {
        const uint16_t freqChannel = (uint16_t)(FREQ_CHANNEL_FIRST + BAND3_137MHz);
        VFO_Info_t *vfo = &gEeprom.VfoInfo[0];
        uint32_t kept;
        bool ok;

        gEeprom.TX_VFO = 0;
        gEeprom.RX_VFO = 0;
        RADIO_SelectVfos();
        SETTINGS_FixupVfoPointers();

        gEeprom.ScreenChannel[0] = freqChannel;
        gEeprom.FreqChannel[0]   = freqChannel;
        RADIO_ConfigureChannel(0, VFO_CONFIGURE_RELOAD);

        vfo->freq_config_RX.Frequency = 14550000u;
        SETTINGS_SaveChannel(freqChannel, 0, vfo, 1);
        RADIO_ConfigureChannel(0, VFO_CONFIGURE);
        kept = vfo->freq_config_RX.Frequency;

        ok = (kept == 14550000u);
        printf("\n[freq] %s an entered frequency survives the reconfigure "
               "(145.5000 -> %u.%05u MHz)\n", ok ? "ok  " : "FAIL",
               (unsigned)(kept / 100000u), (unsigned)(kept % 100000u));
        if (!ok) failures++;
    }

    /* ---- a beep must drive the amplifier enable -------------------------
     *
     * The DAC tone reaches the speaker only through the amp (PC13), and the
     * idle receive path leaves it off (RADIO_SetupRegisters -> AudioPathOff).
     * AUDIO_PlayBeep therefore has to turn it on for the beep, the way the K1
     * did around the chip's tone.  Regression: a rewrite that dropped the path
     * handling made every key beep silent -- only the boot sound, played while
     * the path was still on, was audible. */
    {
        bool ok;

        gCurrentFunction     = FUNCTION_FOREGROUND;
        gEeprom.BEEP_CONTROL = true;
        gEnableSpeaker       = false;
        GPIO_DisableAudioPath();
        host_beeper_reset_counts();

        AUDIO_PlayBeep(BEEP_1KHZ_60MS_OPTIONAL);

        ok = host_beeper_play_count() > 0u &&
             host_beeper_plays_path_on() == host_beeper_play_count();

        printf("\n[beep] %s the beep drives the amp on (%u/%u plays with the path on)\n",
               ok ? "ok  " : "FAIL",
               host_beeper_plays_path_on(), host_beeper_play_count());
        if (!ok) failures++;
    }

    /* ---- entering FM must raise the amp and mute the RF chip's AF -------
     *
     * The stock's FM-on (`FUN_0800D35C`) raises PC13 and mutes the BK4829's
     * AF (`FUN_08009CC4` writes `0x47 = 0x6042`, `REG_47<11:8> = 0` = Mute)
     * because the BK4829's `EARO` and the BK1080's `LOUT`/`ROUT` share the
     * amplifier input.  The imported K1 code raised the path but left the RF
     * chip at Normal AF (`0x6142`), which is what made the first radio run
     * silent.  See docs/ra89r_bk1080.md, "The FM audio path". */
    {
        bool ok;

        gEeprom.FM_FrequencyPlaying = 1000u;   /* 100.0 MHz, K1 units */
        gEeprom.FM_Band             = 1u;
        gCurrentFunction            = FUNCTION_FOREGROUND;
        GPIO_DisableAudioPath();
        gEnableSpeaker              = false;

        FM_Start();

        ok = host_audio_path_is_on() &&
             host_bk4819_last_af() == (int)BK4819_AF_MUTE;

        printf("\n[fm] %s FM raises the amp and mutes the RF chip's AF "
               "(path %s, last AF %d)\n",
               ok ? "ok  " : "FAIL",
               host_audio_path_is_on() ? "on" : "off",
               host_bk4819_last_af());
        if (!ok) failures++;

        FM_TurnOff();

        ok = !host_audio_path_is_on();

        printf("\n[fm] %s FM off lowers the amp (path %s)\n",
               ok ? "ok  " : "FAIL",
               host_audio_path_is_on() ? "on" : "off");
        if (!ok) failures++;
    }

    printf("\n%d failures\n", failures);
    return failures ? 1 : 0;
}
