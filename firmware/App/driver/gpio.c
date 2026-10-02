#include "driver/gpio.h"

/* Register offsets/bit meanings follow the vendor CMSIS header
 * (PY32F4xx_Firmware/Drivers/CMSIS/Device/PUYA/PY32F403/Include/py32f403xD.h):
 * GPIO MODER 0x00, OTYPER 0x04, OSPEEDR 0x08, PUPDR 0x0C, IDR 0x10, ODR 0x14,
 * BSRR 0x18, LCKR 0x1C, AFRL/AFRH 0x20/0x24, BRR 0x28.
 * GPIO port clocks live in RCC->AHB2ENR (IOPAEN = bit 2, IOPBEN = bit 3, ...).
 */

void gpio_port_clock(GPIO_TypeDef *port)
{
    uint32_t bit;

    if (port == GPIOA)
        bit = RCC_AHB2ENR_IOPAEN;
    else if (port == GPIOB)
        bit = RCC_AHB2ENR_IOPBEN;
    else if (port == GPIOC)
        bit = RCC_AHB2ENR_IOPCEN;
    else if (port == GPIOD)
        bit = RCC_AHB2ENR_IOPDEN;
    else
        bit = RCC_AHB2ENR_IOPEEN;

    RCC->AHB2ENR |= bit;
    (void)RCC->AHB2ENR;                 /* clock enable needs a read back */
}

static void configure(GPIO_TypeDef *port, uint32_t mask, uint32_t mode,
                      uint32_t otype, uint32_t ospeed, uint32_t pupd, uint32_t af)
{
    uint32_t pin;

    gpio_port_clock(port);

    for (pin = 0; pin < 16u; pin++) {
        uint32_t bit = 1u << pin;
        uint32_t shift2 = pin * 2u;
        uint32_t shift4 = (pin & 7u) * 4u;
        uint32_t idx = pin >> 3;

        if (!(mask & bit))
            continue;

        port->MODER = (port->MODER & ~(3u << shift2)) | (mode << shift2);
        port->OTYPER = (port->OTYPER & ~bit) | (otype << pin);
        port->OSPEEDR = (port->OSPEEDR & ~(3u << shift2)) | (ospeed << shift2);
        port->PUPDR = (port->PUPDR & ~(3u << shift2)) | (pupd << shift2);
        port->AFR[idx] = (port->AFR[idx] & ~(0xFu << shift4)) | (af << shift4);
    }
}

void gpio_config_af(GPIO_TypeDef *port, uint32_t mask, uint32_t af, uint32_t pull)
{
    configure(port, mask, 2u /* AF */, 0u /* push-pull */, 3u /* very high */,
              pull, af);
}

void gpio_config_output(GPIO_TypeDef *port, uint32_t mask)
{
    configure(port, mask, 1u /* output */, 0u, 3u, 0u, 0u);
}

void gpio_config_input(GPIO_TypeDef *port, uint32_t mask)
{
    configure(port, mask, 0u /* input */, 0u, 0u, 0u, 0u);
}

void gpio_config_analog(GPIO_TypeDef *port, uint32_t mask)
{
    configure(port, mask, 3u /* analog */, 0u, 0u, 0u, 0u);
}

/* ---------------------------------------------------------------------------
 * The K1 application's hardware surface (see driver/gpio.h).  The logical pins
 * are this board's, and the two "path" lines go through this repo's drivers so
 * there is one owner per pin.
 * ------------------------------------------------------------------------- */
#include "driver/audio_path.h"
#include "driver/backlight.h"
#include "driver/keypad.h"

void GPIO_SetOutputPin(uint32_t Pin)
{
    GPIO_PORT(Pin)->BSRR = GPIO_PIN_MASK(Pin);
}

void GPIO_ResetOutputPin(uint32_t Pin)
{
    GPIO_PORT(Pin)->BRR = GPIO_PIN_MASK(Pin);
}

void GPIO_TogglePin(uint32_t Pin)
{
    GPIO_PORT(Pin)->ODR ^= GPIO_PIN_MASK(Pin);
}

bool GPIO_IsInputPinSet(uint32_t Pin)
{
    return (GPIO_PORT(Pin)->IDR & GPIO_PIN_MASK(Pin)) != 0u;
}

void GPIO_EnableAudioPath(void)
{
    audio_path_drive(1);
}

void GPIO_DisableAudioPath(void)
{
    audio_path_drive(0);
}

void GPIO_TurnOnBacklight(void)
{
    BACKLIGHT_TurnOn();
}

void GPIO_TurnOffBacklight(void)
{
    BACKLIGHT_TurnOff();
}

/* The port owns PTT: port_gui.c reads the keypad and drives the *measured*
 * transmit chain (driver/tx.c).  The K1's own PTT path -- CheckKeys() ->
 * GENERIC_Key_PTT -> FUNCTION_Transmit -> RADIO_SetTxParameters -- is the K1's
 * chip sequence, which on this radio still needs comparing against the stock
 * (docs/ra89r_rfpath.md), so this stays false and CheckKeys() does not see PTT.
 * When that comparison lands, this returns the keypad read below. */
bool GPIO_IsPttPressed(void)
{
    return false;

    /* KEY_Code_t key = keypad_poll();
     * return key == KEY_PTT || key == KEY_PTT2; */
}
