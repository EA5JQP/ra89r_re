/* FT-817 CAT frame parser.  See App/cat.h and docs/ra89r_cat.md.
 *
 * Device-header free: the parsing decisions are pure so they can be checked on
 * a PC (tools/test_cat.c).
 */
#include "cat.h"

#include <string.h>

void cat_parser_reset(cat_parser_t *p)
{
    if (p == 0)
        return;
    p->n = 0;
}

static bool bcd_ok(const uint8_t *b, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        if (((b[i] >> 4) & 0x0Fu) > 9u || (b[i] & 0x0Fu) > 9u)
            return false;
    }
    return true;
}

/* A frame is valid only when its command byte is known and its four payload
 * bytes have the shape that command uses (Look4Sat's builders). */
static bool cat_frame_valid(const uint8_t *w)
{
    switch (w[4]) {
    case CAT_CMD_SET_FREQ:
        return bcd_ok(w, 4);
    case CAT_CMD_READ:
    case CAT_CMD_PTT_ON:
    case CAT_CMD_PTT_OFF:
        return (uint8_t)(w[0] | w[1] | w[2] | w[3]) == 0u;
    case CAT_CMD_SET_MODE:
        return w[1] == 0u && w[2] == 0u && w[3] == 0u && w[0] <= 0x0Cu;
    case CAT_CMD_CTCSS_MODE:
        return w[1] == 0u && w[2] == 0u && w[3] == 0u &&
               (w[0] == 0x2Au || w[0] == 0x8Au);
    case CAT_CMD_CTCSS_TONE:
        return w[2] == 0u && w[3] == 0u && bcd_ok(w, 2);
    default:
        return false;
    }
}

bool cat_parser_feed(cat_parser_t *p, uint8_t byte, uint8_t frame[CAT_FRAME_LEN])
{
    if (p == 0 || frame == 0)
        return false;

    if (p->n < CAT_FRAME_LEN) {
        p->win[p->n++] = byte;
    } else {
        memmove(&p->win[0], &p->win[1], CAT_FRAME_LEN - 1u);
        p->win[CAT_FRAME_LEN - 1u] = byte;
    }

    if (p->n < CAT_FRAME_LEN)
        return false;
    if (!cat_frame_valid(p->win))
        return false;

    memcpy(frame, p->win, CAT_FRAME_LEN);
    p->n = 0;
    return true;
}

uint8_t cat_frame_cmd(const uint8_t frame[CAT_FRAME_LEN])
{
    return frame[CAT_FRAME_LEN - 1u];
}

uint32_t cat_bcd_to_freq_10hz(const uint8_t bcd[4])
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < 4; i++)
        v = v * 100u + (uint32_t)(((bcd[i] >> 4) & 0x0Fu) * 10u + (bcd[i] & 0x0Fu));

    return v;
}

void cat_freq_10hz_to_bcd(uint32_t freq_10hz, uint8_t bcd[4])
{
    int i;

    for (i = 3; i >= 0; i--) {
        bcd[i] = (uint8_t)((((freq_10hz / 10u) % 10u) << 4) | (freq_10hz % 10u));
        freq_10hz /= 100u;
    }
}

cat_mod_t cat_mode_to_modulation(uint8_t mode_byte)
{
    /* 0x04 = AM, everything else the radio can do maps to FM. */
    return (mode_byte == 0x04u) ? CAT_MOD_AM : CAT_MOD_FM;
}

uint8_t cat_modulation_to_mode(cat_mod_t mod)
{
    return (mod == CAT_MOD_AM) ? 0x04u : 0x08u;   /* 0x08 = FM */
}
