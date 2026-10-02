/* The K1's boot-time key modes.
 *
 * Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ---------------------------------------------------------------------------
 * RA89R adaptation (see NOTICE, docs/ra89r_port.md):
 *
 * The K1 polls `GPIO_IsPttPressed()` for the PTT.  On this radio the PTT is PB9,
 * which the keypad reader exposes separately as `keypad_ptt2_level()`: it moves
 * no ADC-ladder line, so when PTT and a ladder key are held together
 * `keypad_poll()` reports the ladder key and the PTT has to be read on its own.
 * (The port keeps `GPIO_IsPttPressed()` false on purpose -- its PTT runs through
 * `driver/tx.c`, not the K1's chip sequence.)  Only the F-lock mode is ported;
 * the air-copy, rescue-ops and multiboot modes are not enabled here.
 */
#include "helper/boot.h"

#include <stdbool.h>

#include "driver/keyboard.h"
#include "driver/keypad.h"
#include "driver/system.h"
#include "ui/ui.h"

BOOT_Mode_t BOOT_GetMode(void)
{
    unsigned int i;
    KEY_Code_t   Keys[2];
    bool         PttPressed[2];

    /* Two samples, 20 ms apart, as the K1: PTT has to be held for both, and the
     * same key has to be down for both. */
    for (i = 0; i < 2; i++)
    {
        PttPressed[i] = !keypad_ptt2_level();   /* PB9 low = PTT held */
        Keys[i]       = keypad_poll();
        SYSTEM_DelayMs(20);
    }

    if (!PttPressed[0] || !PttPressed[1])
        return BOOT_MODE_NORMAL;

    if (Keys[0] == Keys[1])
    {
        gKeyReading0     = Keys[0];
        gKeyReading1     = Keys[0];
        gDebounceCounter = 2;

        if (Keys[0] == KEY_SIDE1)
            return BOOT_MODE_F_LOCK;
    }

    return BOOT_MODE_NORMAL;
}

void BOOT_ProcessMode(BOOT_Mode_t Mode)
{
    GUI_SelectNextDisplay((Mode == BOOT_MODE_F_LOCK) ? DISPLAY_MENU : DISPLAY_MAIN);
}
