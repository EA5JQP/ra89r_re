#include "driver/led.h"

#include "board.h"
#include "driver/gpio.h"

/* Provisional: one press of 'l' per state on the radio will say which pin is
 * which colour, and then this is a one-line correction. */
#define LED_RED_PIN   LED_PIN_B
#define LED_GREEN_PIN LED_PIN_A

static led_colour_t s_colour = LED_OFF;

static void led_drive(void)
{
    int red = (s_colour == LED_RED || s_colour == LED_BOTH) ? 1 : 0;
    int green = (s_colour == LED_GREEN || s_colour == LED_BOTH) ? 1 : 0;

    gpio_write(LED_PORT, LED_RED_PIN, LED_ON_LEVEL ? red : !red);
    gpio_write(LED_PORT, LED_GREEN_PIN, LED_ON_LEVEL ? green : !green);
}

void led_set(led_colour_t colour)
{
    if (colour >= LED_STATE_COUNT)
        colour = LED_OFF;
    s_colour = colour;
    led_drive();
}

led_colour_t led_get(void)
{
    return s_colour;
}

const char *led_name(led_colour_t colour)
{
    switch (colour) {
    case LED_RED:   return "red";
    case LED_GREEN: return "green";
    case LED_BOTH:  return "red+green";
    default:        return "off";
    }
}

void led_drive_pins(uint32_t mask)
{
    uint32_t all = LED_PIN_A | LED_PIN_B | LED_AUX_PIN;

    gpio_write(LED_PORT, all & ~mask, 0);
    gpio_write(LED_PORT, mask, 1);
}

void led_init(void)
{
    gpio_port_clock(LED_PORT);
    gpio_config_output(LED_PORT, LED_PIN_A | LED_PIN_B | LED_AUX_PIN);
    led_set(LED_OFF);
}
