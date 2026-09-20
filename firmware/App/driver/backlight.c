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
