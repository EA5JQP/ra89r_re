
#include <stddef.h>
#include <string.h>

#include "app/app.h"
#include "app/chFrScanner.h"
#include "app/scan_dual.h"
#include "audio.h"
#include "driver/bk4815.h"
#include "driver/pa.h"
#include "driver/rx.h"
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
#include "driver/systick.h"
#endif
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "ui/main.h"
//#include "debugging.h"

int8_t            gScanStateDir;
bool              gScanKeepResult;
bool              gScanPauseMode;

#ifdef ENABLE_SCAN_RANGES
uint32_t          gScanRangeStart;
uint32_t          gScanRangeStop;

#if defined(ENABLE_FEAT_F4HWN_SCAN_SUBAUDIBLE) && ENABLE_FEAT_F4HWN_SCAN_SUBAUDIBLE
DCS_CodeType_t    gScanRangeCssType  = CODE_TYPE_OFF;
uint8_t           gScanRangeCssCode  = 0xFF;
static uint8_t    scanRangeCssCandidate = 0xFF;
static uint8_t    scanRangeCssHitCount  = 0;
#endif

#define SCAN_RANGE_SKIP_MAX 64
#if (SCAN_RANGE_SKIP_MAX & (SCAN_RANGE_SKIP_MAX - 1)) != 0
    #error SCAN_RANGE_SKIP_MAX must be a power of two
#endif

typedef struct {
    uint16_t sample[SCAN_RANGE_SKIP_MAX];
    uint32_t start;
    uint32_t stop;
    uint16_t step;
    uint8_t  count;
    uint8_t  next;
} ScanRangeSkipList_t;

static ScanRangeSkipList_t scanRangeSkip;
#endif

typedef enum {
    SCAN_NEXT_CHAN_SCANLIST1 = 0,
    SCAN_NEXT_CHAN_SCANLIST2,
    SCAN_NEXT_CHAN_DUAL_WATCH,
    SCAN_NEXT_CHAN_MR,
    SCAN_NEXT_NUM
} scan_next_chan_t;

scan_next_chan_t    currentScanList;
uint32_t            initialFrqOrChan;
uint8_t             initialCROSS_BAND_RX_TX;

/* The ordinal the shared candidate cursor hands out (CHFRSCANNER_NextCandidate
 * below).  Reset when a scan starts. */
static uint32_t scanCandidateOrdinal;

#ifndef ENABLE_FEAT_F4HWN
    uint32_t lastFoundFrqOrChan;
#else
    uint32_t lastFoundFrqOrChan;
    uint32_t lastFoundFrqOrChanOld;
#endif

static void NextFreqChannel(void);
static void NextMemChannel(void);
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
static void ScanFastResetState(void);
static void ScanBothReset(void);
#endif

#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
static bool ScanFastEnabled(void)
{
    return gSetting_set_scn != (uint8_t)SCAN_MODE_NORMAL;
}

/* Listen-while-scanning: the selected VFO keeps its chip while the other one
 * sweeps (ScanRangeFastPrecheckListen). */
static bool ScanListenEnabled(void)
{
    return gSetting_set_scn == (uint8_t)SCAN_MODE_LISTEN;
}
#endif

#ifdef ENABLE_SCAN_RANGES
static void ScanRangeSkipClear(void)
{
    scanRangeSkip.count = 0;
    scanRangeSkip.next = 0;
}

static void ScanRangeSkipSync(void)
{
    const uint16_t step = gRxVfo->StepFrequency;

    if (scanRangeSkip.start == gScanRangeStart &&
        scanRangeSkip.stop == gScanRangeStop &&
        scanRangeSkip.step == step)
    {
        return;
    }

    ScanRangeSkipClear();
    scanRangeSkip.start = gScanRangeStart;
    scanRangeSkip.stop  = gScanRangeStop;
    scanRangeSkip.step  = step;
}

static uint16_t ScanRangeSkipSampleForFrequency(uint32_t frequency)
{
    const uint16_t step = gRxVfo->StepFrequency;
    if (!gScanRangeStart || step == 0 || frequency < gScanRangeStart || frequency > gScanRangeStop)
        return 0;

    const uint32_t sample = (frequency - gScanRangeStart) / step;
    if (sample >= 0xFFFFu)
        return 0;

    return (uint16_t)(sample + 1);
}

static bool ScanRangeSkipContainsFrequency(uint32_t frequency)
{
    const uint16_t sample = ScanRangeSkipSampleForFrequency(frequency);
    if (sample == 0)
        return false;

    for (uint8_t i = 0; i < scanRangeSkip.count; ++i)
        if (scanRangeSkip.sample[i] == sample)
            return true;

    return false;
}

static uint32_t ScanRangeNextFrequency(void)
{
    uint32_t frequency;
    uint8_t guard = SCAN_RANGE_SKIP_MAX + 1;

    ScanRangeSkipSync();

    do
    {
        frequency = APP_SetFreqByStepAndLimits(gRxVfo, gScanStateDir, gScanRangeStart, gScanRangeStop);
        gRxVfo->freq_config_RX.Frequency = frequency;
    } while (--guard && ScanRangeSkipContainsFrequency(frequency));

    return frequency;
}

bool CHFRSCANNER_ExcludeCurrentScanRange(void)
{
    ScanRangeSkipSync();

    uint16_t sample = ScanRangeSkipSampleForFrequency(lastFoundFrqOrChan);
    if (sample == 0)
        sample = ScanRangeSkipSampleForFrequency(gRxVfo->freq_config_RX.Frequency);

    if (sample == 0)
        return false;

    for (uint8_t i = 0; i < scanRangeSkip.count; ++i)
        if (scanRangeSkip.sample[i] == sample)
            return true;

    scanRangeSkip.sample[scanRangeSkip.next] = sample;
    scanRangeSkip.next = (scanRangeSkip.next + 1) & (SCAN_RANGE_SKIP_MAX - 1);
    if (scanRangeSkip.count < SCAN_RANGE_SKIP_MAX)
        scanRangeSkip.count++;

#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
    ScanFastResetState();
#endif

    return true;
}

bool CHFRSCANNER_HasScanRangeExcludedOrdinal(uint32_t first_ordinal, uint32_t last_ordinal)
{
    if (first_ordinal == 0)
        first_ordinal = 1;
    if (last_ordinal < first_ordinal)
        return false;
    if (first_ordinal > 0xFFFFu)
        return false;
    if (last_ordinal > 0xFFFFu)
        last_ordinal = 0xFFFFu;

    for (uint8_t i = 0; i < scanRangeSkip.count; ++i) {
        const uint16_t sample = scanRangeSkip.sample[i];
        if (sample >= first_ordinal && sample <= last_ordinal)
            return true;
    }

    return false;
}

#if defined(ENABLE_FEAT_F4HWN_SCAN_SUBAUDIBLE) && ENABLE_FEAT_F4HWN_SCAN_SUBAUDIBLE
void CHFRSCANNER_UpdateCssDetection(void)
{
    if (!gScanRangeStart || !FUNCTION_IsRx())
    {
        gScanRangeCssType    = CODE_TYPE_OFF;
        gScanRangeCssCode    = 0xFF;
        scanRangeCssCandidate = 0xFF;
        scanRangeCssHitCount  = 0;
        return;
    }

    uint32_t cdcssFreq;
    uint16_t ctcssFreq;
    const BK4819_CssScanResult_t result = BK4819_GetCxCSSScanResult(&cdcssFreq, &ctcssFreq);

    if (result == BK4819_CSS_RESULT_CDCSS)
    {
        const uint8_t Code = DCS_GetCdcssCode(cdcssFreq);
        if (Code != 0xFF && Code != gScanRangeCssCode)
        {
            gScanRangeCssType    = CODE_TYPE_DIGITAL;
            gScanRangeCssCode    = Code;
            scanRangeCssCandidate = 0xFF;
            scanRangeCssHitCount  = 0;
            gUpdateDisplay = true;
        }
    }
    else if (result == BK4819_CSS_RESULT_CTCSS)
    {
        const uint8_t Code = DCS_GetCtcssCode(ctcssFreq);
        if (Code != 0xFF)
        {
            if (Code == scanRangeCssCandidate)
            {
                if (++scanRangeCssHitCount >= 2 && Code != gScanRangeCssCode)
                {
                    gScanRangeCssType = CODE_TYPE_CONTINUOUS_TONE;
                    gScanRangeCssCode = Code;
                    gUpdateDisplay    = true;
                }
            }
            else
            {
                scanRangeCssCandidate = Code;
                scanRangeCssHitCount  = 1;
            }
        }
    }
}
#endif
#endif

static void CHFRSCANNER_AbortActiveReception(void)
{
    if (!FUNCTION_IsRx())
        return;

    AUDIO_AudioPathOff();
    gEnableSpeaker   = false;
    gMonitor         = false;
    gRxReceptionMode = RX_MODE_NONE;
    gScanPauseMode   = false;

    FUNCTION_Init();
    FUNCTION_Select(FUNCTION_FOREGROUND);
}

