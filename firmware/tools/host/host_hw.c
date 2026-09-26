/* Host implementations for the handful of hardware entry points the ported
 * application sources call (see tools/host/py32f4xx.h).  Preview tools link
 * this; the target links the real drivers instead. */
#include "py32f4xx.h"

#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/systick.h"
#include "driver/tx.h"
#include <string.h>

#include "audio.h"
#include "dcs.h"
#include "functions.h"
#include "driver/keypad.h"
#include "driver/spi_flash.h"

uint32_t SystemCoreClock = 8000000u;

void host_nvic_system_reset(void)
{
    /* A menu item that reboots the radio; a preview just stops here. */
}

/* Backlight: the drawing code switches it, the host only records it. */
static int s_host_backlight = 1;

void BACKLIGHT_Init(void) { s_host_backlight = 1; }
void BACKLIGHT_InitHardware(void) { BACKLIGHT_Init(); }
void BACKLIGHT_TurnOn(void) { s_host_backlight = 1; }
void BACKLIGHT_TurnOff(void) { s_host_backlight = 0; }
bool BACKLIGHT_IsOn(void) { return s_host_backlight != 0; }
void BACKLIGHT_SetBrightness(uint8_t b) { gBacklightBrightness = b; s_host_backlight = (b != 0); }
void BACKLIGHT_UpdateTickless(void) { }
void BACKLIGHT_Update(void) { }
uint16_t gBacklightCountdown_500ms;
uint8_t  gBacklightBrightness = 10;
const uint8_t value[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

/* RF: the real driver (App/driver/bk4819.c) owns these; the host preview
 * compiles no hardware, so it records the calls instead. */
void BK4819_SetFilterBandwidth(const BK4819_FilterBandwidth_t Bandwidth,
                               const bool weak_no_different)
{
    (void)Bandwidth;
    (void)weak_no_different;
}

void BK4819_SetRxAudioGain(void) { }

/* Keypad: the RA89R reads an ADC ladder, so the host stands in with a key the
 * preview sets by hand -- that is how the port's key loop (port_gui.c) is
 * exercised without a radio. */
static KEY_Code_t s_host_key = KEY_INVALID;

void host_set_key(KEY_Code_t key)
{
    s_host_key = key;
}

KEY_Code_t keypad_poll(void)
{
    return s_host_key;
}

void BK4819_DisableDTMF(void) { }

/* RF reads the VFO screen makes (driver/bk4819.c + the codeplug's calibration
 * table, which the storage layer will provide). */
uint16_t BK4819_GetRSSI(void) { return 0; }
int16_t  BK4819_GetRSSI_dBm(void) { return -120; }

/* More of the same: the menu's action path calls these, and the host compiles
 * no audio engine and no GPIO driver. */
void BK4819_ToggleGpioOut(BK4819_GPIO_PIN_t pin, bool enable) { (void)pin; (void)enable; }


/* Leftovers of the menu/battery path in the host build (their owners are the
 * codeplug, app/dtmf.c, app/scanner.c and ui/status.c). */


/* ---------------------------------------------------------------------------
 * A tiny in-RAM stand-in for the external SPI NOR flash, covering the two
 * sectors the port's storage uses (0x1FE000..0x1FFFFF, both empty on this
 * radio).  Program ANDs bits and erase sets 0xFF, like the real part, so the
 * host can exercise the settings blob round-trip and the write test.
 * ------------------------------------------------------------------------- */
#define HOST_FLASH_BASE 0x1FE000u
#define HOST_FLASH_SIZE 0x2000u
static uint8_t s_host_flash[HOST_FLASH_SIZE];

static int host_flash_at(uint32_t addr, uint32_t *off)
{
    if (addr < HOST_FLASH_BASE || addr >= HOST_FLASH_BASE + HOST_FLASH_SIZE)
        return 0;
    *off = addr - HOST_FLASH_BASE;
    return 1;
}

void spi_flash_init(void)
{
    if (s_host_flash[0] == 0 && s_host_flash[1] == 0)
        memset(s_host_flash, 0xFF, sizeof s_host_flash);
}

bool spi_flash_id(uint16_t *man_dev, uint32_t *jedec)
{
    if (man_dev)
        *man_dev = 0x8514;
    if (jedec)
        *jedec = 0x852015;
    return true;
}

uint32_t spi_flash_size(uint32_t jedec)
{
    (void)jedec;
    return 2u * 1024u * 1024u;
}

void spi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    while (len--) {
        uint32_t off;
        *buf++ = host_flash_at(addr, &off) ? s_host_flash[off] : 0xFFu;
        addr++;
    }
}

void spi_flash_sector_erase(uint32_t addr)
{
    uint32_t off;

    if (host_flash_at(addr, &off))
        memset(s_host_flash + off, 0xFF, SPI_FLASH_SECTOR_SIZE);
}

void spi_flash_program(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    while (len--) {
        uint32_t off;
        if (host_flash_at(addr, &off))
            s_host_flash[off] &= *buf;
        buf++;
        addr++;
    }
}

