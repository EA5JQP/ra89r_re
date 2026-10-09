/* Host implementations for the handful of hardware entry points the ported
 * application sources call (see tools/host/py32f4xx.h).  Preview tools link
 * this; the target links the real drivers instead. */
#include "py32f4xx.h"

#include "driver/backlight.h"
#include "driver/bk1080.h"
#include "driver/bk4815.h"
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/pa.h"
#include "driver/rx.h"
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

/* Backlight: the real driver is App/driver/backlight.c, which the preview links
 * so its timeout is exercised rather than stubbed -- it is armed in 500 ms
 * units and decremented only by APP_TimeSlice500ms(), and a stub hides exactly
 * the mistake of decrementing it on the 10 ms slice.  All it needs from the
 * hardware is one GPIO write. */
/* The scratch the device-header double points the GPIO ports at (see
 * tools/host/py32f4xx.h), plus the two GPIO configuration calls the backlight
 * driver makes on its way up.  Its pin writes go through the inline
 * `gpio_write`, which lands here and is simply recorded. */
GPIO_TypeDef host_gpio_scratch[6];

/* `driver/pa.c` names TIM1; a preview never calls `pa_init()`, so it only has to
 * exist for the reference to link. */
TIM_TypeDef host_tim_scratch;

/* `driver/backlight.c` is host-tested through `BACKLIGHT_InitHardware()`, so
 * its TIM7/DMA/SYSCFG/RCC registers are scratch too. */
TIM_TypeDef         host_tim7_scratch;
DMA_TypeDef         host_dma_scratch;
DMA_Channel_TypeDef host_dma_ch2_scratch;
SYSCFG_TypeDef      host_syscfg_scratch;
RCC_TypeDef         host_rcc_scratch;

void gpio_port_clock(GPIO_TypeDef *port) { (void)port; }
void gpio_config_output(GPIO_TypeDef *port, uint32_t mask) { (void)port; (void)mask; }

/* RF: the real driver (App/driver/bk4819.c) owns these; the host preview
 * compiles no hardware, so it records the calls instead. */
void BK4819_SetFilterBandwidth(const BK4819_FilterBandwidth_t Bandwidth,
                               const bool weak_no_different)
{
    (void)Bandwidth;
    (void)weak_no_different;
}

void BK4819_SetRxAudioGain(void) { }

/* FM broadcast: the host compiles no I2C bus, so the K1's BK1080 API is a set of
 * no-ops here.  The FM screen's layout and the FM state machine still run (the
 * ported app/fm.c is linked), which is what the preview exercises; the chip
 * itself is only reachable on the radio (docs/ra89r_bk1080.md). */
uint16_t BK1080_BaseFrequency;
uint16_t BK1080_FrequencyDeviation;
void     BK1080_Init0(void) { }
void     BK1080_Init(uint16_t Frequency, uint8_t band) { (void)Frequency; (void)band; }
uint16_t BK1080_ReadRegister(BK1080_Register_t Register) { (void)Register; return 0; }
void     BK1080_WriteRegister(BK1080_Register_t Register, uint16_t Value) { (void)Register; (void)Value; }
void     BK1080_Mute(bool Mute) { (void)Mute; }
uint16_t BK1080_GetFreqLoLimit(uint8_t band) { static const uint16_t lim[] = {875, 760, 760, 640}; return lim[band % 4]; }
uint16_t BK1080_GetFreqHiLimit(uint8_t band) { static const uint16_t lim[] = {1080, 1080, 900, 760}; return lim[band % 4]; }
void     BK1080_SetFrequency(uint16_t frequency, uint8_t band) { (void)frequency; (void)band; }
void     BK1080_GetFrequencyDeviation(uint16_t Frequency) { (void)Frequency; }

/* The FM feature now brings the BK1080's two-wire bus up itself (the real
 * driver's bk1080_init() -> i2c_bus_init()); the host compiles no bus. */
void     bk1080_init(void) { }

/* driver/rx.c's FM gate.  The preview never runs rx_service(), so the host only
 * has to satisfy the reference. */
void     rx_set_fm_active(bool active) { (void)active; }

/* The dual-scan path in app/chFrScanner.c references the BK4815, the shared
 * band path and the RX scan-source override.  The preview does not run the RF
 * scan; these stand in so it links, and the override is recorded so a preview
 * check can confirm it is cleared on stop. */
void     pa_select_band(uint32_t freq_10hz) { (void)freq_10hz; }
void     bk4815_set_frequency(uint32_t freq_10hz, bool tx) { (void)freq_10hz; (void)tx; }
void     bk4815_write_reg(uint8_t reg, uint16_t value) { (void)reg; (void)value; }
uint16_t bk4815_read_rssi(void) { return 0; }

static int s_host_scan_source;

