#include "driver/backlight.h"

#include "board.h"
#include "driver/gpio.h"

static bool s_on;

void BACKLIGHT_TurnOn(void)
{
    gpio_write(BACKLIGHT_PORT, BACKLIGHT_PIN, BACKLIGHT_ON_LEVEL ? 1 : 0);
    s_on = true;
}

void BACKLIGHT_TurnOff(void)
{
    gpio_write(BACKLIGHT_PORT, BACKLIGHT_PIN, BACKLIGHT_ON_LEVEL ? 0 : 1);
    s_on = false;
}

bool BACKLIGHT_IsOn(void)
{
    return s_on;
}

void BACKLIGHT_Init(void)
{
    gpio_port_clock(BACKLIGHT_PORT);
    gpio_config_output(BACKLIGHT_PORT, BACKLIGHT_PIN);
    BACKLIGHT_TurnOn();
}

/* ---------------------------------------------------------------------------
 * K1-compatible entry points (see backlight.h).
 *
 * The K1 uses these to drive a brightness staircase through the panel's
 * backlight PWM.  On the RA89R the backlight is a plain GPIO (and the stock
 * dims it through DAC_OUT2, which this driver does not implement yet), so the
 * brightness is kept as state and the panel stays on: the imported K1 sources
 * compile and run, the dimming is simply not there.
 * ------------------------------------------------------------------------- */

/* The K1's brightness steps, kept so the imported sources link; the values are
 * its own (0 = off .. 10 = brightest). */
const uint8_t value[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

uint16_t gBacklightCountdown_500ms;
uint8_t  gBacklightBrightness = 10;
uint8_t  gBacklightBrightnessOld = 10;

void BACKLIGHT_InitHardware(void)
{
    BACKLIGHT_Init();
}

void BACKLIGHT_SetBrightness(uint8_t brightness)
{
    if (brightness > 10u)
        brightness = 10u;
    gBacklightBrightness = brightness;
    if (brightness == 0u)
        BACKLIGHT_TurnOff();
    else
        BACKLIGHT_TurnOn();
}

void BACKLIGHT_UpdateTickless(void)
{
    if (gBacklightCountdown_500ms > 0u)
        gBacklightCountdown_500ms--;
}

void BACKLIGHT_Update(void)
{
    BACKLIGHT_UpdateTickless();
}
