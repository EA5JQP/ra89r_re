/* Host stand-in for the status LED driver.
 *
 * `driver/bk4819.c` mirrors the chip's two GPIO outputs onto the MCU LED
 * (docs/ra89r_led.md), so linking it on a PC drags in `driver/led.c` -- which needs
 * the target's GPIO registers.  The tools that only test the RF register layers
 * (tools/test_rf.c) link these two no-ops instead; the preview tools that draw
 * screens link the real driver.
 */
#include "driver/led.h"

static led_colour_t s_host_led = LED_OFF;

void led_init(void)
{
    s_host_led = LED_OFF;
}

void led_set(led_colour_t colour)
{
    s_host_led = colour;
}

led_colour_t led_get(void)
{
    return s_host_led;
}