#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
#define SCAN_FAST_PRECHECK_STEPS    6
#define SCAN_FAST_RSSI_MARGIN       16
#define SCAN_FAST_SQUELCH_MARGIN     8
#define SCAN_FAST_WEAK_MARGIN        8
/* The BK4815 reports a 7-bit RSSI (0x44), about a quarter of the BK4829's
 * 9-bit scale (0x67), so the dual lane uses its own, smaller margins rather
 * than comparing scales.  Radio 2026-10-21: the part's noise floor at 446 MHz
 * read 16-17 while the SQL-1 mark was 16, i.e. inside the noise, and the lane
 * stopped the scan on it every batch -- hence 8 (two marks' worth of margin)
 * after the seed-only first sample.  Still unvalidated as a *threshold*; the
 * floor-relative rule is what keeps it honest. */
#define SCAN_FAST4815_RSSI_MARGIN    8
#define SCAN_FAST4815_SQUELCH_MARGIN 4
#define SCAN_FAST4815_WEAK_MARGIN    4
/* The BK4815's 7-bit RSSI is scaled into the BK4829's 9-bit range (127*4 ~ 508)
 * when both lanes feed the one sparkline series. */
#define SCAN_FAST4815_RSSI_SCALE     4
#define SCAN_FAST_RECHECK_DELAY_US 350
#define SCAN_FAST_FINE_STEP_LIMIT   250
#define SCAN_FAST_FINE_REFINE_SPAN 1000
#define SCAN_FAST_FINE_REFINE_MAX    80
#define SCAN_FAST_FINE_RSSI_DROP      8
#define SCAN_FAST_RSSI_MAX          65535u
// Settle loop guard: at most this many 1us waits while the BK4819 glitch
// indicator stays above SCAN_FAST_GLITCH_THRESHOLD. Caps the worst-case
// settling time per step at ~50us before we read the RSSI anyway.
/* A noisy band (the airband) keeps the glitch indicator above the threshold, so
 * waiting up to SCAN_FAST_GLITCH_GUARD_MAX times costs ~15 ms per candidate
 * (each probe is a bit-banged RF-bus read) and makes a whole scan step ~90 ms --
 * which starves the UI and the keys.  A few samples settle it just as well. */
#define SCAN_FAST_GLITCH_GUARD_MAX  4
#define SCAN_FAST_GLITCH_THRESHOLD  200
// HF/VHF boundary in Hz: BK4819_PickRXFilterPathBasedOnFrequency() switches
// the front-end filter path here, so we only re-run that (relatively
// expensive) call when we actually cross the boundary.
#define SCAN_FAST_HF_VHF_BOUNDARY_HZ 28000000u

typedef enum {
    SCAN_FAST_DISABLED,
    SCAN_FAST_QUIET_BATCH,
    SCAN_FAST_CANDIDATE
} scan_fast_result_t;

static uint16_t scanFastReg30;
static uint16_t scanFastNoiseFloor   = SCAN_FAST_RSSI_MAX;
static uint32_t scanFastPrevFrequency;
static bool     scanFastLastFullTuneCandidate;
static VFO_Info_t scanFastDisplayVfo;
static bool       scanFastDisplayVfoValid;
static scan_candidate_rate_t scanCandidateRate;
#ifdef ENABLE_FEAT_F4HWN_SCAN_RSSI
static uint16_t scanRssiSparkline[CHFRSCANNER_RSSI_SPARKLINE_WIDTH];
static uint8_t  scanRssiSparklineWrite;
static uint8_t  scanRssiSparklineCount;

static void ScanRssiSparklineReset(void)
{
    memset(scanRssiSparkline, 0, sizeof(scanRssiSparkline));
    scanRssiSparklineWrite = 0;
    scanRssiSparklineCount = 0;
}

static void ScanRssiSparklinePush(uint16_t rssi)
{
    scanRssiSparkline[scanRssiSparklineWrite] = rssi;
    scanRssiSparklineWrite++;
    if (scanRssiSparklineWrite >= CHFRSCANNER_RSSI_SPARKLINE_WIDTH)
        scanRssiSparklineWrite = 0;

    if (scanRssiSparklineCount < CHFRSCANNER_RSSI_SPARKLINE_WIDTH)
        scanRssiSparklineCount++;
}

bool CHFRSCANNER_HasScanRssiSparkline(void)
{
    return gScanStateDir != SCAN_OFF && scanRssiSparklineCount > 1;
}

uint8_t CHFRSCANNER_GetScanRssiSparklineLevel(uint8_t index)
{
    uint16_t minRssi = SCAN_FAST_RSSI_MAX;
    uint16_t rssi = 0;
    uint8_t oldest;

    if (index >= CHFRSCANNER_RSSI_SPARKLINE_WIDTH || scanRssiSparklineCount == 0)
        return 0;

    if (index < CHFRSCANNER_RSSI_SPARKLINE_WIDTH - scanRssiSparklineCount)
        return 0;

    oldest = (uint8_t)((scanRssiSparklineWrite + CHFRSCANNER_RSSI_SPARKLINE_WIDTH - scanRssiSparklineCount)
                       % CHFRSCANNER_RSSI_SPARKLINE_WIDTH);
    index -= (uint8_t)(CHFRSCANNER_RSSI_SPARKLINE_WIDTH - scanRssiSparklineCount);

    for (uint8_t i = 0; i < scanRssiSparklineCount; i++)
    {
        const uint16_t sample = scanRssiSparkline[(oldest + i) % CHFRSCANNER_RSSI_SPARKLINE_WIDTH];
        if (sample != 0 && sample < minRssi)
            minRssi = sample;
    }

    rssi = scanRssiSparkline[(oldest + index) % CHFRSCANNER_RSSI_SPARKLINE_WIDTH];
    if (rssi == 0 || minRssi == SCAN_FAST_RSSI_MAX || rssi <= minRssi + 2)
        return 0;

    // 48 RSSI units ~= 24 dB. This keeps quiet jitter low while strong hits pop.
    return (uint8_t)MIN(((uint32_t)(rssi - minRssi) * 5u + 24u) / 48u, 5u);
}
#endif

static void ScanFastResetState(void)
{
    // Called on every scan (re)start, after a reception, and on each
    // wraparound to the start of the channel list / range. The noise
    // floor is re-warmed up from current conditions instead of carrying
    // stale calibration into a later sweep.
    scanFastNoiseFloor            = SCAN_FAST_RSSI_MAX;
    scanFastPrevFrequency         = 0;
    scanFastLastFullTuneCandidate = false;
    scanFastDisplayVfoValid       = false;
    ScanBothReset();
}

static void ScanFastResetNoiseFloor(void)
{
    scanFastNoiseFloor = SCAN_FAST_RSSI_MAX;
}

static uint16_t ScanFastReadRssi(void)
{
    uint8_t guard = SCAN_FAST_GLITCH_GUARD_MAX;
    while (guard-- && BK4819_GetGlitchIndicator() >= SCAN_FAST_GLITCH_THRESHOLD)
    {
        SYSTICK_DelayUs(1);
    }

    // Discard first read: after fast tuning the RSSI/AGC value may still be stale.
    BK4819_GetRSSI();
    return BK4819_GetRSSI();
}

static void ScanFastTune(uint32_t frequency)
{
    if (scanFastPrevFrequency == 0 ||
        ((frequency < SCAN_FAST_HF_VHF_BOUNDARY_HZ) !=
         (scanFastPrevFrequency < SCAN_FAST_HF_VHF_BOUNDARY_HZ)))
    {
        BK4819_PickRXFilterPathBasedOnFrequency(frequency);
    }

    scanFastPrevFrequency = frequency;
    BK4819_SetFrequency(frequency);
    BK4819_WriteRegister(BK4819_REG_30, 0);
    BK4819_WriteRegister(BK4819_REG_30, scanFastReg30);
}

const VFO_Info_t *CHFRSCANNER_GetScanDisplayVfo(void)
{
    if (!ScanFastEnabled() || !scanFastDisplayVfoValid || gScanStateDir == SCAN_OFF || FUNCTION_IsRx())
        return NULL;

    return &scanFastDisplayVfo;
}

static bool ScanFastUpdateDisplayVfo(uint16_t channel, uint32_t *frequency, ModulationMode_t *modulation)
{
    ChannelScanDisplayInfo_t info;

    if (!SETTINGS_FetchChannelScanDisplayInfo(gEeprom.RX_VFO, channel, &info))
    {
        scanFastDisplayVfoValid = false;
        return false;
    }

    scanFastDisplayVfo = gEeprom.VfoInfo[gEeprom.RX_VFO];

    scanFastDisplayVfo.CHANNEL_SAVE = channel;
    scanFastDisplayVfo.freq_config_RX = info.rx;
    scanFastDisplayVfo.freq_config_TX = info.tx;
    scanFastDisplayVfo.TX_OFFSET_FREQUENCY = info.offset;
    scanFastDisplayVfo.StepFrequency = info.stepFrequency;
    scanFastDisplayVfo.STEP_SETTING = info.stepSetting;
    scanFastDisplayVfo.Modulation = info.modulation;
    scanFastDisplayVfo.TX_OFFSET_FREQUENCY_DIRECTION = info.txOffsetFrequencyDirection;
    scanFastDisplayVfo.OUTPUT_POWER = info.outputPower;
    scanFastDisplayVfo.FrequencyReverse = info.frequencyReverse;
    scanFastDisplayVfo.CHANNEL_BANDWIDTH = info.channelBandwidth;
    scanFastDisplayVfo.BUSY_CHANNEL_LOCK = info.busyChannelLock;
    scanFastDisplayVfo.TX_LOCK = info.txLock;
#ifdef ENABLE_DTMF_CALLING
    scanFastDisplayVfo.DTMF_DECODING_ENABLE = info.dtmfDecodingEnable;
#endif
    scanFastDisplayVfo.DTMF_PTT_ID_TX_MODE = info.dtmfPttIdTxMode;

    if (!scanFastDisplayVfo.FrequencyReverse)
    {
        scanFastDisplayVfo.pRX = &scanFastDisplayVfo.freq_config_RX;
        scanFastDisplayVfo.pTX = &scanFastDisplayVfo.freq_config_TX;
    }
    else
    {
        scanFastDisplayVfo.pRX = &scanFastDisplayVfo.freq_config_TX;
        scanFastDisplayVfo.pTX = &scanFastDisplayVfo.freq_config_RX;
    }

    scanFastDisplayVfoValid = true;

    if (frequency)
        *frequency = info.rx.Frequency;
    if (modulation)
        *modulation = info.modulation;

    return true;
}

