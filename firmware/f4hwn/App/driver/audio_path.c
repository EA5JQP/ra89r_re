#include "driver/audio_path.h"

#include "board.h"
#include "board_pins.h"
#include "driver/gpio.h"

static bool s_on;

void audio_path_drive(int on)
{
    gpio_port_clock(AUDIO_PATH_PORT);
    gpio_config_output(AUDIO_PATH_PORT, AUDIO_PATH_PIN);
    gpio_write(AUDIO_PATH_PORT, AUDIO_PATH_PIN, on ? 1 : 0);
    s_on = on != 0;
}

void audio_path_init(void)
{
    audio_path_drive(1);
}

bool audio_path_is_on(void)
{
    return s_on;
}
