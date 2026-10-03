/* Battery sense -- the MCU's ADC channel 9 (PB1).
 *
 * Not a companion chip: the stock reads the pack on the ADC, and this reuses the
 * keypad driver's free-running DMA scan, which already converts PB1 every round
 * (`channels[]` in keypad.c is { 2, 3, 6, 7, 8, 9 }, and the keypad only decodes
 * the first five).  The earlier bit-banged "gauge" on PC14/PB2 was the BK1080 FM
 * receiver; see ra89r_battery.h and ra89r_battery.md.
 */
#include "driver/battery.h"

#include "driver/keypad.h"

/* Calibrated on this radio: 3413 counts read 8.32 V on a multimeter, i.e.
 * 8.32 / 3413 = 2.438 mV per count.  (Full scale, 4095, is therefore ~10 V, the
 * divider's top.)  Re-check if the sense divider or reference ever changes. */
#define BATTERY_MV_NUM 2438u
#define BATTERY_MV_DEN 1000u

uint16_t battery_raw(void)
{
    return keypad_aux_raw();
}

uint16_t battery_level(void)
{
    return (uint16_t)(battery_raw() >> 4);
}

bool battery_mv(uint32_t *mv)
{
    *mv = (uint32_t)battery_raw() * BATTERY_MV_NUM / BATTERY_MV_DEN;
    return true;
}
