/* Host test for App/k5prog.c: frames built byte for byte the way CHIRP's
 * F4HWN driver builds them (_send_command / _receive_reply in
 * f4hwn_v6_2_0.py), against a 64 KB RAM "EEPROM". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "k5prog.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t  mem[0x10000];
static uint8_t  out[4096];
static unsigned out_n;
static int      resets, hellos;

static void t_send(const uint8_t *b, uint16_t n) { memcpy(&out[out_n], b, n); out_n += n; }
static void t_read(uint16_t a, uint8_t *b, uint8_t n) { memcpy(b, &mem[a], n); }
static void t_write(uint16_t a, const uint8_t *b, uint8_t n) { memcpy(&mem[a], b, n); }
static void t_hello(void) { hellos++; }
static void t_reset(void) { resets++; }

static const k5prog_ops_t ops = { t_send, t_read, t_write, t_hello, t_reset, "RA89R F4HWN" };

static uint32_t now = 1000;

/* CHIRP's _send_command(): header >HBB (0xabcd, len, 0), xorarr(data+crc), >H 0xdcba */
static void chirp_send(const uint8_t *data, unsigned len)
{
    uint8_t  f[300];
    unsigned i, n = 0;
    uint16_t crc = k5prog_crc16(data, (uint16_t)len);

    f[n++] = 0xAB; f[n++] = 0xCD; f[n++] = (uint8_t)len; f[n++] = 0;
    memcpy(&f[n], data, len);
    f[n + len] = (uint8_t)crc;
    f[n + len + 1] = (uint8_t)(crc >> 8);
    k5prog_xor(&f[n], (uint16_t)(len + 2));
    n += len + 2;
    f[n++] = 0xDC; f[n++] = 0xBA;
    for (i = 0; i < n; i++) {
        bool took = k5prog_feed(f[i], now);
        CHECK(took, "byte %u of a frame not claimed", i);
    }
}

/* CHIRP's _receive_reply(): returns the de-obfuscated body, or -1. */
static int chirp_reply(uint8_t *body)
{
    unsigned len;

    if (out_n < 8 || out[0] != 0xAB || out[1] != 0xCD || out[3] != 0x00)
        return -1;
    len = out[2];
    if (out_n != len + 8u || out[4 + len + 2] != 0xDC || out[4 + len + 3] != 0xBA)
        return -1;
    memcpy(body, &out[4], len);
    k5prog_xor(body, (uint16_t)len);
    out_n = 0;
    return (int)len;
}

static const uint8_t ts[4] = { 0x6a, 0x39, 0x57, 0x64 };

static void hello(void)
{
    uint8_t p[8] = { 0x14, 0x05, 0x04, 0x00 };
    memcpy(&p[4], ts, 4);
    chirp_send(p, sizeof p);
}

