
#include <stddef.h>
#include <string.h>

#include "app/app.h"
#include "app/chFrScanner.h"
#include "app/scan_rate.h"
#include "audio.h"
#include "driver/bk4815.h"
#include "driver/pa.h"
#include "driver/rx.h"
#include "driver/uart.h"
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
#endif

#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
static bool ScanFastEnabled(void)
{
    return gSetting_set_scn != (uint8_t)SCAN_MODE_NORMAL;
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
/* The BK4815's 7-bit RSSI (0x44) is scaled into the BK4829's 9-bit range
 * (127*4 ~ 508) for the sparkline, so the graph reads the same on either chip. */
#define SCAN_FAST4815_RSSI_SCALE     4
#define SCAN_FAST_RECHECK_DELAY_US 350
#define SCAN_FAST_FINE_STEP_LIMIT   250
#define SCAN_FAST_FINE_REFINE_SPAN 1000
#define SCAN_FAST_FINE_REFINE_MAX    80
#define SCAN_FAST_FINE_RSSI_DROP      8
#define SCAN_FAST_RSSI_MAX          65535u
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
/* The frequency the BK4829 was last probed on: the scanner's mark is taken for
 * that band (rx_squelch_open_mark_for()), not for wherever the receiver sat. */
static uint32_t scanFastMarkFreq;

/* Console '%': per-probe scan trace (range and memory FAST, full tunes,
 * resumes) on the UART.  Blocking output, so it slows the sweep; a
 * bench diagnostic, off by default. */
static bool scanTrace;

bool CHFRSCANNER_ToggleTrace(void)
{
    scanTrace = !scanTrace;
    return scanTrace;
}

bool CHFRSCANNER_TraceOn(void)
{
    return scanTrace;
}
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
}

static void ScanFastResetNoiseFloor(void)
{
    scanFastNoiseFloor = SCAN_FAST_RSSI_MAX;
}

/* The BK4829's 0x67 right after ScanFastTune()'s REG_30 restart is not the
 * channel: the meter restarts low and climbs while the AGC recovers.  Radio,
 * 2026-10-27 (a per-probe trace), PMR + airband list, keyed carrier on PMR1:
 *   - read at once (the K1's glitch guard never waited -- the indicator is
 *     low right after the restart): 14-15 on every channel, carrier included;
 *   - read at the first value over 28 (~1.1-1.6 ms): 30-39, PMR1 only 43.
 * A live receiver sits at 49-61 UHF / 96-103 VHF, and a carrier far above.
 * So follow the meter (~135 us per read on this bus) until it holds still:
 * SCAN_FAST_PLATEAU_READS reads in a row within SCAN_FAST_PLATEAU_TOL of a
 * reference reading, which restarts whenever a read leaves that band -- a ramp
 * up after a restart, or a fall from the previous channel's level after a
 * plain retune.  The result is the *mean* of the plateau reads: the peak of
 * 8-40 noisy reads put plain noise at 90-110 UHF / 80-90 VHF, above the
 * receive gate's single-read marks (75-81), and every pass started with false
 * hits (radio, same session); a keyed carrier read 222 either way.  The cap
 * keeps a dead chip from stalling the scan. */
#define SCAN_FAST_PLATEAU_READS     8u
#define SCAN_FAST_PLATEAU_TOL       6u
#define SCAN_FAST_SETTLE_MAX_US 20000u

static uint16_t ScanFastReadRssi(void)
{
    const uint32_t t0 = systick_micros();
    uint16_t ref  = BK4819_GetRSSI();
    uint16_t last = ref;
    uint8_t  flat = 0;
    uint32_t sum  = 0;    /* the reads inside the band around `ref` */

    while (flat < SCAN_FAST_PLATEAU_READS &&
           (uint32_t)(systick_micros() - t0) < SCAN_FAST_SETTLE_MAX_US)
    {
        const uint16_t r    = BK4819_GetRSSI();
        const uint16_t diff = (r > ref) ? (uint16_t)(r - ref) : (uint16_t)(ref - r);

        last = r;
        if (diff > SCAN_FAST_PLATEAU_TOL) {
            ref  = r;
            flat = 0;
            sum  = 0;
        } else {
            flat++;
            sum += r;
        }
    }

    /* Timed out still moving: the latest reading is the best estimate. */
    return flat ? (uint16_t)((sum + flat / 2u) / flat) : last;
}

