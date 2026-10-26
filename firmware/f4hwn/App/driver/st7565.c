/* ST7565-family panel driver in the UV-K1/K5V3 layout, on the RA89R.
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
 * RA89R adaptation (see NOTICE and docs/ra89r_port.md):
 *
 *   * the API, the buffer geometry and the page mapping are the K1's: the
 *     status line is panel page 0, gFrameBuffer[0..6] are pages 1..7, and the
 *     +4 column offset the K1 writes literally in DrawLine is this repo's
 *     LCD_COLUMN_OFFSET;
 *   * the transport is this repo's bit-banged driver (driver/lcd_st7565.c,
 *     which mirrors the stock firmware's byte writer) instead of the K1's
 *     hardware SPI, so CS/A0/SCLK handling stays in one place;
 *   * the init sequence is lcd_init(), i.e. the bootloader-proven standard
 *     ST7565 sequence, which is known to drive this glass (docs/ra89r_lcd.md).
 *
 * Define ST7565_HOST_TEST to build the buffer half only, with the panel calls
 * compiled out (firmware/tools/preview_k1.c renders layout on a PC).
 */
#include <stdint.h>
#include <stddef.h>

#include "driver/lcd_st7565.h"
#include "driver/st7565.h"

uint8_t gStatusLine[LCD_WIDTH];
uint8_t gFrameBuffer[FRAME_LINES][LCD_WIDTH];

#ifdef ST7565_HOST_TEST
#define PANEL_INIT()            ((void)0)
#define PANEL_REFRESH_INIT()    ((void)0)
#define PANEL_CMD(cmd)          ((void)(cmd))
#define PANEL_DATA(data)        ((void)(data))
#else
#define PANEL_INIT()            lcd_init()
#define PANEL_REFRESH_INIT()    lcd_reinit(lcd_variant())
#define PANEL_CMD(cmd)          lcd_write_cmd(cmd)
#define PANEL_DATA(data)        lcd_write_data(data)
#endif

void ST7565_SelectColumnAndLine(uint8_t Column, uint8_t Line)
{
    /* Raw panel addressing: the K1 expects its caller to have added the +4
     * column offset (DrawLine does), and callers such as FillScreen pass it
     * explicitly, so no offset is applied here. */
    PANEL_CMD((uint8_t)(0xB0u | (Line & 0x0Fu)));
    PANEL_CMD((uint8_t)(0x10u | ((Column >> 4) & 0x0Fu)));
    PANEL_CMD((uint8_t)(Column & 0x0Fu));
}

/* Write a command rather than pixel data (K1 naming kept). */
void ST7565_WriteByte(uint8_t Value)
{
    PANEL_CMD(Value);
}

static void DrawLine(uint8_t column, uint8_t line, const uint8_t *lineBuffer, unsigned size_defVal)
{
    unsigned i;

    ST7565_SelectColumnAndLine((uint8_t)(column + LCD_COLUMN_OFFSET), line);
    for (i = 0; i < size_defVal; i++)
        PANEL_DATA(lineBuffer ? lineBuffer[i] : (uint8_t)size_defVal);
}

void ST7565_DrawLine(const unsigned int Column, const unsigned int Line,
                     const uint8_t *pBitmap, const unsigned int Size)
{
    DrawLine((uint8_t)Column, (uint8_t)Line, pBitmap, Size);
}

void ST7565_BlitFullScreen(void)
{
    unsigned line;

    for (line = 0; line < FRAME_LINES; line++)
        DrawLine(0, (uint8_t)(line + 1), gFrameBuffer[line], LCD_WIDTH);
}

void ST7565_BlitLine(unsigned line)
{
    DrawLine(0, (uint8_t)(line + 1), gFrameBuffer[line], LCD_WIDTH);
}

void ST7565_BlitStatusLine(void)
{
    DrawLine(0, 0, gStatusLine, LCD_WIDTH);
}

void ST7565_FillScreen(uint8_t value)
{
    uint8_t line;
    uint8_t column;

    for (line = 0; line < 8u; line++) {
        ST7565_SelectColumnAndLine((uint8_t)LCD_COLUMN_OFFSET, line);
        for (column = 0; column < LCD_WIDTH; column++)
            PANEL_DATA(value);
    }
}

void ST7565_Init(void)
{
    PANEL_INIT();

    /* Clear both the buffer and the controller RAM, as the K1 does. */
    ST7565_FillScreen(0x00);
    ST7565_FillScreen(0x00);
    for (uint8_t line = 0; line < FRAME_LINES; line++)
        for (uint8_t column = 0; column < LCD_WIDTH; column++)
            gFrameBuffer[line][column] = 0;
    for (uint8_t column = 0; column < LCD_WIDTH; column++)
        gStatusLine[column] = 0;
}

void ST7565_HardwareReset(void)
{
    /* The RA89R has a usable reset line and lcd_init() pulses it; the K1 has
     * no reset pin, which is why its own version of this is empty. */
    PANEL_INIT();
}

void ST7565_FixInterfGlitch(void)
{
    /* Re-run the panel init, which is what the K1 achieves by replaying its
     * command table here. */
    PANEL_REFRESH_INIT();
}

/* Port adaptation: the K1's ST7565_ShutDown (upstream driver/st7565.c line
 * 376).  The fresh panel module can put the display off with the same commands
 * the old-port core knows: all-rails-off, then display off. */
#ifdef ENABLE_FEAT_F4HWN_SLEEP
void ST7565_ShutDown(void)
{
    PANEL_CMD(0xAE);   /* display off -- the K1 command table (st7565.c:263) */
}
#endif

/* Port adaptation: the K1's ST7565_ContrastAndInv (upstream driver/st7565.c
 * line 297), which the old-port copy dropped.  On this panel the same effect
 * is achieved by replaying the panel's own init (lcd_reinit -- the validated
 * boot sequence, which already applies the CTR/INV settings from the port's
 * own set of menu facts); the K1's byte-for-byte table rewrite is what its
 * board needed, this is the equivalent for this one. */
#if defined(ENABLE_FEAT_F4HWN_CTR) || defined(ENABLE_FEAT_F4HWN_INV)
void ST7565_ContrastAndInv(void)
{
    PANEL_REFRESH_INIT();
}
#endif

int16_t map(int16_t x, int16_t in_min, int16_t in_max, int16_t out_min, int16_t out_max)
{
    return (int16_t)((x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min);
}

void ST7565_Gauge(uint8_t line, uint8_t min, uint8_t max, uint8_t value)
{
    uint8_t i;
    uint8_t filled = (uint8_t)map(value, min, max, 56, 120);

    gFrameBuffer[line][54] = 0x0c;
    gFrameBuffer[line][55] = 0x12;
    gFrameBuffer[line][121] = 0x12;
    gFrameBuffer[line][122] = 0x0c;

    for (i = 56; i <= 120; i++)
        gFrameBuffer[line][i] = (i <= filled) ? 0x2d : 0x21;
}
