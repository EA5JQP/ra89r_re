/* The K1's BOARD_* surface on this board (see NOTICE and board.h).
 *
 * The K1's board.c owns the vendor board bring-up (clocks, pins, ADC, flash)
 * and the battery ADC read.  On the RA89R the bring-up is this repo's
 * (driver/clock.c, early.c, gpio.c and the drivers), so only the battery read is
 * left for the application, and that is what this file provides.
 */
#include <stdint.h>

#include "board.h"
#include "driver/battery.h"
#include "helper/battery.h"

void BOARD_ADC_Init(void)
{
    /* Nothing to do: the pack is on ADC channel 9 (PB1), which the keypad
     * driver's free-running DMA scan already converts (driver/keypad.c). */
}

/* The pack is on ADC channel 9 (PB1); driver/battery.c reads it from the keypad's
 * scan.  The K1's helper/battery.c turns the returned value into 10 mV units with
 *     gBatteryVoltageAverage = (value * 760) / gBatteryCalibration[3]
 * so it wants the pack voltage in 10 mV -- which is battery_mv()/10, calibrated
 * on this radio (3413 counts = 8.32 V on a multimeter).  There is no charge-
 * current sense on this board, so the current is reported as zero. */
void BOARD_ADC_GetBatteryInfo(uint16_t *pVoltage, uint16_t *pCurrent)
{
    uint32_t mv = 0;

    battery_mv(&mv);
    if (pVoltage != 0)
        *pVoltage = (uint16_t)(mv / 10u);
    if (pCurrent != 0)
        *pCurrent = 0;
}
