/* First draft of the K1 VFO screen, drawn with the K1's own helpers.
 *
 * This is scaffolding for the port, not the ported screen: it exists to prove
 * the imported substrate (driver/st7565.c + font.c + bitmaps.c + ui/helper.c)
 * end to end on the radio and on a PC, using the same drawing calls the real
 * screen uses.  It is replaced by the K1's ui/main.c (UI_DisplayMain) once the
 * settings/state facade lands -- see ra89r_port.md.
 */
#ifndef APP_K1_VFO_DRAFT_H
#define APP_K1_VFO_DRAFT_H

#include <stdbool.h>
#include <stdint.h>

/* Fills gStatusLine/gFrameBuffer with a VFO screen.  Caller blits. */
void k1_vfo_draft_draw(uint32_t freq_hz, const char *channel,
                       uint8_t rssi_bars, uint8_t battery_bars,
                       bool vfo_b, bool locked);

/* Draw and push to the panel (target only; a no-op buffer draw on the host). */
void k1_vfo_draft_show(uint32_t freq_hz, const char *channel,
                       uint8_t rssi_bars, uint8_t battery_bars,
                       bool vfo_b, bool locked);

#endif /* APP_K1_VFO_DRAFT_H */
