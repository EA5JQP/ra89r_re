/* The K1's BOARD_* surface on this board (see NOTICE and board.h).
 *
 * The K1's board.c owns the vendor board bring-up (clocks, pins, ADC, flash)
 * and the battery ADC read.  On the RA89R the bring-up is this repo's
 * (driver/clock.c, early.c, gpio.c and the drivers), so only the battery read is
 * left for the application, and that is what this file provides.
 */
#include <stdint.h>

#include "board.h"
#include "helper/battery.h"

/* A plausible pack: the K1's units are the raw 12-bit ADC of its divider, and
 * helper/battery.c turns that into 10 mV units with
 *     gBatteryVoltageAverage = (raw * 760) / gBatteryCalibration[3]
 * so 1850 with the K1's own 1900 fallback calibration reads as 7.40 V.
 *
 * Where the real number has to come from: this radio's gauge chip, whose
 * protocol is decoded but which never answers us (ra89r_battery.md), or the
 * stock's own battery path.  Until one of those lands, the screens show a fixed
 * pack -- which is at least honest about being a placeholder.
 */
#define PORT_BATTERY_ADC_PLACEHOLDER 1850u

void BOARD_ADC_Init(void)
{
}

void BOARD_ADC_GetBatteryInfo(uint16_t *pVoltage, uint16_t *pCurrent)
{
    if (pVoltage != 0)
        *pVoltage = PORT_BATTERY_ADC_PLACEHOLDER;
    if (pCurrent != 0)
        *pCurrent = 0;
}
