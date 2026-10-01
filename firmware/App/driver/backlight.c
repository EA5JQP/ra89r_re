/* LCD backlight -- the K1/F4HWN driver on the RA89R.
 *
 * Copyright 2025 muzkr https://github.com/muzkr
 * Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * ---------------------------------------------------------------------------
 * RA89R adaptation (see NOTICE, docs/ra89r_led.md and
 * docs/superpowers/specs/2026-10-01-backlight-k1-port-design.md):
 *
 * The backlight is `GPIOA` pin 5, active high.  The stock dims it through
 * `DAC_OUT2`; this port keeps the K1's method instead -- a TIM7 update event
 * driving DMA1 channel 2 to write duty words into `GPIOA->BSRR`, 32 words per
 * 4 kHz period, so the pin's duty is level/32.  PA5 has no timer-output
 * alternate function, which is exactly why the K1 toggles it by DMA rather than
 * through a timer channel.
 *
 * Everything here is register level (no HAL/LL), following `driver/keypad.c`'s
 * DMA style.  The K1's `GPIO_TurnOn/OffBacklight()` calls are replaced by the
 * direct `gpio_write()` below: this repo's `driver/gpio.c` routes those two back
 * into `BACKLIGHT_TurnOn/Off()`, which would recurse.
 *
 * The `ENABLE_FEAT_F4HWN_SLEEP` countdown is not compiled: that feature is off.
 */
#include "driver/backlight.h"

#include "audio.h"
#include "board.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/system.h"
#include "misc.h"
#include "settings.h"

/* The K1's PWM: 4 kHz, 32 levels.  ARR = 48 MHz / 4000 / 32 - 1 = 374. */
#define BL_PWM_FREQ   4000u
#define BL_LEVELS     32u

#define DUTY_CYCLE_ON_VALUE  ((uint32_t)BACKLIGHT_PIN)
#define DUTY_CYCLE_OFF_VALUE ((uint32_t)BACKLIGHT_PIN << 16)

/* The K1's power-on fade: STEPS intermediate stops, STEP_MS apart, starting at
 * the first PWM value that produces a non-zero duty cycle. */
#define BL_STARTUP_FADE_STEPS   40
#define BL_STARTUP_FADE_STEP_MS 12
#define BL_STARTUP_VISIBLE_MIN  ((255u + BL_LEVELS - 1u) / BL_LEVELS)

/* DMA1 channel 2's TIM7 request, in SYSCFG->CFGR[2] bits[14:8].  Bits[6:0] are
 * the keypad's ADC1 map and must be preserved. */
#define BL_DMA_MAP_TIM7  0x35u

/* The K1's brightness staircase (0 = off .. 10 = max). */
const uint8_t value[11] = { 0, 8, 16, 24, 32, 48, 72, 104, 150, 200, 255 };

uint16_t gBacklightCountdown_500ms;
uint8_t  gBacklightBrightness = 10;

static uint32_t dutyCycle[BL_LEVELS];

static bool    backlightOn;
static bool    gUpdateBacklight;
static uint8_t currentIndex;
static int16_t currentBrightness;
static int16_t targetBrightness;
static int16_t fadeStep;

static void BACKLIGHT_SetHardwareBrightness(uint8_t brightness)
{
    const uint32_t level = (uint32_t)brightness * BL_LEVELS / 255u;

    if (level == 0u) {
        TIM7->CR1 &= ~TIM_CR1_CEN;
        DMA1_Channel2->CCR &= ~DMA_CCR_EN;
        gpio_write(BACKLIGHT_PORT, BACKLIGHT_PIN, BACKLIGHT_ON_LEVEL ? 0 : 1);
        return;
    }

    if (level >= BL_LEVELS) {
        TIM7->CR1 &= ~TIM_CR1_CEN;
        DMA1_Channel2->CCR &= ~DMA_CCR_EN;
        gpio_write(BACKLIGHT_PORT, BACKLIGHT_PIN, BACKLIGHT_ON_LEVEL ? 1 : 0);
        return;
    }

    for (uint32_t i = 0; i < BL_LEVELS; i++)
        dutyCycle[i] = (i < level) ? DUTY_CYCLE_ON_VALUE : DUTY_CYCLE_OFF_VALUE;

    if (!(TIM7->CR1 & TIM_CR1_CEN)) {
        DMA1_Channel2->CCR |= DMA_CCR_EN;
        TIM7->CR1 |= TIM_CR1_CEN;
    }
}

void BACKLIGHT_InitHardware(void)
{
    /* The pin must be a push-pull output: the reset state is analog/input, and
     * neither the direct writes nor the DMA's BSRR writes drive it until it is
     * configured.  The K1 does this in BOARD_GPIO_Init; here the driver owns the
     * pin. */
    gpio_port_clock(BACKLIGHT_PORT);
    gpio_config_output(BACKLIGHT_PORT, BACKLIGHT_PIN);
    RCC->APB1ENR |= RCC_APB1ENR_TIM7EN;
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;

    TIM7->PSC = 0;
    TIM7->ARR = BOARD_SYSCLK_HZ / BL_PWM_FREQ / BL_LEVELS - 1u;
    TIM7->CR1 |= TIM_CR1_ARPE;
    TIM7->DIER |= TIM_DIER_UDE;
    TIM7->EGR = TIM_EGR_UG;

    SYSCFG->CFGR[2] = (SYSCFG->CFGR[2] & ~(0x7Fu << 8)) | ((uint32_t)BL_DMA_MAP_TIM7 << 8);

    DMA1_Channel2->CCR = 0;
    DMA1_Channel2->CPAR = (uint32_t)&GPIOA->BSRR;
    DMA1_Channel2->CMAR = (uint32_t)dutyCycle;
    DMA1_Channel2->CNDTR = BL_LEVELS;
    DMA1->IFCR = DMA_IFCR_CGIF2;
    /* Memory -> peripheral (DIR), circular, memory increment, peripheral fixed,
     * 32-bit both sides, high priority.  DIR is the one that makes the DMA
     * *drive* the pin; without it the transfer runs the wrong way and the panel
     * never lights. */
    DMA1_Channel2->CCR = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_CIRC |
                         DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1 | DMA_CCR_PL;

    /* Init leaves the light off: the counter and channel stay off until a
     * brightness asks for them.  (At boot the state is BSS-zero anyway; setting
     * it here makes the contract hold if the driver is re-initialised.) */
    backlightOn = false;
    TIM7->CR1 &= ~TIM_CR1_CEN;
}

