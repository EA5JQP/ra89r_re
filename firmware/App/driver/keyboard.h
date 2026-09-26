/* Copyright 2023 Manuel Jinger
 * Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#ifndef DRIVER_KEYBOARD_H
#define DRIVER_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

/* Port adaptation (see NOTICE): the key codes come from this repo's keypad
 * driver, which reads the RA89R's ADC key ladder and returns the same K5V3 /
 * F4HWN enumeration the K1 uses (plus KEY_PTT2, which this radio has and the
 * K5V3 does not, and which moves KEY_INVALID up by one).  Keeping one
 * definition avoids two `enum KEY_Code_e`s in one translation unit. */
#include "driver/keypad.h"

typedef enum {
    STATE_IDLE = 0,
    STATE_KA_1,
    STATE_KA_2,
    STATE_KA_3,
#ifdef ENABLE_FEAT_F4HWN_RXTX_LOG_K5VIEWER
    STATE_KA_FEATURE,
#endif
    STATE_KEY_1,
    STATE_KEY_2,
    STATE_KEY_3,
    STATE_KEY_3L,
} ParseState_t;

extern KEY_Code_t gKeyReading0;
extern KEY_Code_t gKeyReading1;
extern uint16_t   gDebounceCounter;
extern bool       gWasFKeyPressed;

#ifdef ENABLE_FEAT_F4HWN_K5VIEWER
// Serial-injected key (written by UART/VCP parser, consumed by KEYBOARD_Poll).
extern volatile KEY_Code_t gKeyFromSerial;

#ifdef ENABLE_FEAT_F4HWN_RXTX_LOG_K5VIEWER
#define SERIAL_VIEWER_FEATURE_RF_LOG 0x01u
#define SERIAL_VIEWER_FEATURE_RF_LOG_HISTORY 0x02u
#define SERIAL_VIEWER_FEATURE_RF_LOG_RESTART 0x80u

// Extension flags announced by the viewer's feature keepalive.
extern volatile uint8_t    gSerialViewerFeatures;
#endif

bool KEYBOARD_ProcessProtocolByte(ParseState_t *state, uint8_t b);
#endif

KEY_Code_t KEYBOARD_Poll(void);
KEY_Code_t KEYBOARD_GetKey(void);

void HideFKeyIcon(void);

#endif
