#!/bin/sh
# All offline checks for this firmware: the host tests, the K1 screen preview,
# and the target build.  Run from anywhere; it cds to firmware/.
#
#   firmware/tools/check_all.sh
#
# The build needs the ARM toolchain; set ARM_TOOLCHAIN_ROOT (see
# tools/BUILDING.md) or the build step is skipped with a warning.
set -e
cd "$(dirname "$0")/.."

CC=${CC:-gcc}
HOST_FLAGS="-std=c11 -I tools/host -I App -I App/driver"

echo "== test_rf (register paths, including stock BT pin semantics) =="
$CC -std=c11 -I tools/host -I App -I App/driver tools/test_rf.c \
    App/driver/bk4829.c App/driver/bk4815.c App/driver/bk4819.c \
    App/driver/pa.c tools/host/host_led.c -o /tmp/ra89r_test_rf
/tmp/ra89r_test_rf | tail -2

echo "== test_bk4815_rx (BK4815 receive tune) =="
$CC -std=c11 -I App -I App/driver tools/test_bk4815_rx.c \
    App/driver/bk4815.c App/driver/bk4815_rx.c -o /tmp/ra89r_test_bk4815_rx
/tmp/ra89r_test_bk4815_rx | tail -2

echo "== test_beeper (DAC tone math) =="
$CC -std=c11 -I App -I App/driver tools/test_beeper.c \
    -o /tmp/ra89r_test_beeper
/tmp/ra89r_test_beeper | tail -2

echo "== test_bk1080 (FM receiver) =="
$CC -std=c11 -I App -I App/driver tools/test_bk1080.c \
    App/driver/bk1080.c -o /tmp/ra89r_test_bk1080
/tmp/ra89r_test_bk1080 | tail -2

echo "== preview_k1 (screens + keys + settings, on a PC) =="
$CC $HOST_FLAGS -DPY32F403xD -include App/k1_features.h -DST7565_HOST_TEST \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    tools/preview_k1.c tools/host/host_hw.c tools/host/host_bk4819.c \
    tools/host/host_beeper.c \
    App/ui/main.c App/ui/menu.c App/ui/ui.c App/ui/status.c App/ui/welcome.c \
    App/ui/battery.c App/ui/scanner.c App/ui/helper.c App/ui/inputbox.c \
    App/ui/fmradio.c \
    App/app/menu.c App/app/action.c App/app/app.c App/app/main.c \
    App/app/generic.c App/app/common.c App/app/chFrScanner.c App/app/dtmf.c \
    App/app/scanner.c App/app/fm.c App/radio.c App/functions.c App/audio.c \
    App/misc.c App/driver/py25q16.c \
    App/board.c App/settings.c App/version.c App/dcs.c App/frequencies.c \
    App/helper/battery.c App/helper/boot.c App/driver/system.c App/font.c \
    App/bitmaps.c App/driver/st7565.c App/driver/keyboard.c \
    App/driver/backlight.c App/driver/audio_path.c -o /tmp/ra89r_preview_k1
/tmp/ra89r_preview_k1 | tail -3

if [ -n "${ARM_TOOLCHAIN_ROOT:-}" ] && [ -d "$ARM_TOOLCHAIN_ROOT" ]; then
    echo "== firmware build =="
    cmake --build build/Debug 2>&1 | tail -4
else
    echo "== firmware build skipped: set ARM_TOOLCHAIN_ROOT =="
fi

echo "== all checks done =="