int main(void)
{
    uint8_t body[300];
    int     n;
    unsigned i;

    for (i = 0; i < sizeof mem; i++)
        mem[i] = (uint8_t)(i * 7u + (i >> 8));
    k5prog_init(&ops);

    /* hello -> 0x0515 with the version at body[4..] (_sayhello/_getstring) */
    hello();
    n = chirp_reply(body);
    CHECK(n == 40, "hello reply len %d", n);
    CHECK(n > 0 && body[0] == 0x15 && body[1] == 0x05, "hello reply id");
    CHECK(n > 0 && memcmp(&body[4], "RA89R F4HWN", 11) == 0, "version string");
    CHECK(hellos == 1, "hello callback");
    CHECK(k5prog_session_active(now), "session active after hello");

    /* readmem: 1b 05 08 00 | <HBB offset len 0 | ts ; reply data at rep[8:] */
    {
        uint8_t p[12] = { 0x1b, 0x05, 0x08, 0x00, 0x80, 0xA0, 0x80, 0x00 };
        memcpy(&p[8], ts, 4);
        chirp_send(p, sizeof p);
        n = chirp_reply(body);
        CHECK(n == 128 + 8, "read reply len %d", n);
        CHECK(n > 0 && body[0] == 0x1C && body[1] == 0x05, "read reply id");
        CHECK(n > 0 && memcmp(&body[8], &mem[0xA080], 128) == 0, "read data");
    }

    /* writemem: 1d 05 | <BBHBB dlen+8 0 offset dlen 1 | ts | data ; rep[0]==0x1e, rep[4..5]=offset */
    {
        uint8_t p[12 + 128];
        uint8_t data[128];

        for (i = 0; i < sizeof data; i++)
            data[i] = (uint8_t)(0xC0u ^ i);
        p[0] = 0x1d; p[1] = 0x05; p[2] = 128 + 8; p[3] = 0;
        p[4] = 0x00; p[5] = 0x12; p[6] = 128; p[7] = 1;
        memcpy(&p[8], ts, 4);
        memcpy(&p[12], data, sizeof data);
        chirp_send(p, sizeof p);
        n = chirp_reply(body);
        CHECK(n == 6 && body[0] == 0x1e && body[4] == 0x00 && body[5] == 0x12, "write reply");
        CHECK(memcmp(&mem[0x1200], data, 128) == 0, "write data landed");
    }

    /* the last CHIRP block is 0x78 bytes (0xA100..0xA177): a multiple of 8 */
    {
        uint8_t p[12 + 0x78];
        memset(p, 0x5A, sizeof p);
        p[0] = 0x1d; p[1] = 0x05; p[2] = 0x78 + 8; p[3] = 0;
        p[4] = 0x00; p[5] = 0xA1; p[6] = 0x78; p[7] = 1;
        memcpy(&p[8], ts, 4);
        chirp_send(p, sizeof p);
        n = chirp_reply(body);
        CHECK(n == 6, "short write reply");
        CHECK(mem[0xA177] == 0x5A && mem[0xA178] != 0x5A, "short write extent");
    }

    /* wrong timestamp: ignored, no reply */
    {
        uint8_t p[12] = { 0x1b, 0x05, 0x08, 0x00, 0, 0, 16, 0, 1, 2, 3, 4 };
        chirp_send(p, sizeof p);
        CHECK(out_n == 0, "stale read answered");
        CHECK(k5prog_stats()->stale == 1, "stale counted");
    }

    /* a corrupt CRC is dropped without a reply */
    {
        uint8_t f[] = { 0xAB, 0xCD, 0x08, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 9, 9, 0xDC, 0xBA };
        for (i = 0; i < sizeof f; i++)
            (void)k5prog_feed(f[i], now);
        CHECK(out_n == 0 && k5prog_stats()->bad >= 1, "bad crc");
    }

    /* console keys and CAT bytes are not claimed while idle */
    CHECK(!k5prog_feed('h', now), "console key claimed");
    CHECK(!k5prog_feed(0x14, now), "BCD byte claimed");
    /* a stray AB followed by a key: the key goes back to the console */
    CHECK(k5prog_feed(0xAB, now), "AB not claimed");
    CHECK(!k5prog_feed('i', now), "key after stray AB claimed");

    /* a stalled frame is dropped after the gap, the next one still parses */
    CHECK(k5prog_feed(0xAB, now) && k5prog_feed(0xCD, now), "start");
    now += K5PROG_BYTE_GAP_MS + 1u;
    hello();
    n = chirp_reply(body);
    CHECK(n == 40, "hello after a stalled frame");

    /* reset */
    {
        uint8_t p[4] = { 0xdd, 0x05, 0x00, 0x00 };
        chirp_send(p, sizeof p);
        CHECK(resets == 1, "reset");
    }

    now += K5PROG_SESSION_MS;
    CHECK(!k5prog_session_active(now), "session expires");

    printf("%s: %d failures\n", failures ? "FAIL" : "all k5prog checks passed", failures);
    return failures ? 1 : 0;
}