static uint16_t ScanFastSaturatingAdd(uint16_t value, uint16_t add)
{
    return (value > SCAN_FAST_RSSI_MAX - add) ? SCAN_FAST_RSSI_MAX : (uint16_t)(value + add);
}

static uint16_t ScanFastSaturatingSub(uint16_t value, uint16_t sub)
{
    return (value > sub) ? (uint16_t)(value - sub) : 0;
}

static uint16_t ScanFastGetNoiseTrigger(void)
{
    return ScanFastSaturatingAdd(scanFastNoiseFloor, SCAN_FAST_RSSI_MARGIN);
}

/* The scanner's BK4829 squelch trigger.  It used to come from
 * `gRxVfo->SquelchOpenRSSIThresh`, which the K1 computes from six bytes of its
 * own EEPROM calibration at 0x010060 -- a region this radio's external flash
 * does not carry (it reads 0xFF), so the trigger was **247** on the 9-bit
 * `0x67` scale, above any signal the radio can report (a keyed carrier measured
 * 236).  The precheck was therefore nearly deaf on the BK4829 lane.  Use the
 * same level-derived mark the receive gate uses (driver/rx.h), so the scanner
 * and the audio agree on what "a signal" is. */
static uint16_t ScanFastReceiveMark(void)
{
    return rx_squelch_open_mark(false, false, gEeprom.SQUELCH_LEVEL);
}

/* Squelch off (level 0) keeps the K1's rule: no fast precheck at all. */
static bool ScanFastSquelchEnabled(void)
{
    return gEeprom.SQUELCH_LEVEL != 0u;
}

static uint16_t ScanFastGetSquelchTrigger(void)
{
    return ScanFastSaturatingSub(ScanFastReceiveMark(), SCAN_FAST_SQUELCH_MARGIN);
}

static bool ScanFastIsNearCandidate(uint16_t rssi)
{
    const uint16_t rssiWithMargin  = ScanFastSaturatingAdd(rssi, SCAN_FAST_WEAK_MARGIN);
    const uint16_t squelchTrigger  = ScanFastGetSquelchTrigger();

    if (scanFastNoiseFloor == SCAN_FAST_RSSI_MAX)
        return ScanFastSquelchEnabled() &&
               rssiWithMargin >= ScanFastReceiveMark();

    return rssiWithMargin >= ScanFastGetNoiseTrigger() &&
           rssiWithMargin >= squelchTrigger;
}

static uint16_t ScanFastReadCandidateRssi(void)
{
    uint16_t rssi = ScanFastReadRssi();

    if (ScanFastIsNearCandidate(rssi))
    {
        SYSTICK_DelayUs(SCAN_FAST_RECHECK_DELAY_US);

        const uint16_t retryRssi = ScanFastReadRssi();
        if (retryRssi > rssi)
            rssi = retryRssi;
    }

    return rssi;
}

static bool ScanFastIsCandidate(uint16_t rssi)
{
    const uint16_t squelchTrigger = ScanFastGetSquelchTrigger();

    if (scanFastNoiseFloor == SCAN_FAST_RSSI_MAX)
    {
        scanFastNoiseFloor = rssi;
        return ScanFastSquelchEnabled() &&
               rssi >= ScanFastReceiveMark();
    }

    const uint16_t noiseTrigger   = ScanFastGetNoiseTrigger();
    const uint16_t rssiWithMargin = ScanFastSaturatingAdd(rssi, SCAN_FAST_WEAK_MARGIN);

    if ((rssi >= noiseTrigger && rssi >= squelchTrigger) ||
        (rssiWithMargin >= noiseTrigger && rssiWithMargin >= squelchTrigger))
    {
        return true;
    }

    if (rssi < scanFastNoiseFloor)
        scanFastNoiseFloor = rssi;
    else
        scanFastNoiseFloor = (uint16_t)((7u * scanFastNoiseFloor + rssi + 4u) >> 3);

    return false;
}

static void ScanFastApplyChannelShape(ModulationMode_t modulation)
{
    const bool modulationChanged = gRxVfo->Modulation != modulation;
    const bool bandwidthChanged  = gRxVfo->CHANNEL_BANDWIDTH != BANDWIDTH_WIDE;

    if (!modulationChanged && !bandwidthChanged)
        return;

    gRxVfo->Modulation         = modulation;
    gRxVfo->CHANNEL_BANDWIDTH  = BANDWIDTH_WIDE;

    if (modulationChanged)
        RADIO_SetModulation(modulation);

    if (modulation == MODULATION_AM)
    {
        BK4819_SetFilterBandwidth(RADIO_GetAMFilterBandwidth(gRxVfo), true);
    }
    else
    {
        BK4819_SetFilterBandwidth(BK4819_FILTER_BW_WIDE, false);
    }

    if (modulationChanged)
    {
        // AM and FM use different demod/AGC profiles, so their RSSI
        // baselines are not directly comparable. Relearn the floor after
        // crossing that boundary instead of treating the next FM block as
        // a wall of candidates.
        ScanFastResetNoiseFloor();
    }
}
#endif

#if defined(ENABLE_FEAT_F4HWN_SCAN_FASTER) && defined(ENABLE_SCAN_RANGES)
static void ScanRangeFastRefineCandidate(uint16_t firstRssi)
{
    const uint16_t step = gRxVfo->StepFrequency;
    if (step == 0 || step >= SCAN_FAST_FINE_STEP_LIMIT)
        return;

    uint16_t maxSteps = SCAN_FAST_FINE_REFINE_SPAN / step;
    if (maxSteps == 0)
        maxSteps = 1;
    if (maxSteps > SCAN_FAST_FINE_REFINE_MAX)
        maxSteps = SCAN_FAST_FINE_REFINE_MAX;

    uint16_t bestRssi = firstRssi;
    uint32_t bestFrequency = gRxVfo->freq_config_RX.Frequency;
    uint8_t fallingSteps = 0;

    for (uint16_t i = 0; i < maxSteps; ++i)
    {
        const uint32_t prevRxFrequency = gRxVfo->pRX->Frequency;

        gRxVfo->freq_config_RX.Frequency = ScanRangeNextFrequency();
        RADIO_ApplyOffset(gRxVfo);

        const uint32_t freq = gRxVfo->pRX->Frequency;
        if ((gScanStateDir > 0 && freq < prevRxFrequency) ||
            (gScanStateDir < 0 && freq > prevRxFrequency))
        {
            break;
        }

        ScanFastTune(freq);

        const uint16_t rssi = ScanFastReadRssi();
        scan_candidate_rate_add(&scanCandidateRate, 1u);
        if (rssi > bestRssi)
        {
            bestRssi = rssi;
            bestFrequency = gRxVfo->freq_config_RX.Frequency;
            fallingSteps = 0;
        }
        else if (bestRssi > rssi && bestRssi - rssi >= SCAN_FAST_FINE_RSSI_DROP)
        {
            if (++fallingSteps >= 3)
                break;
        }
    }

    gRxVfo->freq_config_RX.Frequency = bestFrequency;
    RADIO_ApplyOffset(gRxVfo);
    ScanFastTune(gRxVfo->pRX->Frequency);
}

static scan_fast_result_t ScanRangeFastPrecheck(void)
{
    if (!gScanRangeStart)
        return SCAN_FAST_DISABLED;

    if (!ScanFastSquelchEnabled())
        return SCAN_FAST_DISABLED;

    // Mute AF DAC during the precheck sweep: avoids audio glitches and
    // saves a few uA on each silent step. The bit is restored on the real
    // tune by RADIO_SetupRegisters() in NextFreqChannel().
    scanFastReg30 = BK4819_ReadRegister(BK4819_REG_30) & ~BK4819_REG_30_MASK_ENABLE_AF_DAC;

    for (uint8_t i = 0; i < SCAN_FAST_PRECHECK_STEPS; ++i)
    {
        gRxVfo->freq_config_RX.Frequency = ScanRangeNextFrequency();
        RADIO_ApplyOffset(gRxVfo);

        const uint32_t freq = gRxVfo->pRX->Frequency;

        // Detect wraparound: scanning forward but the new freq is lower
        // than the previous one (or scanning backward but it's higher)
        // means the range has wrapped from stop back to start. Reset the
        // noise floor so the new pass re-warms up from current conditions.
        if (scanFastPrevFrequency != 0 &&
            ((gScanStateDir > 0 && freq < scanFastPrevFrequency) ||
             (gScanStateDir < 0 && freq > scanFastPrevFrequency)))
        {
            ScanFastResetState();
        }

        ScanFastTune(freq);

        const uint16_t rssi = ScanFastReadCandidateRssi();
        scan_candidate_rate_add(&scanCandidateRate, 1u);
#ifdef ENABLE_FEAT_F4HWN_SCAN_RSSI
        ScanRssiSparklinePush(rssi);
#endif
        if (ScanFastIsCandidate(rssi))
        {
            ScanRangeFastRefineCandidate(rssi);
            return SCAN_FAST_CANDIDATE;
        }
    }

    return SCAN_FAST_QUIET_BATCH;
}
#endif

