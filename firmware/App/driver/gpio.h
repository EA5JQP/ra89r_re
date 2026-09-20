/* Thin GPIO helpers -- no HAL/LL dependency, register level only. */
#ifndef DRIVER_GPIO_H
#define DRIVER_GPIO_H

#include <stdint.h>
#include "py32f4xx.h"

/* Enable the AHB2 clock of a GPIO port (AHB2ENR IOPxxEN bits). */
void gpio_port_clock(GPIO_TypeDef *port);

/* Configure every pin set in mask as alternate function, push-pull,
 * high speed, with the given pull (0 = none, 1 = pull-up, 2 = pull-down). */
void gpio_config_af(GPIO_TypeDef *port, uint32_t mask, uint32_t af, uint32_t pull);

/* Configure every pin set in mask as a push-pull output (high speed). */
void gpio_config_output(GPIO_TypeDef *port, uint32_t mask);

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

#endif /* DRIVER_GPIO_H */
