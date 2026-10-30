/* Host tests for the spectrum's chip policy (App/app/spectrum_rf.c).
 *
 *   gcc -std=c11 -I App -I App/driver tools/test_spectrum.c \
 *       App/app/spectrum_rf.c -o /tmp/test_spectrum
 *
 * spectrum_rf.c is device-header free, so this links nothing else: the chip
 * toggle, the RSSI normalization and the persisted-byte encoding are checked on a
 * PC, without a radio.
 */
#include <stdio.h>
#include <stdbool.h>

#include "app/spectrum_rf.h"

static int failures;

static void check(bool ok, const char *what)
{
    printf("[%s] %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

int main(void)
{
    /* The in-screen toggle flips between the two chips. */
    check(spectrum_rf_next_chip(SPECTRUM_CHIP_4829) == SPECTRUM_CHIP_4815,
          "toggle 4829 -> 4815");
    check(spectrum_rf_next_chip(SPECTRUM_CHIP_4815) == SPECTRUM_CHIP_4829,
          "toggle 4815 -> 4829");

    /* The BK4815's 7-bit RSSI is scaled into the BK4829's 9-bit range. */
    check(spectrum_rf_normalize_rssi(SPECTRUM_CHIP_4829, 0x1234) == 0x1234,
          "4829 rssi unchanged");
    check(spectrum_rf_normalize_rssi(SPECTRUM_CHIP_4815, 0x40) == 0x100,
          "4815 rssi x4");
    check(spectrum_rf_normalize_rssi(SPECTRUM_CHIP_4815, 0x7F) == 508,
          "4815 rssi full scale -> 508");
    check(spectrum_rf_normalize_rssi(SPECTRUM_CHIP_4815, 0xFF) == 508,
          "4815 rssi masked to 7 bits");

    /* The persisted byte round-trips; anything else decodes to 4829. */
    check(spectrum_rf_decode_chip(spectrum_rf_encode_chip(SPECTRUM_CHIP_4829)) == SPECTRUM_CHIP_4829,
          "4829 round-trips");
    check(spectrum_rf_decode_chip(spectrum_rf_encode_chip(SPECTRUM_CHIP_4815)) == SPECTRUM_CHIP_4815,
          "4815 round-trips");
    check(spectrum_rf_decode_chip(0xFF) == SPECTRUM_CHIP_4829, "invalid byte -> 4829");
    check(spectrum_rf_decode_chip(2) == SPECTRUM_CHIP_4829, "removed Both byte -> 4829");
    check(spectrum_rf_decode_chip(3) == SPECTRUM_CHIP_4829, "out-of-range byte -> 4829");
    check(spectrum_rf_encode_chip((spectrum_chip_t)9) == 0, "invalid chip -> 0");

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall spectrum_rf checks passed\n");
    return 0;
}