#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
/* Listen-while-scanning (`SetScn = LISTEN`): the selected VFO keeps receiving
 * on its own transceiver while the other one sweeps the range.  Only the scan
 * lane's chip is written:
 *   - the listen lane is left exactly as the coordinator tuned it, so the
 *     poller's RSSI/squelch/audio keep working on it;
 *   - the AF DAC is not muted (unlike the FAST precheck): the listen lane's
 *     audio reaches the amplifier through that node;
 *   - probes outside the listen lane's band are skipped, because a cross-band
 *     probe would move the shared front-end path under the receiver.
 * The walk still uses gRxVfo as its cursor (that is how the K1's range scan
 * steps), which is why rx_service() must not re-tune the lanes while a scan is
 * running -- it yields, see rx.c.  See
 * docs/superpowers/plans/2026-10-19-listen-while-scanning.md. */
static scan_fast_result_t ScanRangeFastPrecheckListen(void)
{
    const bool     listen4815 = (SETTINGS_GetVfoTransceiver(gEeprom.RX_VFO) == RF_XCVR_BK4815);
    const uint32_t listen_f   = gEeprom.VfoInfo[gEeprom.RX_VFO].freq_config_RX.Frequency;
    const bool     listen_uhf = (listen_f >= PA_BAND_SPLIT);

    if (!gScanRangeStart)
        return SCAN_FAST_DISABLED;

    if (!ScanFastSquelchEnabled())
        return SCAN_FAST_DISABLED;

    for (uint8_t i = 0; i < SCAN_FAST_PRECHECK_STEPS; ++i)
    {
        gRxVfo->freq_config_RX.Frequency = ScanRangeNextFrequency();
        RADIO_ApplyOffset(gRxVfo);

        const uint32_t freq = gRxVfo->pRX->Frequency;

        /* Wrap detection, as in ScanRangeFastPrecheck(). */
        if (scanFastPrevFrequency != 0 &&
            ((gScanStateDir > 0 && freq < scanFastPrevFrequency) ||
             (gScanStateDir < 0 && freq > scanFastPrevFrequency)))
        {
            ScanFastResetState();
        }

        /* Stay in the listen lane's band: a probe elsewhere would move the
         * shared band path and make the receiver deaf for that step. */
        if ((freq >= PA_BAND_SPLIT) != listen_uhf)
            continue;

        uint16_t rssi;

        if (listen4815)
        {
            /* Scan lane: the BK4829.  No 0x30 toggle (that is the sweep's own
             * audio mute) and no path change: the probes stay in-band. */
            BK4819_SetFrequency(freq);
            rssi = ScanFastReadCandidateRssi();
        }
        else
        {
            /* Scan lane: the BK4815. */
            bk4815_set_frequency(freq, false);
            bk4815_write_reg(0x75u, listen_uhf ? 0x0Au : 0x11u);
            SYSTICK_DelayUs(SCAN_FAST_RECHECK_DELAY_US);
            bk4815_read_rssi();                       /* discard the stale read */
            rssi = (uint16_t)((bk4815_read_rssi() & 0x7fu) * SCAN_FAST4815_RSSI_SCALE);
        }

        scan_candidate_rate_add(&scanCandidateRate, 1u);
#ifdef ENABLE_FEAT_F4HWN_SCAN_RSSI
        ScanRssiSparklinePush(rssi);
#endif

        if (ScanFastIsCandidate(rssi))
            return SCAN_FAST_CANDIDATE;
    }

    return SCAN_FAST_QUIET_BATCH;
}

static bool MemChannelFastPrecheck(uint16_t channel)
{
    uint32_t frequency;
    ModulationMode_t modulation;

    if (!ScanFastSquelchEnabled())
    {
        scanFastLastFullTuneCandidate = false;
        return true;
    }

    if (!ScanFastUpdateDisplayVfo(channel, &frequency, &modulation))
    {
        scanFastLastFullTuneCandidate = false;
        return true;
    }

    ScanFastApplyChannelShape(modulation);

    // Mute AF DAC for the same reason as in ScanRangeFastPrecheck().
    scanFastReg30 = BK4819_ReadRegister(BK4819_REG_30) & ~BK4819_REG_30_MASK_ENABLE_AF_DAC;
    ScanFastTune(frequency);

    const uint16_t rssi = ScanFastReadCandidateRssi();
    scan_candidate_rate_add(&scanCandidateRate, 1u);
#ifdef ENABLE_FEAT_F4HWN_SCAN_RSSI
    ScanRssiSparklinePush(rssi);
#endif

    if (ScanFastIsCandidate(rssi))
    {
        scanFastLastFullTuneCandidate = true;
        return true;  // signal detected: let the full tune path follow
    }

    // No signal here: still mirror the probed frequency in the VFO so the
    // status line (channel name + frequency) keeps in sync as we skip.
    // RADIO_ConfigureChannel() will overwrite these values cleanly when a
    // candidate is eventually retained.
    scanFastLastFullTuneCandidate = false;
    gRxVfo->freq_config_RX.Frequency = frequency;
    return false;
}

static void AdvanceMemScanList(const bool enabled)
{
    if (enabled)
        if (++currentScanList >= SCAN_NEXT_NUM)
            currentScanList = SCAN_NEXT_CHAN_SCANLIST1;
}

static void SetMemScanProgressChannel(uint16_t channel)
{
    gEeprom.MrChannel[    gEeprom.RX_VFO] = channel;
    gEeprom.ScreenChannel[gEeprom.RX_VFO] = channel;
    gRxVfo->CHANNEL_SAVE = channel;
}
#endif

#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
/* The "Both" scan: two lanes, one per transceiver.  The candidate stream is
 * split by ordinal (scan_dual_assign_candidate) and each lane keeps its own
 * RSSI floor, because the BK4829 (0x67) and BK4815 (0x44) report RSSI on
 * different scales.  The RF path is shared, so the lanes are time-multiplexed,
 * not simultaneous; this whole path is radio-validated work (docs/ra89r_scan.md).
 *
 * Only the frequency range is interleaved for now: the scan list keeps the K1's
 * single-lane fast precheck, which the spec allows as the conservative
 * fallback, so list priority/rotation stays exactly as validated. */
static scan_dual_state_t scanBothState;
static uint8_t           scanBothLastBand = 0xFFu;
static uint32_t          scanBothPrevFrequency;

/* Where the dual-lane batch spends its time, printed by console J: the
 * candidate pull, the tunes and the reads, accumulated in microseconds plus a
 * batch counter.  Profiling only -- it never changes a decision. */
static uint32_t scanBothProfUs[5];      /* 0 pull, 1 tune, 2 read, 3 wall, 4 gap */
static uint32_t scanBothProfBatches;
static uint32_t scanBothProfProbes;
static uint32_t scanBothProfLastEnd;

void CHFRSCANNER_GetScanBothProfile(uint32_t us[5], uint32_t *batches,
                                    uint32_t *probes)
{
    unsigned i;

    for (i = 0; i < 5u && us != 0; i++)
        us[i] = scanBothProfUs[i];
    if (batches != 0)
        *batches = scanBothProfBatches;
    if (probes != 0)
        *probes = scanBothProfProbes;
}

void CHFRSCANNER_ResetScanBothProfile(void)
{
    unsigned i;

    for (i = 0; i < 5u; i++)
        scanBothProfUs[i] = 0u;
    scanBothProfBatches = 0u;
    scanBothProfProbes  = 0u;
    scanBothProfLastEnd = 0u;
}

static bool ScanBothEnabled(void)
{
    return ScanFastEnabled() &&
           gSetting_set_scn == (uint8_t)SCAN_MODE_FAST_BOTH;
}

static void ScanBothReset(void)
{
    scan_dual_reset(&scanBothState);
    scanBothState.mode   = (uint8_t)SCAN_MODE_FAST_BOTH;
    scanBothState.active = true;
    scanBothLastBand     = 0xFFu;
    scanBothPrevFrequency = 0;
}

#ifdef ENABLE_SCAN_RANGES
/* Point the shared front-end at a candidate's band and tune that candidate's
 * chip.  The band switch only happens when the band actually changes. */
