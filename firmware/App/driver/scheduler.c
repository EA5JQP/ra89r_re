/* The K1's periodic countdowns (App/scheduler.c), on this board (see NOTICE).
 *
 * The K1 runs its countdowns inside SysTick_Handler.  This port's SysTick
 * handler only counted milliseconds, so none of them ran.  The most visible
 * consequence is the scan: long-press `*` sets `gScanStateDir` (the "S" shows)
 * but the scan never steps, because `gScanPauseDelayIn_10ms` is what re-arms
 * `gScheduleScanListen` and nothing decremented it.  The same applies to the
 * CTCSS/CDCSS "found" countdowns, the tail-note elimination, the power save and
 * the FM countdowns.
 *
 * The body is the K1's, in its order.  The 500 ms group runs every fiftieth
 * 10 ms tick, as it does there.
 */
#include "driver/scheduler.h"

#include "app/chFrScanner.h"
#include "app/scanner.h"
#include "audio.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "settings.h"

#ifdef ENABLE_FMRADIO_EMBEDDED
    #include "app/fm.h"
#endif

#define DECREMENT(cnt) \
    do {               \
        if (cnt > 0)   \
            cnt--;     \
    } while (0)

#define DECREMENT_AND_TRIGGER(cnt, flag) \
    do {                                 \
        if (cnt > 0)                     \
            if (--cnt == 0)              \
                flag = true;             \
    } while (0)

void scheduler_tick_10ms(void)
{
    static uint8_t sub;

    /* ---- the K1's 500 ms group (every 50th 10 ms tick) ------------------- */
    if (++sub >= 50u) {
        sub = 0;

#ifdef ENABLE_FEAT_F4HWN
        DECREMENT_AND_TRIGGER(gVfoSaveCountdown_10ms, gScheduleVfoSave);
        DECREMENT_AND_TRIGGER(gTxTimerCountdownAlert_500ms - ALERT_TOT * 2,
                              gTxTimeoutReachedAlert);
#endif
        DECREMENT_AND_TRIGGER(gTxTimerCountdown_500ms, gTxTimeoutReached);
        DECREMENT(gSerialConfigCountDown_500ms);
    }

    /* ---- the 10 ms group ------------------------------------------------- */
    DECREMENT(gFoundCDCSSCountdown_10ms);
    DECREMENT(gFoundCTCSSCountdown_10ms);

    if (gCurrentFunction == FUNCTION_FOREGROUND)
        DECREMENT_AND_TRIGGER(gBatterySaveCountdown_10ms, gSchedulePowerSave);

    if (gCurrentFunction == FUNCTION_POWER_SAVE)
        DECREMENT_AND_TRIGGER(gPowerSave_10ms, gPowerSaveCountdownExpired);

    if (gScanStateDir == SCAN_OFF && !gCssBackgroundScan &&
        gEeprom.DUAL_WATCH != DUAL_WATCH_OFF)
        if (gCurrentFunction != FUNCTION_MONITOR &&
            gCurrentFunction != FUNCTION_TRANSMIT &&
            gCurrentFunction != FUNCTION_RECEIVE)
            DECREMENT_AND_TRIGGER(gDualWatchCountdown_10ms, gScheduleDualWatch);

    if (gScanStateDir != SCAN_OFF)
        if (gCurrentFunction != FUNCTION_MONITOR &&
            gCurrentFunction != FUNCTION_TRANSMIT)
            DECREMENT_AND_TRIGGER(gScanPauseDelayIn_10ms, gScheduleScanListen);

#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
    /* Scan stall watchdog: if the resume countdown has expired but no resume
     * was scheduled and nothing is being received, force one.  The
     * `gScanPauseDelayIn_10ms == 0` guard keeps it from overriding the
     * user's ScnRev pause while that is still counting down. */
    #define SCAN_FAST_STALL_WATCHDOG_10ms 25u   /* 250 ms */
    {
        static uint16_t stall;

        if (gSetting_set_scn && gScanStateDir != SCAN_OFF &&
            gScanPauseDelayIn_10ms == 0 && !gScheduleScanListen &&
            !g_SquelchLost &&
            gCurrentFunction != FUNCTION_RECEIVE &&
            gCurrentFunction != FUNCTION_TRANSMIT &&
            gCurrentFunction != FUNCTION_MONITOR)
        {
            if (++stall >= SCAN_FAST_STALL_WATCHDOG_10ms) {
                stall = 0;
                gScheduleScanListen = true;
            }
        } else {
            stall = 0;
        }
    }
#endif

    DECREMENT_AND_TRIGGER(gTailNoteEliminationCountdown_10ms,
                          gFlagTailNoteEliminationComplete);

#ifdef ENABLE_VOX
    DECREMENT(gVoxStopCountdown_10ms);
#endif

#ifdef ENABLE_FMRADIO_EMBEDDED
    if (gFM_ScanState != FM_SCAN_OFF && gCurrentFunction != FUNCTION_MONITOR)
        if (gCurrentFunction != FUNCTION_TRANSMIT &&
            gCurrentFunction != FUNCTION_RECEIVE)
            DECREMENT_AND_TRIGGER(gFmPlayCountdown_10ms, gScheduleFM);
#endif

    DECREMENT(boot_counter_10ms);
}
