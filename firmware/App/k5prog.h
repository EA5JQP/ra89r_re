/* The UV-K5/K1 serial programming protocol (what CHIRP's F4HWN drivers and
 * webchirp speak), as a byte-fed engine with no device dependency.
 *
 * Wire format, both directions:
 *
 *   AB CD  len_lo len_hi  |  XOR16(payload[len] + crc16_lo crc16_hi)  |  DC BA
 *
 * The XOR key is the K5's 16-byte table; the CRC is CRC-16/XMODEM over the
 * plain payload.  Replies carry no CRC: the two bytes before DC BA are
 * XOR16(FF FF), as the K1 firmware sends them.  A payload starts with a
 * little-endian command ID and size.
 *
 * Commands served (the subset CHIRP's clone driver uses):
 *   0x0514 hello          -> 0x0515 version + session timestamp latch
 *   0x051B read  (ts)     -> 0x051C offset, size, data
 *   0x051D write (ts)     -> 0x051E offset
 *   0x052F hello variant  -> 0x0515
 *   0x05DD reset
 * Read/write are ignored unless they carry the timestamp of the last hello.
 *
 * See docs/ra89r_chirp.md.
 */
#ifndef APP_K5PROG_H
#define APP_K5PROG_H

#include <stdbool.h>
#include <stdint.h>

#define K5PROG_MAX_PAYLOAD   252u    /* the K1's UART_Command_t data limit */
#define K5PROG_MAX_BLOCK     128u    /* largest read the K1 answers */
#define K5PROG_SESSION_MS    3000u   /* a session ends this long after a frame */
#define K5PROG_BYTE_GAP_MS   500u    /* a frame stalled this long is dropped */

typedef struct {
    /* Raw bytes to the wire (no newline translation). */
    void (*send)(const uint8_t *buf, uint16_t len);
    /* The K1 virtual EEPROM.  `len` is at most K5PROG_MAX_BLOCK. */
    void (*read)(uint16_t addr, uint8_t *buf, uint8_t len);
    void (*write)(uint16_t addr, const uint8_t *buf, uint8_t len);
    /* A hello opened a session (the backend snapshots its state). */
    void (*hello)(void);
    /* 0x05DD.  May not return. */
    void (*reset)(void);
    /* The 16-byte version field of the hello reply (CHIRP shows it). */
    const char *version;
} k5prog_ops_t;

typedef struct {
    uint32_t frames;     /* frames that passed CRC and were dispatched */
    uint32_t bad;        /* framing / CRC failures */
    uint32_t reads;
    uint32_t writes;
    uint32_t stale;      /* read/write with the wrong session timestamp */
} k5prog_stats_t;

void k5prog_init(const k5prog_ops_t *ops);

/* Feed one received byte.  Returns true when the byte belongs to a K5 frame
 * (the caller must not hand it to anyone else).  An idle engine only claims
 * 0xAB, which neither a typed console key nor a BCD CAT byte can be.  A
 * complete frame is dispatched from inside this call. */
bool k5prog_feed(uint8_t byte, uint32_t now_ms);

/* A frame is part-received. */
bool k5prog_busy(void);

/* A valid frame arrived within the last K5PROG_SESSION_MS. */
bool k5prog_session_active(uint32_t now_ms);

const k5prog_stats_t *k5prog_stats(void);

/* Exposed for the host test and the sim. */
uint16_t k5prog_crc16(const uint8_t *data, uint16_t len);
void     k5prog_xor(uint8_t *data, uint16_t len);

#endif
