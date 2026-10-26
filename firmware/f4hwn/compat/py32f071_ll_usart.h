/* compat: the K5V3 files include py32f071_ll_usart.h directly.
 * The RA89R SDK (PY32F4xx_Firmware) ships the same LL API for the PY32F403;
 * forward to it (see docs/superpowers/plans/2026-10-26-f4hwn-fresh-port.md).
 */
#pragma once
#include "py32f403_ll_usart.h"