static void ScanBothTune(const scan_candidate_t *c)
{
    const uint32_t t0 = systick_micros();

    if (c->band != scanBothLastBand) {
        pa_select_band(c->frequency_10hz);
        scanBothLastBand = c->band;
    }

    if (scan_dual_assign_candidate(c) == SCAN_LANE_BK4815) {
        bk4815_set_frequency(c->frequency_10hz, false);
        bk4815_write_reg(0x75u, (c->frequency_10hz >= PA_BAND_SPLIT) ? 0x0Au : 0x11u);
    } else {
        ScanFastTune(c->frequency_10hz);
    }

    scanBothProfUs[1] += systick_micros() - t0;
}

/* Read a tuned lane's RSSI.  The BK4829 read keeps its glitch-indicator settle
 * and near-candidate re-check; the BK4815 read keeps its fixed settle and the
 * discarded first sample (see ScanBothProbe()'s old form). */
static uint16_t ScanBothRead(scan_lane_chip_t lane, bool already_settled)
{
    const uint32_t t0 = systick_micros();
    uint16_t       rssi;

    if (lane == SCAN_LANE_BK4815) {
        /* The fixed settle is only needed when nothing else covered it: in a
         * pair the BK4829 read runs first and takes longer than this delay. */
        if (!already_settled)
            SYSTICK_DelayUs(SCAN_FAST_RECHECK_DELAY_US);
        bk4815_read_rssi();          /* discard the first, possibly stale read */
        rssi = bk4815_read_rssi();
    } else {
        rssi = ScanFastReadCandidateRssi();
    }

    scanBothProfUs[2] += systick_micros() - t0;
    return rssi;
}

/* A pair of consecutive candidates on different chips in the same band is
 * probed as a pipeline: tune both, then read both.  The second tune's bus time
 * (and, for a BK4815 in the pair, its settle delay) covers the first chip's
 * settling, so the pair costs less than two independent probes.  The shared
 * front end forbids pairing across bands -- a band switch would move the path
 * under the first chip before it is read -- so those fall back to one probe at
 * a time. */
static bool ScanBothPairUsable(const scan_candidate_t *a, const scan_candidate_t *b)
{
    return a->band == b->band &&
           scan_dual_assign_candidate(a) != scan_dual_assign_candidate(b);
}

/* Tune and read a pair; `rssi[0]`/`rssi[1]` follow the candidates' order.  The
 * BK4815 is always tuned and read last, so the BK4829's read runs after both
 * tunes and its duration covers the BK4815's settle -- which is therefore
 * skipped.  Only the stranded 4815 case (a pair that could not be pipelined)
 * still pays the fixed delay. */
static void ScanBothProbePair(const scan_candidate_t *a, const scan_candidate_t *b,
                              uint16_t *rssi)
{
    const scan_lane_chip_t lane_a = scan_dual_assign_candidate(a);
    const scan_lane_chip_t lane_b = scan_dual_assign_candidate(b);

    if (lane_a == SCAN_LANE_BK4815) {
        ScanBothTune(b);
        ScanBothTune(a);
        rssi[1] = ScanBothRead(lane_b, false);   /* BK4829: covers a's settle */
        rssi[0] = ScanBothRead(lane_a, true);
    } else {
        ScanBothTune(a);
        ScanBothTune(b);
        rssi[0] = ScanBothRead(lane_a, false);   /* BK4829: covers b's settle */
        rssi[1] = ScanBothRead(lane_b, true);
    }
}

/* How many candidate pairs one interval probes.  The K1's scanner runs one
 * precheck batch per app pass, and everything outside the precheck (the 10 ms
 * scheduler slot plus the throttled redraw) was ~10.6 ms per batch on
 * 2026-10-21, so a longer batch amortizes it: 48 candidates at 27.8 ms in the
 * batch plus the gap.  A hit is still reported at the end of the batch, i.e.
 * within ~40 ms of sweep.  */
#define SCAN_BOTH_PRECHECK_PAIRS 24u

/* One interval of the dual-lane range sweep, in ordinal order; the earliest
 * ordinal wins if both lanes hit.  No candidate array is kept: the pairs are
 * pulled and evaluated as they are probed, so the batch costs no extra stack. */
static scan_fast_result_t ScanBothFastPrecheck(void)
{
    const unsigned   target = 2u * SCAN_BOTH_PRECHECK_PAIRS;
    unsigned         pulled = 0;
    bool             have_hit = false;
    uint32_t         best_ordinal = 0;
    uint32_t         best_frequency = 0;
    scan_lane_chip_t sel = SCAN_LANE_NONE;

    if (!ScanFastSquelchEnabled())
        return SCAN_FAST_DISABLED;

    /* Seed the BK4829 control register the way ScanRangeFastPrecheck does, with
     * the AF DAC bit cleared: ScanFastTune() writes 0 and then this value, so
     * without it the chip would be left with 0x30 = 0 (disabled). */
    scanFastReg30 = BK4819_ReadRegister(BK4819_REG_30) & ~BK4819_REG_30_MASK_ENABLE_AF_DAC;
    scanBothState.selected_hit = SCAN_LANE_NONE;

    {
        const uint32_t batch_t0 = systick_micros();

        if (scanBothProfLastEnd != 0u)
            scanBothProfUs[4] += batch_t0 - scanBothProfLastEnd;

    while (pulled < target) {
        scan_candidate_t c[2];
        uint16_t         rssi[2] = { 0u, 0u };
        unsigned         have    = 0;
        unsigned         k;
        const uint32_t   pull_t0 = systick_micros();

        while (have < 2u && pulled < target &&
               CHFRSCANNER_NextCandidate(&c[have])) {
            have++;
            pulled++;
        }
        scanBothProfUs[0] += systick_micros() - pull_t0;
        if (have == 0u)
            break;

        scanBothProfProbes += have;

        if (have == 2u && ScanBothPairUsable(&c[0], &c[1]))
            ScanBothProbePair(&c[0], &c[1], rssi);
        else {
            for (k = 0; k < have; k++) {
                ScanBothTune(&c[k]);
                rssi[k] = ScanBothRead(scan_dual_assign_candidate(&c[k]), false);
            }
        }

        for (k = 0; k < have; k++) {
            const scan_lane_chip_t lane = scan_dual_assign_candidate(&c[k]);
            scan_lane_state_t     *lane_state = &scanBothState.lanes[lane];
            const bool             is_4815 = (lane == SCAN_LANE_BK4815);
            const uint16_t         squelch = is_4815
                                                 ? rx_squelch_open_mark(true,
                                                                       c[k].frequency_10hz >= PA_BAND_SPLIT,
                                                                       gEeprom.SQUELCH_LEVEL)
                                                 : ScanFastReceiveMark();
            const uint16_t         noise_margin = is_4815 ? SCAN_FAST4815_RSSI_MARGIN
                                                          : SCAN_FAST_RSSI_MARGIN;
            const uint16_t         squelch_margin = is_4815 ? SCAN_FAST4815_SQUELCH_MARGIN
                                                            : SCAN_FAST_SQUELCH_MARGIN;
            const uint16_t         weak_margin = is_4815 ? SCAN_FAST4815_WEAK_MARGIN
                                                         : SCAN_FAST_WEAK_MARGIN;

            scan_candidate_rate_add(&scanCandidateRate, 1u);

#ifdef ENABLE_FEAT_F4HWN_SCAN_RSSI
            /* Feed the graph from both lanes.  The BK4829 reports 9-bit RSSI
             * (0x67) and the BK4815 7-bit (0x44), so the BK4815 value is scaled
             * into the same range; otherwise the series would sawtooth as lanes
             * alternate. */
            ScanRssiSparklinePush(is_4815
                                      ? (uint16_t)(rssi[k] * SCAN_FAST4815_RSSI_SCALE)
                                      : rssi[k]);
#endif

            /* A range wrap (forward and lower, or backward and higher) is a new
             * pass: re-warm both lanes' floors, as the Default precheck does. */
            if (scanBothPrevFrequency != 0 &&
                ((gScanStateDir > 0 && c[k].frequency_10hz < scanBothPrevFrequency) ||
                 (gScanStateDir < 0 && c[k].frequency_10hz > scanBothPrevFrequency)))
            {
                ScanBothReset();
                scanFastReg30 = BK4819_ReadRegister(BK4819_REG_30) & ~BK4819_REG_30_MASK_ENABLE_AF_DAC;
            }
            scanBothPrevFrequency = c[k].frequency_10hz;

            lane_state->last_frequency_10hz = c[k].frequency_10hz;
            lane_state->candidates++;

            if (scan_dual_rssi_candidate(lane_state, rssi[k], squelch,
                                         noise_margin, squelch_margin, weak_margin) &&
                (!have_hit || c[k].ordinal < best_ordinal)) {
                have_hit       = true;
                best_ordinal   = c[k].ordinal;
                best_frequency = c[k].frequency_10hz;
                sel            = lane;
            }
        }
    }

    if (pulled == 0u)
        return SCAN_FAST_DISABLED;

    scanBothProfBatches++;
    scanBothProfUs[3] += systick_micros() - batch_t0;
    scanBothProfLastEnd = systick_micros();

    if (have_hit) {
        gRxVfo->freq_config_RX.Frequency = best_frequency;
        scanBothState.selected_hit = sel;
        rx_set_scan_source_override(sel == SCAN_LANE_BK4815
                                        ? RX_SCAN_SOURCE_BK4815
                                        : RX_SCAN_SOURCE_BK4829);
        return SCAN_FAST_CANDIDATE;
    }

    return SCAN_FAST_QUIET_BATCH;
    }   /* batch_t0 scope */
}
#endif /* ENABLE_SCAN_RANGES */

