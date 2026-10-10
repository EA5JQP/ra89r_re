/* Host tests for the FT-817 CAT parser (App/cat.c).
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_cat.c App/cat.c -o /tmp/test_cat
 *
 * cat.c is device-header free, so this links nothing else: the 5-byte frame
 * assembly, the auto-detect validity rules and the BCD helpers are checked on a
 * PC, without a radio.
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "cat.h"

static int failures;

static void check(bool ok, const char *what)
{
    printf("[%s] %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

/* Look4Sat's Ft817CatProtocol.buildSetFreqCommand(Hz): BCD(Hz/10) + 0x01. */
static void test_set_freq(void)
{
    cat_parser_t p;
    uint8_t frame[CAT_FRAME_LEN];
    cat_parser_reset(&p);

    /* 145.500000 MHz -> 14 55 00 00 01 */
    const uint8_t bytes[5] = { 0x14, 0x55, 0x00, 0x00, 0x01 };
    bool got = false;
    for (int i = 0; i < 5; i++)
        got = cat_parser_feed(&p, bytes[i], frame) || got;

    check(got, "set-frequency frame is detected");
    check(memcmp(frame, bytes, 5) == 0, "set-frequency frame bytes are exact");
    check(cat_frame_cmd(frame) == CAT_CMD_SET_FREQ, "command is SET_FREQ");
    check(cat_bcd_to_freq_10hz(frame) == 14550000u,
          "frequency decodes to 145.500000 MHz (14550000 x 10 Hz)");
}

/* A read/PTT/mode/CTCSS frame is 4 bytes then the command. */
static void test_other_frames(void)
{
    cat_parser_t p;
    uint8_t frame[CAT_FRAME_LEN];

    cat_parser_reset(&p);
    const uint8_t read[5] = { 0x00, 0x00, 0x00, 0x00, 0x03 };
    for (int i = 0; i < 5; i++)
        (void)cat_parser_feed(&p, read[i], frame);
    check(cat_frame_cmd(frame) == CAT_CMD_READ, "read frame detected");

    cat_parser_reset(&p);
    const uint8_t ptt[5] = { 0x00, 0x00, 0x00, 0x00, 0x08 };
    for (int i = 0; i < 5; i++)
        (void)cat_parser_feed(&p, ptt[i], frame);
    check(cat_frame_cmd(frame) == CAT_CMD_PTT_ON, "PTT-on frame detected");

    cat_parser_reset(&p);
    const uint8_t ptt_off[5] = { 0x00, 0x00, 0x00, 0x00, 0x88 };
    for (int i = 0; i < 5; i++)
        (void)cat_parser_feed(&p, ptt_off[i], frame);
    check(cat_frame_cmd(frame) == CAT_CMD_PTT_OFF, "PTT-off frame detected");

    /* Set mode: [mode,0,0,0,0x07] (FM = 0x08 in Look4Sat's map). */
    cat_parser_reset(&p);
    const uint8_t mode[5] = { 0x08, 0x00, 0x00, 0x00, 0x07 };
    for (int i = 0; i < 5; i++)
        (void)cat_parser_feed(&p, mode[i], frame);
    check(cat_frame_cmd(frame) == CAT_CMD_SET_MODE, "set-mode frame detected");
}

/* Look4Sat's builder sends a leading mode/sub byte before the command. */
static void test_mode_and_ctcss(void)
{
    cat_parser_t p;
    uint8_t frame[CAT_FRAME_LEN];

    cat_parser_reset(&p);
    const uint8_t fm[5] = { 0x01, 0x00, 0x00, 0x00, 0x07 };   /* mode byte 0x01 = USB */
    for (int i = 0; i < 5; i++)
        (void)cat_parser_feed(&p, fm[i], frame);
    check(cat_frame_cmd(frame) == CAT_CMD_SET_MODE && frame[0] == 0x01,
          "mode byte is preserved");

    cat_parser_reset(&p);
    const uint8_t ctcss[5] = { 0x2A, 0x00, 0x00, 0x00, 0x0A };  /* CTCSS enc on */
    for (int i = 0; i < 5; i++)
        (void)cat_parser_feed(&p, ctcss[i], frame);
    check(cat_frame_cmd(frame) == CAT_CMD_CTCSS_MODE && frame[0] == 0x2A,
          "CTCSS-mode byte is preserved");
}

/* Auto-detect: console text before a CAT frame must not hide it. */
static void test_autodetect(void)
{
    cat_parser_t p;
    uint8_t frame[CAT_FRAME_LEN];
    bool got = false;

    cat_parser_reset(&p);
    const char *noise = "hello\r\n";
    for (const char *c = noise; *c; c++)
        got = cat_parser_feed(&p, (uint8_t)*c, frame) || got;
    check(!got, "console text alone does not decode a frame");

    const uint8_t bytes[5] = { 0x14, 0x55, 0x00, 0x00, 0x01 };
    for (int i = 0; i < 5; i++)
        got = cat_parser_feed(&p, bytes[i], frame) || got;
    check(got, "a CAT frame after console text is still detected");
    check(cat_bcd_to_freq_10hz(frame) == 14550000u, "auto-detected frame decodes");
}

/* Invalid BCD in a set-frequency frame must not be accepted. */
static void test_reject(void)
{
    cat_parser_t p;
    uint8_t frame[CAT_FRAME_LEN];
    bool got = false;

    cat_parser_reset(&p);
    const uint8_t bad[5] = { 0xAB, 0x55, 0x00, 0x00, 0x01 };   /* 0xA/0xB nibbles */
    for (int i = 0; i < 5; i++)
        got = cat_parser_feed(&p, bad[i], frame) || got;
    check(!got, "a non-BCD set-frequency frame is rejected");
}

int main(void)
{
    test_set_freq();
    test_other_frames();
    test_mode_and_ctcss();
    test_autodetect();
    test_reject();

    /* The frequency->BCD encoder must round-trip what the radio replies. */
    {
        uint8_t bcd[4];
        cat_freq_10hz_to_bcd(43037500u, bcd);
        check(cat_bcd_to_freq_10hz(bcd) == 43037500u, "frequency BCD round-trips");
    }

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall cat parser checks passed\n");
    return 0;
}
