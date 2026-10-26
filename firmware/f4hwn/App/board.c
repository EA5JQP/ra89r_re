/* The K1's BOARD_* surface on this board (see NOTICE and board.h).
 *
 * Port adaptation (from the validated ra89r port): the K1's board.c owns the
 * vendor bring-up -- clocks, pins, ADC, flash -- and the battery ADC read.
 * On this board the facts live in the register-level drivers this port ships
 * under f4hwn/board/ (clock 48 MHz per docs/ra89r_port.md, the panel's own
 * pin setup in driver/st7565.c, the keypad's free-running ADC scan in
 * driver/keypad.c -- which also carries the pack sense on channel 9), so
 * BOARD_Init is the order the old port validated its boot in, and only the
 * battery glue (unchanged) plus the K1-side pin ordering live here.
 */
#include <stdint.h>

#include "board.h"
#include "driver/battery.h"
#include "driver/clock.h"
#include "driver/keypad.h"
#include "driver/rf_bus.h"
#include "driver/backlight.h"
#include "driver/led.h"
#include "driver/st7565.h"
#include "helper/battery.h"

void BOARD_FLASH_Init(void)
{
    /* The flash wait states belong to the clock rate; the validated clock
     * bring-up sets them together with the PLL (board/clock.c). */
    (void)clock_init();
}

void BOARD_GPIO_Init(void)
{
    /* Port clocks used before their own init runs: the panel's GPIOB data
     * line and the LEDs on GPIOA.  The keypad ladder's analog pins and the
     * keyboard scanner own their GPIO config in keypad_init(). */
    gpio_port_clock(GPIOA);
    gpio_port_clock(GPIOB);

    /* LEDs idle off (PA13 red / PA14 green, active high -- docs/ra89r_led.md). */
    led_init();

    /* The panel pins are configured by the validated st7565 driver (GPIO
     * bit-bang, no SPI -- see its Port adaptation block). */
    ST7565_Init();

    /* The keypad reader (also clears the "all pins analog" state the stock
     * parks the ladder in, and starts the free-running scan). */
    keypad_init();

    /* RF bus select + data + clock idle state. */
    rf_bus_init();

    /* Backlight idle-off with the validated PA5 PWM facts. */
    BACKLIGHT_InitHardware();
}

void BOARD_Init(void)
{
    BOARD_FLASH_Init();
    BOARD_GPIO_Init();
}

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
    if (pVoltage != 0) {
        *pVoltage = (uint16_t)(mv / 10u);

        /* The K1 averages the last four samples and fills one per ~1 s, so a
         * fresh boot reads a quarter of the pack until they do (level 0, an
         * empty icon).  The pack sits on a stable ADC, so seed all four with
         * this reading and the first display is already right. */
        gBatteryVoltages[0] = *pVoltage;
        gBatteryVoltages[1] = *pVoltage;
        gBatteryVoltages[2] = *pVoltage;
        gBatteryVoltages[3] = *pVoltage;
    }
    if (pCurrent != 0)
        *pCurrent = 0;
}
