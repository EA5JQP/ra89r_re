/* The UV-K5/K1 serial programming protocol engine.  See App/k5prog.h.
 *
 * The command handling follows the K1's app/uart.c (Copyright 2023 Dual
 * Tachyon, 2025 muzkr; Apache-2.0): the same frame checks, the same
 * timestamp rule, the same reply layouts.  The receive side is a byte-fed
 * state machine instead of the K1's DMA-ring scan, because USART1 here is
 * shared with the console and CAT (App/main.c). */
#include "k5prog.h"

#include <string.h>

static const uint8_t k_xor[16] = {
    0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40,
    0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80
};

enum {
    ST_IDLE,     /* waiting for AB */
    ST_CD,       /* got AB */
    ST_LEN0,
    ST_LEN1,
    ST_BODY,     /* len + 2 obfuscated bytes */
    ST_FOOT0,    /* DC */
    ST_FOOT1     /* BA */
};

static const k5prog_ops_t *s_ops;
static k5prog_stats_t      s_stats;
static uint8_t             s_state = ST_IDLE;
static uint16_t            s_len;
static uint16_t            s_got;
static uint32_t            s_last_byte;
static uint32_t            s_last_frame;
static bool                s_seen_frame;
static uint32_t            s_timestamp;
static bool                s_have_ts;
static uint8_t             s_buf[K5PROG_MAX_PAYLOAD + 2u];

void k5prog_init(const k5prog_ops_t *ops)
{
    s_ops = ops;
    memset(&s_stats, 0, sizeof s_stats);
    s_state      = ST_IDLE;
    s_seen_frame = false;
    s_have_ts    = false;
}