uint16_t CHFRSCANNER_ReadRssiSettled(void)
{
    return ScanFastReadRssi();
}

/* REG_30 as the chip holds it (read at each batch's seed), so the plain-write
 * tune below only rewrites it when the AF DAC mute is not applied yet. */
static uint16_t scanFastReg30Chip;

/* Seed the probe's REG_30: the chip's current value with the AF DAC cleared, so
 * the sweep is silent.  ScanFastTune() writes 0 and then this value in the
 * restart mode, so without the seed the chip would be left disabled. */
static void ScanFastSeedReg30(void)
{
    scanFastReg30Chip = BK4819_ReadRegister(BK4819_REG_30);
    scanFastReg30     = scanFastReg30Chip & ~BK4819_REG_30_MASK_ENABLE_AF_DAC;
}

/* How a probe retunes the BK4829.  The K1 restarts the receiver
 * (REG_30 = 0, then the old value) after the 0x38/0x39 write; on this board the
 * meter then restarts at ~0 and climbs for 1-14 ms (docs/ra89r_scan.md), which
 * ScanFastReadRssi() has to wait out.  The plain write only moves the
 * synthesizer and leaves the AGC running.  Console '#' switches back to the restart for an A/B with `J`. */
static bool scanFastPlainTune = true;

/* The plain write is only for a step: the restart it skips also runs the
 * BK4829's VCO calibration (REG_30's VCO_CALIB bit), and after a long jump the
 * synthesizer is still settling when the meter is read.  Radio 2026-10-28,
 * range 446.00625-447.00625: the wrap back to 446.00625 (a 1 MHz jump) read a
 * keyed PMR1 carrier as 84, noise level.  Anything further than this from the
 * previous probe (10 Hz units: 100 kHz) takes the restart. */
#define SCAN_FAST_PLAIN_MAX_JUMP 10000u

bool CHFRSCANNER_TogglePlainTune(void)
{
    scanFastPlainTune = !scanFastPlainTune;
    return scanFastPlainTune;
}

