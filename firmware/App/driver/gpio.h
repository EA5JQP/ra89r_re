/* Thin GPIO helpers -- no HAL/LL dependency, register level only. */
#ifndef DRIVER_GPIO_H
#define DRIVER_GPIO_H

#include <stdbool.h>
#include <stdint.h>
#include "py32f4xx.h"
#include "board_pins.h"

/* Enable the AHB2 clock of a GPIO port (AHB2ENR IOPxxEN bits). */
void gpio_port_clock(GPIO_TypeDef *port);

/* Configure every pin set in mask as alternate function, push-pull,
 * high speed, with the given pull (0 = none, 1 = pull-up, 2 = pull-down). */
void gpio_config_af(GPIO_TypeDef *port, uint32_t mask, uint32_t af, uint32_t pull);

/* Configure every pin set in mask as a push-pull output (high speed). */
void gpio_config_output(GPIO_TypeDef *port, uint32_t mask);

/* As gpio_config_output, but with an explicit OSPEEDR value (0 low .. 3 very
 * high).  gpio_config_output() uses 3; the stock configures the companion bus
 * pins with 2 (high), so the battery driver matches that exactly. */
void gpio_config_output_ospeed(GPIO_TypeDef *port, uint32_t mask, uint32_t ospeed);

/* Configure every pin set in mask as a floating input. */
void gpio_config_input(GPIO_TypeDef *port, uint32_t mask);

/* Configure every pin set in mask as an analog input: no drive, no pull, no
 * alternate function.  This is the reset-default state of a pin, and it is what
 * an analog signal (an ADC input, or a key ladder) needs -- driving or pulling
 * such a line corrupts the level and, for a ladder, sinks current through it. */
void gpio_config_analog(GPIO_TypeDef *port, uint32_t mask);

static inline void gpio_set(GPIO_TypeDef *port, uint32_t mask)
{
    port->BSRR = mask;
}

static inline void gpio_clear(GPIO_TypeDef *port, uint32_t mask)
{
    port->BRR = mask;
}

static inline void gpio_write(GPIO_TypeDef *port, uint32_t mask, int value)
{
    if (value)
        port->BSRR = mask;
    else
        port->BRR = mask;
}

static inline uint32_t gpio_read(GPIO_TypeDef *port, uint32_t mask)
{
    return port->IDR & mask;
}

/* ---------------------------------------------------------------------------
 * The K1 application's hardware surface (see NOTICE).
 *
 * The imported K1 sources reach the hardware through these names, which in the
 * K1 tree are inline wrappers over its LL GPIO.  The RA89R is a different MCU
 * with a different pinout, so they are mapped here to this board's pins and to
 * this repo's drivers:
 *
 *   K1 (PY32F071)              RA89R (PY32F403)
 *   GPIO_PIN_PTT      PB10     the keypad's PTT (ADC ladder), plus PTT2 on PB9
 *   GPIO_PIN_BACKLIGHT PF8     PA5   (driver/backlight.c)
 *   GPIO_PIN_AUDIO_PATH PA8    PC13  (driver/audio_path.c)
 *   GPIO_PIN_FLASHLIGHT PC13   unmapped: this radio has no flashlight output
 *
 * The pin encoding is the K1's -- port offset in the top half, mask in the low
 * half -- so GPIO_MAKE_PIN/GPIO_PORT/GPIO_PIN_MASK keep working on call sites.
 */
#define GPIO_PORT_A_OFFSET   0x0000u
#define GPIO_PORT_B_OFFSET   0x0400u
#define GPIO_PORT_C_OFFSET   0x0800u

#define GPIO_MAKE_PIN(port_offset, pin_mask) \
    ((uint32_t)(((uint32_t)(port_offset) << 16) | (0xFFFFu & (pin_mask))))
#define GPIO_PORT(pin)       ((GPIO_TypeDef *)(0x48000000u + ((pin) >> 16)))
#define GPIO_PIN_MASK(pin)   (0xFFFFu & (pin))

#define GPIO_PIN_PTT          GPIO_MAKE_PIN(GPIO_PORT_B_OFFSET, KEYPAD_PTT2_PIN)
#define GPIO_PIN_BACKLIGHT    GPIO_MAKE_PIN(GPIO_PORT_A_OFFSET, BACKLIGHT_PIN)
#define GPIO_PIN_AUDIO_PATH   GPIO_MAKE_PIN(GPIO_PORT_C_OFFSET, AUDIO_PATH_PIN)
#define GPIO_PIN_FLASHLIGHT   GPIO_MAKE_PIN(GPIO_PORT_C_OFFSET, (1u << 13))

void GPIO_SetOutputPin(uint32_t Pin);
void GPIO_ResetOutputPin(uint32_t Pin);
void GPIO_TogglePin(uint32_t Pin);
bool GPIO_IsInputPinSet(uint32_t Pin);

void GPIO_EnableAudioPath(void);
void GPIO_DisableAudioPath(void);
void GPIO_TurnOnBacklight(void);
void GPIO_TurnOffBacklight(void);

/* PTT is not a plain GPIO on this radio: it is one of the keypad ladder's keys
 * (with PTT2 on PB9), so this reads the keypad reader. */
bool GPIO_IsPttPressed(void);

#endif /* DRIVER_GPIO_H */
