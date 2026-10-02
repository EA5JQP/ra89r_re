/* Copyright 2025 muzkr
 * https://github.com/muzkr
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

#ifndef DRIVER_PY25Q16_H
#define DRIVER_PY25Q16_H

#include <stdint.h>
#include <stdbool.h>

void PY25Q16_Init();
void PY25Q16_ReadBuffer(uint32_t Address, void *pBuffer, uint32_t Size);
void PY25Q16_ReadBufferSafe(uint32_t Address, void *pBuffer, uint32_t Size);
void PY25Q16_WriteBuffer(uint32_t Address, const void *pBuffer, uint32_t Size, bool Append);
void PY25Q16_SectorErase(uint32_t Address);

/* Drop the internal single-sector write cache. Call after erasing/programming
 * flash behind the driver's back (e.g. the raw multiboot slot/bank ops) so a
 * later write cannot skip or resurrect data based on a stale cached sector. It
 * is also called before multiboot reuses the cache storage as a RAM overlay. */
void PY25Q16_InvalidateCache(void);

#ifdef ENABLE_FEAT_F4HWN_OVERLAY_APPS
/* The 4 KiB sector cache, reused as the overlay-app execution workspace. */
uint8_t *PY25Q16_OverlayBuffer(void);
#endif

#ifdef ENABLE_FEAT_F4HWN_MULTIBOOT
/*
 * Multiboot per-bank config banking.
 *
 * Each firmware slot gets its own config bank by default (memory channels,
 * names, VFOs, settings), though SetCfg can point the running firmware at a
 * different bank. A non-zero bank base transparently shifts every flash access
 * BELOW PY25Q16_BANK_SHARED_FROM into the active bank; calibration, boot logo,
 * firmware slots and the multiboot marker all live at/above that boundary and
 * stay shared across every bank.
 *
 * This is the single choke point: both the EEPROM emulation (eeprom_compat.c)
 * and the firmware's direct config reads/writes (settings.c) end up here, so
 * one offset covers them all - no per-call-site patching.
 *
 * Set once at boot, before any settings read, from
 *   PY25Q16_SetBankBase(MB_BankBase(MB_BootResolveState()));
 * and never changed again during a session (a slot restore or SetCfg selection
 * records the next bank and resets first), so the banking itself never needs a
 * cache flush. Raw bank erases behind the driver explicitly call
 * PY25Q16_InvalidateCache().
 */
#define PY25Q16_BANK_SHARED_FROM  0x00010000u   /* calibration boundary (see flash map) */
void PY25Q16_SetBankBase(uint32_t Base);
#endif

/* ---------------------------------------------------------------------------
 * The port's surface over the same chip.  The implementation (driver/py25q16.c)
 * is the port's, because the K1's py25q16.c drives the K1's SPI; the PY25Q16_*
 * calls above are its interface, so the K1 code that uses them is unchanged.
 * ------------------------------------------------------------------------- */

void     storage_init(void);
uint32_t storage_size(void);
bool     storage_id(uint16_t *man_dev, uint32_t *jedec);

/* The port's own settings blob (no K1 counterpart): gEeprom and the frequency
 * channels the stock codeplug has no place for. */
#define STORAGE_EXTRA_MAX 240u
bool storage_load_settings(void);
bool storage_save_settings(void);
bool storage_set_extra(const void *data, uint32_t size);
bool storage_get_extra(void *data, uint32_t size);
bool storage_write_test(uint32_t *bad_offset);

/* The K1 application's EEPROM image, rebuilt in the RA89R's erased band at the
 * addresses the K1 code already uses (only the channel-record base moves off 0,
 * which is the stock's codeplug).  See docs/ra89r_calibration.md. */
#define K1_IMAGE_NAME_BASE 0x04000u
#define K1_IMAGE_ATTR_BASE 0x08000u
#define K1_IMAGE_CH_BASE   0x09000u
#define K1_IMAGE_CAL_BASE  0x100C0u
#define K1_IMAGE_CAL_SIZE  0xD0u
#define K1_IMAGE_BASE      0x04000u
#define K1_IMAGE_END       0x20000u
bool storage_writable(uint32_t addr, uint32_t size);
void storage_import_k1(void);

#endif