static void ScanFastTune(uint32_t frequency)
{
    if (scanFastPrevFrequency == 0 ||
        ((frequency < SCAN_FAST_HF_VHF_BOUNDARY_HZ) !=
         (scanFastPrevFrequency < SCAN_FAST_HF_VHF_BOUNDARY_HZ)))
    {
        BK4819_PickRXFilterPathBasedOnFrequency(frequency);
    }

    const uint32_t jump = (frequency > scanFastPrevFrequency)
                              ? frequency - scanFastPrevFrequency
                              : scanFastPrevFrequency - frequency;
    const bool     plain = scanFastPlainTune && scanFastPrevFrequency != 0 &&
                           jump <= SCAN_FAST_PLAIN_MAX_JUMP;

    scanFastPrevFrequency = frequency;
    scanFastMarkFreq      = frequency;
    BK4819_SetFrequency(frequency);

    if (plain) {
        /* Only the AF DAC mute, once per batch; no restart. */
        if (scanFastReg30Chip != scanFastReg30) {
            BK4819_WriteRegister(BK4819_REG_30, scanFastReg30);
            scanFastReg30Chip = scanFastReg30;
        }
        return;
    }

    BK4819_WriteRegister(BK4819_REG_30, 0);
    BK4819_WriteRegister(BK4819_REG_30, scanFastReg30);
    scanFastReg30Chip = scanFastReg30;
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
 * live mark from the receive gate (driver/rx.c), which tracks the lane's floor,
 * so the scanner and the audio agree on what "a signal" is. */
static uint16_t ScanFastReceiveMark(void)
{
    /* The BK4829's own scale, at the probed band: the *active* lane's mark is
     * on the BK4815's 7-bit scale when the VFO is on that chip, and on the
     * receiver's band rather than the probe's. */
    return rx_squelch_open_mark_for(false, scanFastMarkFreq);
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

/* The single-lane FAST precheck probes the chip the full tune will land on --
 * the selected VFO's transceiver (rx_scan_commit()).  It used to probe the
 * BK4829 unconditionally: with the VFO on the BK4815 that chip's receive-path
 * pins are set for the BK4815 lane (~16 dB down on the BK4829 at VHF) and the
 * mark was on the other chip's scale. */
static bool ScanFastLaneIs4815(void)
{
    return SETTINGS_GetVfoTransceiver(gEeprom.RX_VFO) == RF_XCVR_BK4815;
}

/* One BK4815 probe: tune + band, the fixed settle, a discarded first read. */
static uint16_t ScanFast4815Read(uint32_t frequency)
{
    bk4815_set_frequency(frequency, false);
    bk4815_set_band((frequency >= PA_BAND_SPLIT) ? 0x0Au : 0x11u);
    SYSTICK_DelayUs(SCAN_FAST_RECHECK_DELAY_US);
    bk4815_read_rssi();
    return (uint16_t)(bk4815_read_rssi() & 0x7fu);
}

/* A BK4815-lane candidate: at or over the lane's mark on two probes.  The
 * confirmation keeps a single noisy sample from costing a full tune.  `display` gets the reading scaled
 * into the 9-bit sparkline range. */
static bool ScanFast4815Probe(uint32_t frequency, uint16_t *display)
{
    const uint16_t mark = rx_squelch_open_mark_for(true, frequency);
    const uint16_t raw  = ScanFast4815Read(frequency);

    if (display)
        *display = (uint16_t)(raw * SCAN_FAST4815_RSSI_SCALE);

    return raw >= mark && ScanFast4815Read(frequency) >= mark;
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
    ScanFastSeedReg30();

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

        if (ScanFastLaneIs4815())
        {
            uint16_t display = 0;
            const bool hit = ScanFast4815Probe(freq, &display);

            if (scanTrace)
                uart_printf("rng %u 4815 rssi %u mark %u -> %s\n", (unsigned)freq,
                            (unsigned)(display / SCAN_FAST4815_RSSI_SCALE),
                            (unsigned)rx_squelch_open_mark_for(true, freq),
                            hit ? "HIT" : "-");
            scan_candidate_rate_add(&scanCandidateRate, 1u);
#ifdef ENABLE_FEAT_F4HWN_SCAN_RSSI
            ScanRssiSparklinePush(display);
#endif
            if (hit)
                return SCAN_FAST_CANDIDATE;
            continue;
        }

        ScanFastTune(freq);

        const uint16_t rssi = ScanFastReadCandidateRssi();
        const uint16_t trace_floor = scanFastNoiseFloor;
        const bool     hit = ScanFastIsCandidate(rssi);

        if (scanTrace)
            uart_printf("rng %u 4829 rssi %u floor %u mark %u -> %s\n", (unsigned)freq,
                        (unsigned)rssi, (unsigned)trace_floor,
                        (unsigned)ScanFastReceiveMark(), hit ? "HIT" : "-");
        scan_candidate_rate_add(&scanCandidateRate, 1u);
#ifdef ENABLE_FEAT_F4HWN_SCAN_RSSI
        ScanRssiSparklinePush(rssi);
#endif
        if (hit)
        {
            ScanRangeFastRefineCandidate(rssi);
            return SCAN_FAST_CANDIDATE;
        }
    }

    return SCAN_FAST_QUIET_BATCH;
}
#endif

#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
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

    bool     hit;
    uint16_t rssi = 0;
    if (ScanFastLaneIs4815())
    {
        hit = ScanFast4815Probe(frequency, &rssi);
    }
    else
    {
        ScanFastApplyChannelShape(modulation);

        // Mute AF DAC for the same reason as in ScanRangeFastPrecheck().
        ScanFastSeedReg30();
        ScanFastTune(frequency);

        rssi = ScanFastReadCandidateRssi();
        hit  = ScanFastIsCandidate(rssi);
    }


    if (scanTrace)
        uart_printf("mem ch%u %u %s rssi %u -> %s\n", (unsigned)(channel + 1u),
                    (unsigned)frequency, ScanFastLaneIs4815() ? "4815(x4)" : "4829",
                    (unsigned)rssi, hit ? "HIT" : "-");
    scan_candidate_rate_add(&scanCandidateRate, 1u);
#ifdef ENABLE_FEAT_F4HWN_SCAN_RSSI
    ScanRssiSparklinePush(rssi);
#endif

    if (hit)
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
/* Diagnostics (console 'J'): probes and wall time since the scan started. */
void CHFRSCANNER_GetScanRate(scan_candidate_rate_stats_t *out)
{
    scan_candidate_rate_snapshot(&scanCandidateRate, systick_millis(), out);
}
#else
void CHFRSCANNER_GetScanRate(scan_candidate_rate_stats_t *out)
{
    if (out != 0)
        memset(out, 0, sizeof *out);
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

void CHFRSCANNER_BenchmarkRun(void)
{
    if (!scanBench.armed)
        return;

    gSetting_set_scn = (uint8_t)SCAN_MODE_FAST;
    CHFRSCANNER_Start(true, SCAN_FWD);
}

void CHFRSCANNER_BenchmarkFinish(scan_bench_result_t *out)
{
    scan_candidate_rate_stats_t st;

    if (gScanStateDir != SCAN_OFF)
        CHFRSCANNER_Stop();

    CHFRSCANNER_GetScanRate(&st);
    if (out != 0) {
        out->probes            = st.candidates;
        out->elapsed_ms        = st.elapsed_ms;
        out->probes_per_second = st.candidates_per_second;
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

void CHFRSCANNER_BenchmarkRun(void) { }

void CHFRSCANNER_BenchmarkFinish(scan_bench_result_t *out)
{
    if (out != 0) {
        out->probes            = 0u;
        out->elapsed_ms        = 0u;
        out->probes_per_second = 0u;
    }
}

void CHFRSCANNER_BenchmarkDisarm(void) { }
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
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
    if (scanTrace)
        uart_printf("CONT fn %u sql %u rssi %u\n", (unsigned)gCurrentFunction,
                    (unsigned)g_SquelchLost, (unsigned)rx_rssi());
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
    /* The scan wrote the chips under the coordinator: re-apply both lanes. */
    rf_dual_reapply();
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
        if (ScanFastEnabled())
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
    /* Put the receiving chip on the candidate (see rx_scan_commit()). */
    rx_scan_commit(gRxVfo->pRX->Frequency);
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
    if (scanTrace && scanFastLastFullTuneCandidate)
        uart_printf("TUNE %u on %s, 0x0C %04X\n",
                    (unsigned)gRxVfo->pRX->Frequency, rx_audio_is_4815() ? "4815" : "4829",
                    (unsigned)BK4819_ReadRegister(BK4819_REG_0C));
#endif

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
 * and the fast precheck stay in NextMemChannel(). */
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
        /* Put the receiving chip on the channel (see rx_scan_commit()). */
        rx_scan_commit(gRxVfo->pRX->Frequency);
#ifdef ENABLE_FEAT_F4HWN_SCAN_FASTER
        if (scanTrace)
            uart_printf("TUNE ch%u %u on %s, 0x0C %04X\n", (unsigned)(gNextMrChannel + 1u),
                        (unsigned)gRxVfo->pRX->Frequency, rx_audio_is_4815() ? "4815" : "4829",
                        (unsigned)BK4819_ReadRegister(BK4819_REG_0C));
#endif

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