/* Diagnostics (console 'J'): the dual-lane snapshot.  `active` is only true
 * while a Both scan is actually running. */
bool CHFRSCANNER_GetScanDualStats(scan_dual_stats_t *out)
{
    if (out == 0)
        return false;

    scan_dual_get_stats(&scanBothState, out);
    out->mode   = gSetting_set_scn;
    out->active = scanBothState.active && ScanBothEnabled() && gScanStateDir != SCAN_OFF;
    scan_candidate_rate_snapshot(&scanCandidateRate, systick_millis(), &out->rate);
    return out->active;
}
#else
bool CHFRSCANNER_GetScanDualStats(scan_dual_stats_t *out)
{
    if (out != 0) {
        memset(out, 0, sizeof *out);
        out->selected_hit = SCAN_LANE_NONE;
    }
    return false;
}
#endif /* ENABLE_FEAT_F4HWN_SCAN_FASTER */

#if defined(ENABLE_SCAN_RANGES) && defined(ENABLE_FEAT_F4HWN_SCAN_FASTER)
/* The console benchmark (see chFrScanner.h).  It forces the selected RX VFO
 * onto a frequency channel and a shared range so CHFRSCANNER_Start() takes the
 * range path, then runs each mode for the caller. */
static struct {
    bool     armed;
    uint8_t  rx_vfo;
    uint16_t screen;
    uint16_t freq_chan;
    uint16_t channel_save;
    uint16_t band;
    uint32_t rx_freq;
    uint32_t tx_freq;
    uint32_t range_start;
    uint32_t range_stop;
    uint8_t  mode;
} scanBench;

void CHFRSCANNER_BenchmarkArm(uint32_t start_10hz, uint32_t stop_10hz)
{
    const uint8_t rx = gEeprom.RX_VFO;

    CHFRSCANNER_ResetScanBothProfile();
    scanBench.armed        = true;
    scanBench.rx_vfo       = rx;
    scanBench.screen       = gEeprom.ScreenChannel[rx];
    scanBench.freq_chan    = gEeprom.FreqChannel[rx];
    scanBench.channel_save = gRxVfo->CHANNEL_SAVE;
    scanBench.band         = gRxVfo->Band;
    scanBench.rx_freq      = gRxVfo->freq_config_RX.Frequency;
    scanBench.tx_freq      = gRxVfo->freq_config_TX.Frequency;
    scanBench.range_start  = gScanRangeStart;
    scanBench.range_stop   = gScanRangeStop;
    scanBench.mode         = gSetting_set_scn;

    /* Frequency mode, so Start() arms the range instead of the memory cursor. */
    gRxVfo->Band              = (uint16_t)FREQUENCY_GetBand(start_10hz);
    gEeprom.ScreenChannel[rx] = (uint16_t)(FREQ_CHANNEL_FIRST + gRxVfo->Band);
    gEeprom.FreqChannel[rx]   = gEeprom.ScreenChannel[rx];
    gRxVfo->CHANNEL_SAVE      = gEeprom.ScreenChannel[rx];
    gRxVfo->freq_config_RX.Frequency = start_10hz;
    gRxVfo->freq_config_TX.Frequency = start_10hz;
    gScanRangeStart = start_10hz;
    gScanRangeStop  = stop_10hz;
}

void CHFRSCANNER_BenchmarkRun(uint8_t mode)
{
    if (!scanBench.armed)
        return;

    gSetting_set_scn = mode ? (uint8_t)SCAN_MODE_FAST_BOTH : (uint8_t)SCAN_MODE_FAST;
    CHFRSCANNER_Start(true, SCAN_FWD);
}

void CHFRSCANNER_BenchmarkFinish(scan_bench_result_t *out)
{
    scan_dual_stats_t st;

    if (gScanStateDir != SCAN_OFF)
        CHFRSCANNER_Stop();

    CHFRSCANNER_GetScanDualStats(&st);
    if (out != 0) {
        out->probes            = st.rate.candidates;
        out->elapsed_ms        = st.rate.elapsed_ms;
        out->probes_per_second = st.rate.candidates_per_second;
    }
}

void CHFRSCANNER_BenchmarkDisarm(void)
{
    const uint8_t rx = scanBench.rx_vfo;

    if (!scanBench.armed)
        return;

    gEeprom.ScreenChannel[rx] = scanBench.screen;
    gEeprom.FreqChannel[rx]   = scanBench.freq_chan;
    gRxVfo->CHANNEL_SAVE      = scanBench.channel_save;
    gRxVfo->Band              = scanBench.band;
    gRxVfo->freq_config_RX.Frequency = scanBench.rx_freq;
    gRxVfo->freq_config_TX.Frequency = scanBench.tx_freq;
    gScanRangeStart = scanBench.range_start;
    gScanRangeStop  = scanBench.range_stop;
    gSetting_set_scn = scanBench.mode;
    RADIO_ConfigureChannel(rx, VFO_CONFIGURE_RELOAD);
    scanBench.armed = false;
}
#else
void CHFRSCANNER_BenchmarkArm(uint32_t start_10hz, uint32_t stop_10hz)
{
    (void)start_10hz;
    (void)stop_10hz;
}

void CHFRSCANNER_BenchmarkRun(uint8_t mode) { (void)mode; }

void CHFRSCANNER_BenchmarkFinish(scan_bench_result_t *out)
{
    if (out != 0) {
        out->probes            = 0u;
        out->elapsed_ms        = 0u;
        out->probes_per_second = 0u;
    }
}

void CHFRSCANNER_BenchmarkDisarm(void) { }

void CHFRSCANNER_GetScanBothProfile(uint32_t us[5], uint32_t *batches,
                                    uint32_t *probes)
{
    if (us != 0)
        us[0] = us[1] = us[2] = us[3] = us[4] = 0u;
    if (batches != 0)
        *batches = 0u;
    if (probes != 0)
        *probes = 0u;
}

void CHFRSCANNER_ResetScanBothProfile(void) { }
#endif

#if defined(ENABLE_FEAT_F4HWN_RESUME_STATE) || defined(ENABLE_SCAN_RANGES)
    void CHFRSCANNER_ScanRange(void) {
        if (gScanRangeStart) {
            gScanRangeStart = 0;
            return;
        }

        gScanRangeStart = gTxVfo->pRX->Frequency;
        gScanRangeStop = gEeprom.VfoInfo[!gEeprom.TX_VFO].freq_config_RX.Frequency;
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
        ScanFastResetState();
#endif
        if(gScanRangeStart > gScanRangeStop)
            SWAP(gScanRangeStart, gScanRangeStop);

        ScanRangeSkipSync();
    }
#endif

void CHFRSCANNER_Start(const bool storeBackupSettings, const int8_t scan_direction)
{
    if (storeBackupSettings) {
        initialCROSS_BAND_RX_TX = gEeprom.CROSS_BAND_RX_TX;
        gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;
        gScanKeepResult = false;
    }
    
    RADIO_SelectVfos();
    CHFRSCANNER_AbortActiveReception();

    gNextMrChannel   = gRxVfo->CHANNEL_SAVE;
    currentScanList = SCAN_NEXT_CHAN_SCANLIST1;
    gScanStateDir    = scan_direction;
    scanCandidateOrdinal = 0;
    rx_clear_scan_source_override();
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
    if (storeBackupSettings)
        scan_candidate_rate_reset(&scanCandidateRate, systick_millis());
#endif
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
#ifdef ENABLE_FEAT_F4HWN_SCAN_RSSI
    ScanRssiSparklineReset();
#endif
    ScanFastResetState();
#endif

    if (IS_MR_CHANNEL(gNextMrChannel))
    {
        bool scanListChanged = false;

        if(!RADIO_CheckValidList(gEeprom.SCAN_LIST_DEFAULT)) {
            RADIO_NextValidList(1);
            scanListChanged = true;
        }

        if (storeBackupSettings || scanListChanged)
            UI_MAIN_NotifyScanListChanged();

        // channel mode
        if (storeBackupSettings) {
            initialFrqOrChan = gRxVfo->CHANNEL_SAVE;
            lastFoundFrqOrChan = initialFrqOrChan;
        }
        NextMemChannel();
    }
    else
    {   // frequency mode
        if (storeBackupSettings) {
            initialFrqOrChan = gRxVfo->freq_config_RX.Frequency;
            lastFoundFrqOrChan = initialFrqOrChan;
        }
        NextFreqChannel();
    }

#ifdef ENABLE_FEAT_F4HWN
    lastFoundFrqOrChanOld = lastFoundFrqOrChan;
#endif

    gScanPauseDelayIn_10ms = scan_pause_delay_in_2_10ms;
    gScheduleScanListen    = false;
    gRxReceptionMode       = RX_MODE_NONE;
    gScanPauseMode         = false;
}

void CHFRSCANNER_ManualResume(const int8_t scan_direction)
{
    CHFRSCANNER_Start(false, scan_direction);

    gScanPauseDelayIn_10ms = !ScanFastSquelchEnabled()
        ? scan_pause_delay_in_3_10ms
        : 1;
    gScheduleScanListen    = false;
}