void spi_flash_write_enable(void) { }
void spi_flash_wait_ready(void) { }

/* ---------------------------------------------------------------------------
 * Stand-ins for the drivers the host does not build: the RF driver (our
 * driver/bk4819.c), the GPIO path helpers (driver/gpio.c), the SysTick delay and
 * the measured transmit chain (driver/tx.c).  Signatures match driver/bk4819.h.
 * ------------------------------------------------------------------------- */
void     BK4819_SetFrequency(uint32_t Frequency) { (void)Frequency; }
void     BK4819_SetAF(BK4819_AF_Type_t AF) { (void)AF; }
void     BK4819_SetAGC(bool enable) { (void)enable; }
void     BK4819_InitAGC(bool amModulation) { (void)amModulation; }
void     BK4819_SetupSquelch(uint8_t SquelchOpenRSSIThresh, uint8_t SquelchCloseRSSIThresh,
                             uint8_t SquelchOpenNoiseThresh, uint8_t SquelchCloseNoiseThresh,
                             uint8_t SquelchCloseGlitchThresh, uint8_t SquelchOpenGlitchThresh)
{
    (void)SquelchOpenRSSIThresh; (void)SquelchCloseRSSIThresh; (void)SquelchOpenNoiseThresh;
    (void)SquelchCloseNoiseThresh; (void)SquelchCloseGlitchThresh; (void)SquelchOpenGlitchThresh;
}
void     BK4819_SetupPowerAmplifier(const uint8_t bias, const uint32_t frequency)
{ (void)bias; (void)frequency; }
void     BK4819_SetCTCSSFrequency(uint32_t BaudRate) { (void)BaudRate; }
void     BK4819_SetTailDetection(const uint32_t freq_10Hz) { (void)freq_10Hz; }
void     BK4819_SetCDCSSCodeWord(uint32_t CodeWord) { (void)CodeWord; }
void     BK4819_SetCompander(const unsigned int mode) { (void)mode; }
void     BK4819_EnableVox(uint16_t Vox1Threshold, uint16_t Vox0Threshold)
{ (void)Vox1Threshold; (void)Vox0Threshold; }
void     BK4819_DisableVox(void) { }
void     BK4819_DisableScramble(void) { }
void     BK4819_EnableDTMF(void) { }
void     BK4819_SetRegValue(RegisterSpec s, uint16_t v) { (void)s; (void)v; }
void     BK4819_WriteRegister(BK4819_REGISTER_t Register, uint16_t Data)
{ (void)Register; (void)Data; }
uint16_t BK4819_ReadRegister(BK4819_REGISTER_t Register) { (void)Register; return 0; }
void     BK4819_PickRXFilterPathBasedOnFrequency(uint32_t Frequency) { (void)Frequency; }

void GPIO_EnableAudioPath(void) { }
void GPIO_DisableAudioPath(void) { }

void systick_delay_ms(uint32_t ms) { (void)ms; }

bool tx_active(void) { return false; }
void tx_start(uint32_t freq_10hz, tx_source_t source) { (void)freq_10hz; (void)source; }
void tx_stop(void) { }

/* More of the RF driver's entry points the app core calls. */
void BK4819_Conditional_RX_TurnOn_and_GPIO6_Enable(void) { }
void BK4819_EnterDTMF_TX(bool bLocalLoopback) { (void)bLocalLoopback; }
void BK4819_ExitDTMF_TX(bool bKeepOpen) { (void)bKeepOpen; }
void BK4819_ExitSubAu(void) { }
void BK4819_EnterTxMute(void) { }
void BK4819_ExitTxMute(void) { }
void BK4819_TurnsOffTones_TurnsOnRX(void) { }
void BK4819_PlayRoger(BK4819_FilterBandwidth_t Bandwidth) { (void)Bandwidth; }
void BK4819_EnableTXLink(void) { }
void BK4819_EnableRXLink(void) { }

void BK4819_PlayDTMFString(const char * pString, bool bDelayFirst, uint16_t FirstCodePersistTime, uint16_t HashCodePersistTime, uint16_t CodePersistTime, uint16_t CodeInternalTime) { (void)pString; (void)bDelayFirst; (void)FirstCodePersistTime; (void)HashCodePersistTime; (void)CodePersistTime; (void)CodeInternalTime; }
void BK4819_PlaySingleTone(const unsigned int tone_Hz, const unsigned int delay, const unsigned int level, const bool play_speaker) { (void)tone_Hz; (void)delay; (void)level; (void)play_speaker; }
void BK4819_PrepareTransmit(void) { }
void BK4819_Sleep(void) { }
void BK4819_PlayDTMF(char Code) { (void)Code; }
void BK4819_PlayTone(uint16_t Frequency, bool bTuningGainSwitch)
{ (void)Frequency; (void)bTuningGainSwitch; }
void BK4819_PlayToneRaw(const unsigned int tone_Hz, const unsigned int delay)
{ (void)tone_Hz; (void)delay; }

/* driver/bk4819.c owns this; the host needs it for radio.c/app.c. */
bool gRxIdleMode;