/* K1 name: hardware only, no light. */
void BACKLIGHT_Init(void)
{
    BACKLIGHT_InitHardware();
}

void BACKLIGHT_SetBrightness(uint8_t targetIndex)
{
    if (currentIndex == targetIndex)
        return;

    currentIndex = targetIndex;
    targetBrightness = value[targetIndex];

    const int16_t diff = (int16_t)(targetBrightness - currentBrightness);

    if (diff == 0) {
        gUpdateBacklight = false;
        return;
    }

    fadeStep = (diff > 0) ? (int16_t)-(-diff >> 4) : (int16_t)(diff >> 4);
    gUpdateBacklight = true;
}

uint8_t BACKLIGHT_GetBrightness(void)
{
    return currentIndex;
}

void BACKLIGHT_Update(void)
{
    if (!gUpdateBacklight)
        return;

    currentBrightness = (int16_t)(currentBrightness + fadeStep);

    if ((fadeStep > 0 && currentBrightness >= targetBrightness) ||
        (fadeStep < 0 && currentBrightness <= targetBrightness)) {
        currentBrightness = targetBrightness;
        gUpdateBacklight = false;
    }

    BACKLIGHT_SetHardwareBrightness((uint8_t)currentBrightness);
}

static void BACKLIGHT_FadeInStartup(void)
{
    const int16_t from = (targetBrightness < (int16_t)BL_STARTUP_VISIBLE_MIN)
                             ? targetBrightness
                             : (int16_t)BL_STARTUP_VISIBLE_MIN;
    const int16_t span = (int16_t)(targetBrightness - from);

    currentBrightness = from;
    BACKLIGHT_SetHardwareBrightness((uint8_t)currentBrightness);

    if (span <= 0) {
        gUpdateBacklight = false;
        return;
    }

    for (uint16_t s = 1; s < BL_STARTUP_FADE_STEPS; s++) {
        /* x in Q8 (0..256), e = smoothstep(x) in Q16 (0..65536). */
        const uint32_t x = (uint32_t)s * 256u / BL_STARTUP_FADE_STEPS;
        const uint32_t e = (x * x * (768u - 2u * x)) >> 8;

        currentBrightness = (int16_t)(from + (int16_t)(((uint32_t)span * e) >> 16));
        BACKLIGHT_SetHardwareBrightness((uint8_t)currentBrightness);
        SYSTEM_DelayMs(BL_STARTUP_FADE_STEP_MS);
    }

    currentBrightness = targetBrightness;
    BACKLIGHT_SetHardwareBrightness((uint8_t)currentBrightness);
    gUpdateBacklight = false;
}

static void BACKLIGHT_Sound(void)
{
    if (gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_SOUND ||
        gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_ALL
#ifdef ENABLE_FEAT_F4HWN_LOGO
        || gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_LOGO
#endif
    ) {
        AUDIO_PlayBeep(BEEP_880HZ_60MS_TRIPLE_BEEP);
        AUDIO_PlayBeep(BEEP_880HZ_60MS_TRIPLE_BEEP);
    }

    gK5startup = false;
}

void BACKLIGHT_TurnOn(void)
{
    gBacklightBrightnessOld = BACKLIGHT_GetBrightness();

    if (gEeprom.BACKLIGHT_TIME == 0u) {
        BACKLIGHT_TurnOff();
        if (gK5startup)
            BACKLIGHT_Sound();
        return;
    }

    backlightOn = true;
    BACKLIGHT_SetBrightness(gEeprom.BACKLIGHT_MAX);

    if (gK5startup) {
        BACKLIGHT_FadeInStartup();
        BACKLIGHT_Sound();
    }

    if (gEeprom.BACKLIGHT_TIME >= 61u)
        gBacklightCountdown_500ms = 0;              /* always on */
    else
        gBacklightCountdown_500ms = (uint16_t)(1u + (gEeprom.BACKLIGHT_TIME * 5u) * 2u);
}

void BACKLIGHT_TurnOff(void)
{
    BACKLIGHT_SetBrightness(gEeprom.BACKLIGHT_MIN);
    gBacklightCountdown_500ms = 0;
    backlightOn = false;
}

bool BACKLIGHT_IsOn(void)
{
    return backlightOn;
}

void BACKLIGHT_UpdateTickless(void)
{
    while (gUpdateBacklight) {
        BACKLIGHT_Update();
        SYSTEM_DelayMs(10);
    }
}

/* Diagnostic: how many duty words are currently ON (the PWM's level). */
unsigned BACKLIGHT_DutyOnCount(void)
{
    unsigned n = 0;
    unsigned i;

    for (i = 0; i < BL_LEVELS; i++)
        if (dutyCycle[i] == DUTY_CYCLE_ON_VALUE)
            n++;

    return n;
}