/*
void CHFRSCANNER_ContinueScanning(void)
{
    if (IS_FREQ_CHANNEL(gNextMrChannel))
    {
        if (gCurrentFunction == FUNCTION_INCOMING)
            APP_StartListening(gMonitor ? FUNCTION_MONITOR : FUNCTION_RECEIVE);
        else
            NextFreqChannel();  // switch to next frequency
    }
    else
    {
        if (gCurrentCodeType == CODE_TYPE_OFF && gCurrentFunction == FUNCTION_INCOMING)
            APP_StartListening(gMonitor ? FUNCTION_MONITOR : FUNCTION_RECEIVE);
        else
            NextMemChannel();    // switch to next channel
    }
    
    gScanPauseMode      = false;
    gRxReceptionMode    = RX_MODE_NONE;
    gScheduleScanListen = false;
}
*/

void CHFRSCANNER_ContinueScanning(void)
{
    /* A resumed scan is a fresh pass: drop any paused-hit receive override so
     * the receiver follows the saved VFO route again until the next hit. */
    rx_clear_scan_source_override();
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
    if (scanFastLastFullTuneCandidate &&
        gCurrentFunction != FUNCTION_INCOMING &&
        !g_SquelchLost)
    {
        // A rejected full-tune candidate is just a false RSSI hit. Keep the
        // learned floor; resetting here can make the next channel blind when
        // it is the real signal, especially while scanning down.
        scanFastLastFullTuneCandidate = false;
    }
#endif

    if (gCurrentFunction == FUNCTION_INCOMING &&
        (IS_FREQ_CHANNEL(gNextMrChannel) || gCurrentCodeType == CODE_TYPE_OFF))
    {
        APP_StartListening(gMonitor ? FUNCTION_MONITOR : FUNCTION_RECEIVE);
    }
    else
    {
        IS_FREQ_CHANNEL(gNextMrChannel) ? NextFreqChannel() : NextMemChannel();
    }

    gScanPauseMode      = false;
    gRxReceptionMode    = RX_MODE_NONE;
    gScheduleScanListen = false;
}

void CHFRSCANNER_Found(void)
{
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
    // After a real reception the BK4819 AGC has shifted, biasing the next
    // few RSSI readings high. Reset the precheck state so it warms up from
    // the current noise floor instead of carrying stale calibration into
    // the post-reception scan, which otherwise turns nearly every channel
    // into a CANDIDATE and erases the speed gain.
    ScanFastResetState();
#endif

    if (gEeprom.SCAN_RESUME_MODE > 80) {
        if (!gScanPauseMode) {
            gScanPauseDelayIn_10ms = scan_pause_delay_in_5_10ms * (gEeprom.SCAN_RESUME_MODE - 80) * 5;
            gScanPauseMode = true;
        }
    } else {
        gScanPauseDelayIn_10ms = 0;
    }

    // gScheduleScanListen is always false...
    gScheduleScanListen = false;

    /*
    if(gEeprom.SCAN_RESUME_MODE > 1 && gEeprom.SCAN_RESUME_MODE < 26)
    {
        if (!gScanPauseMode)
        {
            gScanPauseDelayIn_10ms = scan_pause_delay_in_5_10ms * (gEeprom.SCAN_RESUME_MODE - 1) * 5;
            gScheduleScanListen    = false;
            gScanPauseMode         = true;
        }
    }
    else
    {
        gScanPauseDelayIn_10ms = 0;
        gScheduleScanListen    = false;
    }
    */

    /*
    switch (gEeprom.SCAN_RESUME_MODE)
    {
        case SCAN_RESUME_TO:
            if (!gScanPauseMode)
            {
                gScanPauseDelayIn_10ms = scan_pause_delay_in_1_10ms;
                gScheduleScanListen    = false;
                gScanPauseMode         = true;
            }
            break;

        case SCAN_RESUME_CO:
        case SCAN_RESUME_SE:
            gScanPauseDelayIn_10ms = 0;
            gScheduleScanListen    = false;
            break;
    }
    */

#ifdef ENABLE_FEAT_F4HWN
    lastFoundFrqOrChanOld = lastFoundFrqOrChan;
#endif

    if (IS_MR_CHANNEL(gRxVfo->CHANNEL_SAVE)) { //memory scan
        lastFoundFrqOrChan = gRxVfo->CHANNEL_SAVE;
    }
    else { // frequency scan
        lastFoundFrqOrChan = gRxVfo->freq_config_RX.Frequency;
    }


    gScanKeepResult = true;
}

void CHFRSCANNER_Stop(void)
{
    if(initialCROSS_BAND_RX_TX != CROSS_BAND_OFF) {
        gEeprom.CROSS_BAND_RX_TX = initialCROSS_BAND_RX_TX;
        initialCROSS_BAND_RX_TX = CROSS_BAND_OFF;
    }
    
    gScanStateDir = SCAN_OFF;
    rx_clear_scan_source_override();
#if defined(ENABLE_FEAT_F4HWN_SCAN_FASTER) && defined(ENABLE_FEAT_F4HWN_SCAN_RSSI)
    ScanRssiSparklineReset();
#endif

    const uint32_t chFr = gScanKeepResult ? lastFoundFrqOrChan : initialFrqOrChan;
    const bool channelChanged = chFr != initialFrqOrChan;
    if (IS_MR_CHANNEL(gNextMrChannel)) {
        gEeprom.MrChannel[gEeprom.RX_VFO]     = chFr;
        gEeprom.ScreenChannel[gEeprom.RX_VFO] = chFr;
        RADIO_ConfigureChannel(gEeprom.RX_VFO, VFO_CONFIGURE_RELOAD);

        if(channelChanged) {
            SETTINGS_SaveVfoIndices();
            gUpdateStatus = true;
        }
    }
    else {
        gRxVfo->freq_config_RX.Frequency = chFr;
        RADIO_ApplyOffset(gRxVfo);
        RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
        if(channelChanged) {
            SETTINGS_SaveChannel(gRxVfo->CHANNEL_SAVE, gEeprom.RX_VFO, gRxVfo, 1);
        }
    }

    #ifdef ENABLE_FEAT_F4HWN_RESUME_STATE
        gEeprom.CURRENT_STATE = 0;
        SETTINGS_WriteCurrentState();
    #endif

    RADIO_SetupRegisters(true);
    gUpdateDisplay = true;
}

/* The sweep's per-quiet-batch display refresh was a large share of the wall
 * time (radio 2026-10-21: 32 ms of probe work inside a 48 ms batch), so the
 * status line is redrawn every `SCAN_DISPLAY_DIVIDER` batches instead of every
 * one.  A hit still refreshes immediately. */
#define SCAN_DISPLAY_DIVIDER 4
static uint8_t scanDisplayDivider;

static void ScanRequestDisplay(void)
{
    if (++scanDisplayDivider >= SCAN_DISPLAY_DIVIDER) {
        scanDisplayDivider = 0;
        gUpdateDisplay     = true;
    }
}

static void NextFreqChannel(void)
{
#ifdef ENABLE_SCAN_RANGES
    if(gScanRangeStart) {
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
        if (ScanBothEnabled())
        {
            const scan_fast_result_t bothResult = ScanBothFastPrecheck();

            if (bothResult == SCAN_FAST_QUIET_BATCH)
            {
                scanFastLastFullTuneCandidate = false;
                gScanPauseDelayIn_10ms = 1;
                ScanRequestDisplay();
                return;
            }

            if (bothResult == SCAN_FAST_DISABLED)
            {
                scanFastLastFullTuneCandidate = false;
                gRxVfo->freq_config_RX.Frequency = ScanRangeNextFrequency();
            }
            else
            {
                scanFastLastFullTuneCandidate = true;
            }
        }
        else if (ScanListenEnabled())
        {
            const scan_fast_result_t listenResult = ScanRangeFastPrecheckListen();

            if (listenResult == SCAN_FAST_QUIET_BATCH)
            {
                scanFastLastFullTuneCandidate = false;
                gScanPauseDelayIn_10ms = 1;
                ScanRequestDisplay();
                return;
            }

            if (listenResult == SCAN_FAST_DISABLED)
            {
                scanFastLastFullTuneCandidate = false;
                gRxVfo->freq_config_RX.Frequency = ScanRangeNextFrequency();
            }
            else
            {
                scanFastLastFullTuneCandidate = true;
            }
        }
        else if (ScanFastEnabled())
        {
            const scan_fast_result_t fastResult = ScanRangeFastPrecheck();

            if (fastResult == SCAN_FAST_QUIET_BATCH)
            {
                scanFastLastFullTuneCandidate = false;
                gScanPauseDelayIn_10ms = 1;
                ScanRequestDisplay();
                return;
            }

            if (fastResult == SCAN_FAST_DISABLED)
            {
                scanFastLastFullTuneCandidate = false;
                gRxVfo->freq_config_RX.Frequency = ScanRangeNextFrequency();
            }
            else
            {
                scanFastLastFullTuneCandidate = true;
            }
        }
        else
        {
            scanFastLastFullTuneCandidate = false;
            gRxVfo->freq_config_RX.Frequency = ScanRangeNextFrequency();
        }
#else
        gRxVfo->freq_config_RX.Frequency = ScanRangeNextFrequency();
#endif
    }
    else
#endif
    {
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
        scanFastLastFullTuneCandidate = false;
#endif
        gRxVfo->freq_config_RX.Frequency = APP_SetFrequencyByStep(gRxVfo, gScanStateDir);
    }

    RADIO_ApplyOffset(gRxVfo);
    RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
    RADIO_SetupRegisters(true);

#ifdef ENABLE_FASTER_CHANNEL_SCAN
    gScanPauseDelayIn_10ms = 9;   // 90ms
#else
    gScanPauseDelayIn_10ms = scan_pause_delay_in_6_10ms;
#endif

    gUpdateDisplay     = true;
}

