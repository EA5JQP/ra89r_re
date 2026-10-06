#include "driver/audio_path.h"

#include "board.h"
#include "board_pins.h"
#include "driver/gpio.h"

static bool s_on;
static bool s_bt_exclusive;

void audio_path_drive(int on)
{
    const bool effective_on = (on != 0) && !s_bt_exclusive;

    gpio_port_clock(AUDIO_PATH_PORT);
    gpio_config_output(AUDIO_PATH_PORT, AUDIO_PATH_PIN);
    gpio_write(AUDIO_PATH_PORT, AUDIO_PATH_PIN, effective_on ? 1 : 0);
    s_on = effective_on;
}

void audio_path_set_bt_exclusive(bool enabled)
{
    s_bt_exclusive = enabled;
    if (enabled)
        audio_path_drive(0);
}

void audio_path_init(void)
{
    audio_path_drive(1);
}

bool audio_path_is_on(void)
{
    return s_on;
}
