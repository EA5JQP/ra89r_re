/* FT-817 CAT (Yaesu) frame parser, for the radio's serial port.
 *
 * Look4Sat's `Ft817CatProtocol` drives an FT-817 over 5-byte frames: four
 * payload bytes then a command byte.  This module is the pure parser and BCD
 * helpers -- device-header free so it is host-testable (tools/test_cat.c); the
 * radio actions (tune/PTT/...) live in the caller.
 *
 * Auto-detect: the serial port also carries the bring-up console, so a frame is
 * only accepted when its command byte is a known one and its payload is
 * well-formed for that command -- console text cannot masquerade as a frame.
 */
#ifndef APP_CAT_H
#define APP_CAT_H

#include <stdbool.h>
#include <stdint.h>

#define CAT_FRAME_LEN 5

/* The FT-817 command byte, as Look4Sat's Ft817CatProtocol defines it. */
typedef enum {
    CAT_CMD_SET_FREQ   = 0x01,
    CAT_CMD_SM         = 0x02,   /* read S-meter (FT-817) */
    CAT_CMD_READ       = 0x03,
    CAT_CMD_SET_MODE   = 0x07,
    CAT_CMD_PTT_ON     = 0x08,
    CAT_CMD_PTT_OFF    = 0x88,
    CAT_CMD_CTCSS_MODE = 0x0A,
    CAT_CMD_CTCSS_TONE = 0x0B,
    CAT_CMD_VS         = 0x81,   /* VFO select A/B (FT-817) */
    /* Port extension, not in the FT-817 set: the receive state the FT-817 has
     * no read for.  Reply (5 bytes): status, rssi_lo, rssi_hi, sql_level,
     * CAT_CMD_STATUS; status bit 0 = squelch open, bit 1 = the audio lane is
     * the BK4815, bits 7:2 = the scan mode.  The RSSI is the lane's raw value
     * (9-bit on 0x67, 7-bit on 0x44). */
    CAT_CMD_STATUS     = 0xE1
} cat_cmd_t;

typedef struct {
    uint8_t win[CAT_FRAME_LEN];
    uint8_t n;
} cat_parser_t;

void cat_parser_reset(cat_parser_t *p);

/* Feed one received byte.  Returns true when a complete valid frame was
 * assembled into `frame` (5 bytes); the parser resets for the next one. */
bool cat_parser_feed(cat_parser_t *p, uint8_t byte, uint8_t frame[CAT_FRAME_LEN]);

uint8_t cat_frame_cmd(const uint8_t frame[CAT_FRAME_LEN]);

/* True if the 5 bytes are a well-formed frame (the auto-detect validity rule
 * `cat_parser_feed()` applies).  Exposed for the serial auto-detect in
 * `App/main.c`, which tests a buffered burst before applying it. */
bool cat_frame_valid_bytes(const uint8_t *w);

/* Apply a parsed frame to the radio (App/cat_radio.c): tune the selected VFO,
 * set modulation/PTT/CTCSS, or reply to a READ. */
void cat_apply(const uint8_t frame[CAT_FRAME_LEN]);

/* FT-817 frequency BCD is the decimal, 10 Hz-unit value, 2 digits per byte. */
uint32_t cat_bcd_to_freq_10hz(const uint8_t bcd[4]);
void     cat_freq_10hz_to_bcd(uint32_t freq_10hz, uint8_t bcd[4]);

/* Map a Look4Sat/FT-817 mode byte to this radio's modulation.  Only FM and AM
 * exist on the RA89R; the SSB/CW/DIG/PKT modes fall back to FM. */
typedef enum {
    CAT_MOD_FM = 0,   /* covers FM, and the SSB/CW modes this radio lacks */
    CAT_MOD_AM = 1,
    CAT_MOD_NONE = 2
} cat_mod_t;

cat_mod_t cat_mode_to_modulation(uint8_t mode_byte);
uint8_t   cat_modulation_to_mode(cat_mod_t mod);

#endif