void             rx_set_scan_source_override(rx_scan_source_t source) { s_host_scan_source = (int)source; }
void             rx_clear_scan_source_override(void) { s_host_scan_source = 0; }
rx_scan_source_t rx_scan_source_override(void) { return (rx_scan_source_t)s_host_scan_source; }
int              host_rx_scan_source(void) { return s_host_scan_source; }

/* Keypad: the RA89R reads an ADC ladder, so the host stands in with a key the
 * preview sets by hand -- that is how the port's key loop (port_gui.c) is
 * exercised without a radio. */
static KEY_Code_t s_host_key  = KEY_INVALID;
static bool       s_host_ptt2 = false;

void host_set_key(KEY_Code_t key)
{
    s_host_key = key;
}

void host_set_ptt2(bool pressed)
{
    s_host_ptt2 = pressed;
}

KEY_Code_t keypad_poll(void)
{
    return s_host_key;
}

/* PB9: the level as read, low = pressed (see driver/keypad.c). */
bool keypad_ptt2_level(void)
{
    return !s_host_ptt2;
}

/* `helper/boot.c` prints what it sampled and names the keys; the preview links
 * neither the UART nor the real keypad reader. */
const char *keypad_name(KEY_Code_t key) { (void)key; return "?"; }
void uart_printf(const char *fmt, ...) { (void)fmt; }

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
 * A tiny in-RAM stand-in for the external SPI NOR flash, covering three
 * regions: the codeplug (0x0000..0x1FFF, which the preview preloads with a
 * factory-shaped image), the K1 image (0x4000..0x13FFF, empty until the port's
 * import writes it) and the two sectors the port's storage uses
 * (0x1FE000..0x1FFFFF, both empty on this radio).  Program ANDs bits and erase
 * sets 0xFF, like the real part, so the host can exercise the import, the
 * channel decoder, the settings blob round-trip and the write test.
 * ------------------------------------------------------------------------- */
#define HOST_FLASH_BASE 0x1FE000u
#define HOST_FLASH_SIZE 0x2000u
static uint8_t s_host_flash[HOST_FLASH_SIZE];

#define HOST_CP_BASE 0x0000u
#define HOST_CP_SIZE 0x2000u
static uint8_t s_host_codeplug[HOST_CP_SIZE];

/* The K1 application's own EEPROM image: names 0x4000, attributes 0x8000,
 * channel records 0x9000, calibration 0x100C0.  This is the region the port's
 * one-time import writes (driver/py25q16.c) and the K1 runtime then reads; the
 * host must model it or the preview would exercise a runtime nobody runs. */
#define HOST_IMG_BASE 0x04000u
#define HOST_IMG_SIZE 0x10000u
static uint8_t s_host_image[HOST_IMG_SIZE];

static int host_flash_at(uint32_t addr, uint32_t *off)
{
    if (addr >= HOST_CP_BASE && addr < HOST_CP_BASE + HOST_CP_SIZE) {
        *off = addr - HOST_CP_BASE;
        return 1;
    }
    if (addr >= HOST_IMG_BASE && addr < HOST_IMG_BASE + HOST_IMG_SIZE) {
        *off = addr - HOST_IMG_BASE;
        return 3;
    }
    if (addr < HOST_FLASH_BASE || addr >= HOST_FLASH_BASE + HOST_FLASH_SIZE)
        return 0;
    *off = addr - HOST_FLASH_BASE;
    return 2;
}

static uint8_t *host_flash_ptr(uint32_t addr)
{
    uint32_t off;
    const int which = host_flash_at(addr, &off);

    if (which == 1)
        return &s_host_codeplug[off];
    if (which == 2)
        return &s_host_flash[off];
    if (which == 3)
        return &s_host_image[off];
    return 0;
}

/* A codeplug shaped like the one this radio shipped with: five channels with
 * the stock's 21-byte records (rx, tx, rx tone, tx tone, three flag bytes, six
 * characters of name) and both bitmaps marking those five channels.  The last
 * is deliberately below the 134 MHz split, so the dual-scan lane rule (the
 * BK4815 is only used above it) can be exercised through the real cursor. */
