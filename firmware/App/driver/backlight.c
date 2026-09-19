#include "driver/backlight.h"

#include "board.h"
#include "driver/gpio.h"

static bool s_on;

/* Both lamp lines are on the same port (GPIOA) in this test build; see
 * BACKLIGHT_AUX_PIN in board_pins.h for why there are two. */
static void backlight_drive(bool on)
{
    gpio_write(BACKLIGHT_PORT, BACKLIGHT_PIN,
               (BACKLIGHT_ON_LEVEL ? on : !on) ? 1 : 0);
    gpio_write(BACKLIGHT_PORT, BACKLIGHT_AUX_PIN,
               (BACKLIGHT_AUX_ON_LEVEL ? on : !on) ? 1 : 0);
    s_on = on;
}

void BACKLIGHT_TurnOn(void)
{
    backlight_drive(true);
}

void BACKLIGHT_TurnOff(void)
{
    backlight_drive(false);
}

bool BACKLIGHT_IsOn(void)
{
    return s_on;
}

void BACKLIGHT_Init(void)
{
    gpio_config_output(BACKLIGHT_PORT, BACKLIGHT_PIN | BACKLIGHT_AUX_PIN);
    BACKLIGHT_TurnOn();
}
