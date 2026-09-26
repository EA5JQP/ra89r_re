/* Host stand-ins for the RF register layer's remaining entry points.
 *
 * The preview tools do not link the real `driver/bk4819.c` (it needs a target
 * transport); `tools/host/host_hw.c` covers the calls the screens make and this
 * file covers the calls the *application loop* makes -- key actions, the
 * scanner and the tone players.  Nothing here has a radio behind it, so every
 * read reports "nothing there" and every write is dropped.
 *
 * The tools that assert on register behaviour (`tools/test_rf.c`) link the real
 * driver instead and must not link this file.
 */
#include "driver/bk4819.h"

void BK4819_Init(void) { }
void BK4819_WriteU8(uint8_t Data) { (void)Data; }
void BK4819_WriteU16(uint16_t Data) { (void)Data; }
bool BK4819_IsGpioOutSet(BK4819_GPIO_PIN_t Pin) { (void)Pin; return false; }
void BK4819_RX_TurnOn(void) { }
void BK4819_EnableScramble(uint8_t Type) { (void)Type; }
bool BK4819_CompanderEnabled(void) { return false; }
void BK4819_PrepareToPlayTone(bool bTuningGainSwitch) { (void)bTuningGainSwitch; }
void BK4819_ResetFSK(void) { }
void BK4819_Idle(void) { }
void BK4819_EnterBypass(void) { }
void BK4819_EnterRaw(void) { }
void BK4819_ExitBypass(void) { }
void BK4819_TxOn_Beep(void) { }
void BK4819_TransmitTone(bool bLocalLoopback, uint32_t Frequency)
{ (void)bLocalLoopback; (void)Frequency; }
void BK4819_GenTail(uint8_t Tail) { (void)Tail; }
void BK4819_PlayCDCSSTail(void) { }
void BK4819_PlayCTCSSTail(void) { }
int8_t BK4819_GetRxGain_dB(void) { return 0; }
uint8_t BK4819_GetGlitchIndicator(void) { return 0; }
uint8_t BK4819_GetExNoiceIndicator(void) { return 0; }
uint16_t BK4819_GetVoiceAmplitudeOut(void) { return 0; }
uint8_t BK4819_GetAfTxRx(void) { return 0; }
bool BK4819_GetFrequencyScanResult(uint32_t *pFrequency)
{ if (pFrequency) *pFrequency = 0; return false; }
BK4819_CssScanResult_t BK4819_GetCxCSSScanResult(uint32_t *pCdcssFreq, uint16_t *pCtcssFreq)
{
    if (pCdcssFreq) *pCdcssFreq = 0;
    if (pCtcssFreq) *pCtcssFreq = 0;
    return BK4819_CSS_RESULT_NOT_FOUND;
}
void BK4819_SetFrequencyScan(bool enable) { (void)enable; }
void BK4819_SetScanFrequency(uint32_t Frequency) { (void)Frequency; }
void BK4819_Disable(void) { }
void BK4819_StopScan(void) { }
uint8_t BK4819_GetCTCShift(void) { return 0; }
void BK4819_SendFSKData(uint16_t *pData) { (void)pData; }
void BK4819_PrepareFSKReceive(void) { }
void BK4819_Enable_AfDac_DiscMode_TxDsp(void) { }
void BK4819_GetVoxAmp(uint16_t *pResult) { if (pResult) *pResult = 0; }
void BK4819_SetScrambleFrequencyControlWord(uint32_t Frequency) { (void)Frequency; }
void BK4819_PlayDTMFEx(bool bLocalLoopback, char Code)
{ (void)bLocalLoopback; (void)Code; }
void BK4819_SetRxAudioGains(uint8_t volume_gain, uint8_t dac_gain)
{ (void)volume_gain; (void)dac_gain; }
void BK4819_SetRogerMode(uint8_t mode) { (void)mode; }

/* The driver hands its audio-path switch to the board layer through this; on
 * the host nothing consumes it, so it is only remembered. */
static void (*s_audio_path_cb)(int on);

void BK4819_SetAudioPathCallback(void (*cb)(int on))
{
    s_audio_path_cb = cb;
}

void host_bk4819_audio_path(int on)
{
    if (s_audio_path_cb)
        s_audio_path_cb(on);
}