static void host_codeplug_defaults(void)
{
    static const struct {
        uint32_t rx;
        uint32_t tx;
        uint16_t rx_tone;              /* 0x0FFF = none */
        uint16_t tx_tone;
        const char *name;
    } channels[5] = {
        { 14497500u, 14497500u, 0x0FFFu, 0x0FFFu, "CH-01 " },
        { 14575000u, 14575000u, 0x0FFFu, 0x0FFFu, "CH-02 " },
        /* 885 = CTCSS 88.5 Hz; 0x0013 + bit 15 = DCS 023 inverted. */
        { 43037500u, 43037500u, 0x0375u, 0x8013u, "CH-03 " },
        { 43865000u, 43865000u, 0x0FFFu, 0x0FFFu, "CH-04 " },
        /* Below the stock's 134 MHz split: a BK4829-only scan candidate. */
        {  5000000u,  5000000u, 0x0FFFu, 0x0FFFu, "CH-05 " },
    };
    unsigned int i;

    memset(s_host_codeplug, 0xFF, sizeof s_host_codeplug);

    for (i = 0; i < 5; i++) {
        uint8_t *record = s_host_codeplug + i * 21u;

        record[0] = (uint8_t)(channels[i].rx);
        record[1] = (uint8_t)(channels[i].rx >> 8);
        record[2] = (uint8_t)(channels[i].rx >> 16);
        record[3] = (uint8_t)(channels[i].rx >> 24);
        record[4] = (uint8_t)(channels[i].tx);
        record[5] = (uint8_t)(channels[i].tx >> 8);
        record[6] = (uint8_t)(channels[i].tx >> 16);
        record[7] = (uint8_t)(channels[i].tx >> 24);
        record[8] = (uint8_t)(channels[i].rx_tone);
        record[9] = (uint8_t)(channels[i].rx_tone >> 8);
        record[10] = (uint8_t)(channels[i].tx_tone);
        record[11] = (uint8_t)(channels[i].tx_tone >> 8);
        record[12] = 0x00;             /* flags A: wide, high power */
        record[13] = 0x00;             /* flags B */
        record[14] = 0x04;             /* flags C: 12.5 kHz step */
        memcpy(record + 15, channels[i].name, 6);
    }

    /* 7936: channel used; 7968: scan allow.  Both mark the first five. */
    s_host_codeplug[7936] = 0x1Fu;
    s_host_codeplug[7968] = 0x1Fu;
}

void spi_flash_init(void)
{
    static bool initialised;

    if (!initialised) {
        memset(s_host_flash, 0xFF, sizeof s_host_flash);
        memset(s_host_image, 0xFF, sizeof s_host_image);
        host_codeplug_defaults();
        initialised = true;
    }
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
        const uint8_t *p = host_flash_ptr(addr);

        *buf++ = (p != 0) ? *p : 0xFFu;
        addr++;
    }
}

static unsigned s_host_erase_count;

unsigned host_flash_erase_count(void)
{
    return s_host_erase_count;
}

void spi_flash_sector_erase(uint32_t addr)
{
    uint8_t *p = host_flash_ptr(addr);

    s_host_erase_count++;
    if (p != 0)
        memset(p, 0xFF, SPI_FLASH_SECTOR_SIZE);
}

void spi_flash_program(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    while (len--) {
        uint8_t *p = host_flash_ptr(addr);

        if (p != 0)
            *p &= *buf;
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
static int s_host_last_af = -1;
void     BK4819_SetAF(BK4819_AF_Type_t AF) { s_host_last_af = (int)AF; }
int      host_bk4819_last_af(void) { return s_host_last_af; }
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

/* The amplifier enable (PC13).  The beeper's DAC tone only reaches the speaker
 * through it, and the idle receive path leaves it off, so a beep has to turn it
 * on.  The previews track it so that can be asserted (see tools/preview_k1.c). */
static int s_host_audio_path;

void GPIO_EnableAudioPath(void) { s_host_audio_path = 1; }
void GPIO_DisableAudioPath(void) { s_host_audio_path = 0; }

bool host_audio_path_is_on(void) { return s_host_audio_path != 0; }

void systick_delay_ms(uint32_t ms) { (void)ms; }
void SYSTICK_DelayUs(uint32_t us) { (void)us; }

bool tx_active(void) { return false; }
void tx_start(uint32_t freq_10hz, uint8_t power, tx_source_t source) { (void)freq_10hz; (void)power; (void)source; }
void tx_stop(void) { }
void tx_poll_ptt(void) { }

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

/* The receive-side decoders the application polls every slice.  Nothing in the
 * host has a carrier, so they have nothing to report. */
uint8_t BK4819_GetCTCType(void) { return BK4819_CSS_RESULT_NOT_FOUND; }
uint8_t BK4819_GetCDCSSCodeType(void) { return BK4819_CSS_RESULT_NOT_FOUND; }
uint8_t BK4819_GetDTMF_5TONE_Code(void) { return 0; }

/* driver/gpio.c owns these; on this radio PTT is the measured transmit chain
 * rather than a GPIO the K1 reads (see port_gui.c), so the host says "not
 * pressed" exactly as the target does. */
bool GPIO_IsPttPressed(void) { return false; }
void GPIO_TogglePin(uint32_t Pin) { (void)Pin; }

/* driver/bk4819.c owns this; the host needs it for radio.c/app.c. */
bool gRxIdleMode;
