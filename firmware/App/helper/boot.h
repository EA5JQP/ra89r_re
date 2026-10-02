/* The K1's boot-time key modes (helper/boot.c).
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
 * RA89R adaptation (see NOTICE, docs/ra89r_port.md): only the F-lock mode is
 * kept.  The air-copy, rescue-ops and multiboot modes the K1 can also enter are
 * not enabled in this port.
 */
#ifndef HELPER_BOOT_H
#define HELPER_BOOT_H

#include <stdint.h>

enum BOOT_Mode_t
{
    BOOT_MODE_NORMAL = 0,
    BOOT_MODE_F_LOCK,
};

typedef enum BOOT_Mode_t BOOT_Mode_t;

BOOT_Mode_t BOOT_GetMode(void);
void BOOT_ProcessMode(BOOT_Mode_t Mode);

#endif /* HELPER_BOOT_H */
