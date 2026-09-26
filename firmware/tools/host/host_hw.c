/* Host implementations for the handful of hardware entry points the ported
 * application sources call (see tools/host/py32f4xx.h).  Preview tools link
 * this; the target links the real drivers instead. */
#include "py32f4xx.h"

#include "driver/backlight.h"
#include "driver/bk4819.h"
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
const uint8_t gEEPROM_RSSI_CALIB[7][8] = {{0}};

/* More of the same: the menu's action path calls these, and the host compiles
 * no audio engine and no GPIO driver. */
void AUDIO_PlayBeep(BEEP_Type_t beep) { (void)beep; }
void FUNCTION_Select(FUNCTION_Type_t function) { (void)function; }
void BK4819_ToggleGpioOut(BK4819_GPIO_PIN_t pin, bool enable) { (void)pin; (void)enable; }

uint8_t gBacklightBrightnessOld;

/* Leftovers of the menu/battery path in the host build (their owners are the
 * codeplug, app/dtmf.c, app/scanner.c and ui/status.c). */
uint8_t        gBacklightTimeOriginal;
uint8_t        gDTMF_InputBox_Index;
uint8_t        gReducedService;
uint8_t        gScanCssResultCode;
DCS_CodeType_t gScanCssResultType;
bool           gScanUseCssResult;
void UI_DisplayBattery(uint8_t Level, uint8_t blink) { (void)Level; (void)blink; }


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