uint16_t k5prog_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0;
    uint16_t i;
    unsigned b;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (b = 0; b < 8u; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

void k5prog_xor(uint8_t *data, uint16_t len)
{
    uint16_t i;

    for (i = 0; i < len; i++)
        data[i] ^= k_xor[i % 16u];
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* Frame and send one reply payload (ID + size + data), the K1's SendReply(). */
static void send_reply(uint8_t *payload, uint16_t size)
{
    uint8_t head[4] = { 0xAB, 0xCD, (uint8_t)size, (uint8_t)(size >> 8) };
    uint8_t foot[4];

    k5prog_xor(payload, size);
    foot[0] = (uint8_t)(k_xor[(size + 0u) % 16u] ^ 0xFFu);
    foot[1] = (uint8_t)(k_xor[(size + 1u) % 16u] ^ 0xFFu);
    foot[2] = 0xDC;
    foot[3] = 0xBA;

    s_ops->send(head, sizeof head);
    s_ops->send(payload, size);
    s_ops->send(foot, sizeof foot);
}

static void send_version(void)
{
    /* REPLY_0514_t: header, Version[16], bHasCustomAesKey, bIsInLockScreen,
     * Padding[2], Challenge[4] -- 4 + 36 bytes. */
    uint8_t r[40];
    size_t  n;

    memset(r, 0, sizeof r);
    wr16(&r[0], 0x0515u);
    wr16(&r[2], (uint16_t)(sizeof r - 4u));
    n = strlen(s_ops->version);
    memcpy(&r[4], s_ops->version, n < 16u ? n : 16u);
    send_reply(r, sizeof r);
}

static void cmd_hello(const uint8_t *p, uint16_t size)
{
    if (size < 4u + 4u)
        return;
    s_timestamp = rd32(&p[4]);
    s_have_ts   = true;
    if (s_ops->hello != 0)
        s_ops->hello();
    send_version();
}

static void cmd_read(const uint8_t *p, uint16_t size)
{
    /* CMD_051B_t: header, Offset u16, Size u8, Padding u8, Timestamp u32. */
    uint8_t  r[4u + 4u + K5PROG_MAX_BLOCK];
    uint16_t offset;
    uint8_t  n;

    if (size < 12u)
        return;
    if (!s_have_ts || rd32(&p[8]) != s_timestamp) {
        s_stats.stale++;
        return;
    }
    offset = rd16(&p[4]);
    n      = p[6];
    if (n > K5PROG_MAX_BLOCK)
        return;

    memset(r, 0, sizeof r);
    wr16(&r[0], 0x051Cu);
    wr16(&r[2], (uint16_t)(n + 4u));
    wr16(&r[4], offset);
    r[6] = n;
    s_ops->read(offset, &r[8], n);
    s_stats.reads++;
    send_reply(r, (uint16_t)(n + 8u));
}

static void cmd_write(const uint8_t *p, uint16_t size)
{
    /* CMD_051D_t: header, Offset u16, Size u8, bAllowPassword u8,
     * Timestamp u32, Data[Size]. */
    uint8_t  r[6];
    uint16_t offset;
    uint8_t  n;

    if (size < 12u)
        return;
    offset = rd16(&p[4]);
    n      = p[6];
    /* Data[] must be inside the frame (the K1 checks Header.Size). */
    if (rd16(&p[2]) < 8u + n || size < 12u + n)
        return;
    if (!s_have_ts || rd32(&p[8]) != s_timestamp) {
        s_stats.stale++;
        return;
    }

    /* The K1 writes whole 8-byte units and drops a sub-unit tail; CHIRP's
     * blocks are multiples of 8, so nothing is lost in practice. */
    n = (uint8_t)(n & ~7u);
    if (n > 0u)
        s_ops->write(offset, &p[12], n);
    s_stats.writes++;

    wr16(&r[0], 0x051Eu);
    wr16(&r[2], 2u);
    wr16(&r[4], offset);
    send_reply(r, sizeof r);
}

static void dispatch(uint16_t size)
{
    const uint8_t *p  = s_buf;
    const uint16_t id = rd16(&p[0]);

    switch (id) {
    case 0x0514:
    case 0x052F:
        cmd_hello(p, size);
        break;
    case 0x051B:
        cmd_read(p, size);
        break;
    case 0x051D:
        cmd_write(p, size);
        break;
    case 0x05DD:
        if (s_ops->reset != 0)
            s_ops->reset();
        break;
    default:
        break;           /* the K1 ignores what it does not implement */
    }
}

static void frame_done(uint32_t now_ms)
{
    uint16_t crc;

    k5prog_xor(s_buf, (uint16_t)(s_len + 2u));
    crc = (uint16_t)(s_buf[s_len] | ((uint16_t)s_buf[s_len + 1u] << 8));
    if (s_len < 4u || rd16(&s_buf[2]) > s_len - 4u || k5prog_crc16(s_buf, s_len) != crc) {
        s_stats.bad++;
        return;
    }
    s_stats.frames++;
    s_last_frame = now_ms;
    s_seen_frame = true;
    dispatch(s_len);
}

bool k5prog_feed(uint8_t b, uint32_t now_ms)
{
    if (s_ops == 0)
        return false;

    if (s_state != ST_IDLE && (uint32_t)(now_ms - s_last_byte) > K5PROG_BYTE_GAP_MS) {
        s_stats.bad++;
        s_state = ST_IDLE;
    }
    s_last_byte = now_ms;

    switch (s_state) {
    case ST_IDLE:
        if (b != 0xABu)
            return false;
        s_state = ST_CD;
        return true;
    case ST_CD:
        if (b == 0xCDu) {
            s_state = ST_LEN0;
            return true;
        }
        /* Not a frame after all: the lone AB is dropped (it is no key); a
         * second AB may still start one. */
        s_stats.bad++;
        if (b == 0xABu)
            return true;
        s_state = ST_IDLE;
        return false;
    case ST_LEN0:
        s_len   = b;
        s_state = ST_LEN1;
        return true;
    case ST_LEN1:
        s_len = (uint16_t)(s_len | ((uint16_t)b << 8));
        if (s_len > K5PROG_MAX_PAYLOAD) {
            s_stats.bad++;
            s_state = ST_IDLE;
            return true;
        }
        s_got   = 0;
        s_state = ST_BODY;
        return true;
    case ST_BODY:
        s_buf[s_got++] = b;
        if (s_got == s_len + 2u)
            s_state = ST_FOOT0;
        return true;
    case ST_FOOT0:
        if (b != 0xDCu) {
            s_stats.bad++;
            s_state = ST_IDLE;
            return true;
        }
        s_state = ST_FOOT1;
        return true;
    case ST_FOOT1:
        s_state = ST_IDLE;
        if (b != 0xBAu) {
            s_stats.bad++;
            return true;
        }
        frame_done(now_ms);
        return true;
    default:
        s_state = ST_IDLE;
        return false;
    }
}

bool k5prog_busy(void)
{
    return s_state != ST_IDLE;
}

bool k5prog_session_active(uint32_t now_ms)
{
    return s_seen_frame && (uint32_t)(now_ms - s_last_frame) < K5PROG_SESSION_MS;
}

const k5prog_stats_t *k5prog_stats(void)
{
    return &s_stats;
}
