/* Host implementations for the handful of hardware entry points the ported
 * application sources call (see tools/host/py32f4xx.h).  Preview tools link
 * this; the target links the real drivers instead. */
#include "py32f4xx.h"

#include "driver/backlight.h"
#include "driver/bk4819.h"

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