/* The memory-scan cursor: pick the next valid list channel, honouring the
 * configured scan list, the two priority channels, the direction and the wrap.
 * It updates currentScanList/gNextMrChannel exactly as the K1 does; the RF work
 * and the fast precheck stay in NextMemChannel(), and CHFRSCANNER_NextCandidate
 * reuses it for the dual-lane path. */
static uint16_t CHFRSCANNER_NextMemCursor(const bool enabled)
{
    static uint16_t prev_mr_chan = 0;
    const int16_t   chan1        = (gEeprom.SCAN_LIST_DEFAULT > 0 && gEeprom.SCAN_LIST_DEFAULT <= MR_CHANNELS_LIST + 1 && gEeprom.SCANLIST_PRIORITY_CH[0] != MR_CHANNELS_MAX) ? gEeprom.SCANLIST_PRIORITY_CH[0] : -1;
    const int16_t   chan2        = (gEeprom.SCAN_LIST_DEFAULT > 0 && gEeprom.SCAN_LIST_DEFAULT <= MR_CHANNELS_LIST + 1 && gEeprom.SCANLIST_PRIORITY_CH[1] != MR_CHANNELS_MAX) ? gEeprom.SCANLIST_PRIORITY_CH[1] : -1;
    uint16_t        chan         = 0;

    if (enabled)
    {
        switch (currentScanList)
        {
            case SCAN_NEXT_CHAN_SCANLIST1:
                prev_mr_chan = gNextMrChannel;
    
                //sprintf(str, "-> Chan1 %d\n", chan1 + 1);
                //LogUart(str);

                if (chan1 >= 0)
                {
                    if (RADIO_CheckValidChannel(chan1, false, gEeprom.SCAN_LIST_DEFAULT))
                    {
                        currentScanList = SCAN_NEXT_CHAN_SCANLIST1;
                        gNextMrChannel   = chan1;
                        break;
                    }
                }

                [[fallthrough]];
            case SCAN_NEXT_CHAN_SCANLIST2:

                //sprintf(str, "-> Chan2 %d\n", chan2 + 1);
                //LogUart(str);

                if (chan2 >= 0)
                {
                    if (RADIO_CheckValidChannel(chan2, false, gEeprom.SCAN_LIST_DEFAULT))
                    {
                        currentScanList = SCAN_NEXT_CHAN_SCANLIST2;
                        gNextMrChannel   = chan2;
                        break;
                    }
                }

                [[fallthrough]];
            /*
            case SCAN_NEXT_CHAN_SCANLIST3:
                if (chan3 >= 0)
                {
                    if (RADIO_CheckValidChannel(chan3, false, 0))
                    {
                        currentScanList = SCAN_NEXT_CHAN_SCANLIST3;
                        gNextMrChannel   = chan3;
                        break;
                    }
                }
                [[fallthrough]];
            */
            // this bit doesn't yet work if the other VFO is a frequency
            case SCAN_NEXT_CHAN_DUAL_WATCH:
                // dual watch is enabled - include the other VFO in the scan
//              if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF)
//              {
//                  chan = (gEeprom.RX_VFO + 1) & 1u;
//                  chan = gEeprom.ScreenChannel[chan];
//                  if (IS_MR_CHANNEL(chan))
//                  {
//                      currentScanList = SCAN_NEXT_CHAN_DUAL_WATCH;
//                      gNextMrChannel   = chan;
//                      break;
//                  }
//              }

            default:
            case SCAN_NEXT_CHAN_MR:
                currentScanList = SCAN_NEXT_CHAN_MR;
                gNextMrChannel   = prev_mr_chan;
                chan             = 0xFFFF;
                break;
        }
    }

    if (!enabled || chan == 0xFFFF)
    {
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
        const uint16_t searchStart = gNextMrChannel;
#endif
        chan = RADIO_FindNextChannel(gNextMrChannel + gScanStateDir, gScanStateDir, true, gEeprom.SCAN_LIST_DEFAULT);
        if (chan == 0xFFFF)
        {   // no valid channel found -> wrapping back to the first channel
            chan = MR_CHANNEL_FIRST;
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
            // Wraparound: re-warm the precheck noise floor on the new pass
            // so it tracks current RF conditions instead of an EMA that
            // accumulated drift over the previous full sweep.
            ScanFastResetState();
#endif
        }
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
        else if ((gScanStateDir > 0 && chan < searchStart) ||
                 (gScanStateDir < 0 && chan > searchStart))
        {
            // RADIO_FindNextChannel() wraps internally, so 0xFFFF is not
            // returned on a normal full-sweep wrap. Detect that transition
            // here and restart the RSSI floor learning for the new pass.
            ScanFastResetState();
        }
#endif

        gNextMrChannel = chan;

        //sprintf(str, "----> Chan %d\n", chan + 1);
        //LogUart(str);
    }

    return gNextMrChannel;
}

static void NextMemChannel(void)
{
    const bool     enabled   = (gEeprom.SCAN_LIST_DEFAULT > 0 && gEeprom.SCAN_LIST_DEFAULT <= MR_CHANNELS_LIST + 1) ? gEeprom.SCAN_LIST_ENABLED : true;
    const uint16_t prev_chan = gNextMrChannel;

    (void)CHFRSCANNER_NextMemCursor(enabled);

#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
    SetMemScanProgressChannel(gNextMrChannel);

    if (ScanFastEnabled() && !MemChannelFastPrecheck(gNextMrChannel))
    {
        gScanPauseDelayIn_10ms = 1;
        gUpdateDisplay = true;
        AdvanceMemScanList(enabled);
        return;
    }
#endif

    if (gNextMrChannel != prev_chan
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
        || scanFastLastFullTuneCandidate
#endif
    )
    {
#ifndef ENABLE_FEAT_F4HWN_SCAN_FASTER
        gEeprom.MrChannel[    gEeprom.RX_VFO] = gNextMrChannel;
        gEeprom.ScreenChannel[gEeprom.RX_VFO] = gNextMrChannel;
#endif

        RADIO_ConfigureChannel(gEeprom.RX_VFO, VFO_CONFIGURE_RELOAD);
        RADIO_SetupRegisters(true);

        gUpdateDisplay = true;
    }

#ifdef ENABLE_FASTER_CHANNEL_SCAN
    gScanPauseDelayIn_10ms = 9;  // 90ms .. <= ~60ms it misses signals (squelch response and/or PLL lock time) ?
#else
    gScanPauseDelayIn_10ms = scan_pause_delay_in_3_10ms;
#endif

#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
    AdvanceMemScanList(enabled);
#else
    if (enabled)
        if (++currentScanList >= SCAN_NEXT_NUM)
            currentScanList = SCAN_NEXT_CHAN_SCANLIST1;  // back round we go
#endif
}

/* The shared candidate cursor the dual-lane scanner draws from.  It advances
 * the same K1 order as NextFreqChannel()/NextMemChannel() -- range
 * step/limits/skip for a frequency sweep, list membership/priority/direction/
 * wrap for channels -- and tags each item with a monotonically increasing
 * ordinal.  It does not tune or precheck; the caller does.  Returns false when
 * there is nothing to visit. */
bool CHFRSCANNER_NextCandidate(scan_candidate_t *out)
{
    if (out == 0)
        return false;

#ifdef ENABLE_SCAN_RANGES
    if (gScanRangeStart)
    {
        out->frequency_10hz    = ScanRangeNextFrequency();
        out->channel           = 0;
        out->band              = (uint8_t)FREQUENCY_GetBand(out->frequency_10hz);
        out->is_memory_channel = false;
        out->ordinal           = scanCandidateOrdinal++;
        return true;
    }
#endif

    {
        const bool     enabled = (gEeprom.SCAN_LIST_DEFAULT > 0 && gEeprom.SCAN_LIST_DEFAULT <= MR_CHANNELS_LIST + 1) ? gEeprom.SCAN_LIST_ENABLED : true;
        const uint16_t chan    = CHFRSCANNER_NextMemCursor(enabled);
        ChannelScanDisplayInfo_t info;

        if (!IS_MR_CHANNEL(chan))
            return false;

        if (!SETTINGS_FetchChannelScanDisplayInfo(gEeprom.RX_VFO, chan, &info))
            return false;

        out->channel           = chan;
        out->is_memory_channel = true;
        out->frequency_10hz    = info.rx.Frequency;
        out->band              = (uint8_t)FREQUENCY_GetBand(out->frequency_10hz);
        out->ordinal           = scanCandidateOrdinal++;
        return true;
    }
}
