# The F4HWN fresh port (`port2`) -- see docs/superpowers/plans/2026-10-26-f4hwn-fresh-port.md.
#
# Everything here is the K1's own build shape, carried over when its own
# CMake can express it: the same App source list, the same enable_feature()
# selection, the -DENABLE_X feature mechanism (NOT a forced header -- the
# snapshot's files stay as-is).  Only hardware differences live outside this
# file: the compat headers (compat/py32f071_*.h, see README) and the board
# facts, which live in the copied register-level drivers of the old port and
# the classified Port adaptation blocks inside the snapshot.

set(F4HWN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/f4hwn)
set(F4HWN_APP ${F4HWN_DIR}/App)

add_executable(f4hwn_fw ${CORE_SOURCES} ${ASM_SOURCES})

# ---------------------------------------------------------------------------
# feature set: the K1's default preset, adapted only to this board
# ---------------------------------------------------------------------------

# The K1's own `enable_feature()` from firmware_uv-k1/CMakeLists.txt: feature
# macros optionally add sources.  Kept verbatim so the snapshot's feature list
# stays diffable against upstream.
macro(f4hwn_feature feature)
    if(${feature})
        target_compile_definitions(f4hwn_fw PRIVATE ${feature})
        if(${ARGC} GREATER 1)
            target_sources(f4hwn_fw PRIVATE ${ARGN})
        endif()
    endif()
endmacro()

set(ENABLE_UART        ON)       # USART1 PB6/PB7 115200; USB (VCP/CherryUSB) off -- no USB
set(ENABLE_VOX         ON)
set(ENABLE_TX1750      ON)
set(ENABLE_FLASHLIGHT  OFF)      # no lamp on this radio; PC13 is the amplifier line
set(ENABLE_SPECTRUM    OFF)
set(ENABLE_NOAA        OFF)
set(ENABLE_VOICE       OFF)
set(ENABLE_AIRCOPY     OFF)
set(ENABLE_FMRADIO     OFF)
set(ENABLE_BIG_FREQ              ON)
set(ENABLE_SMALL_BOLD            ON)
set(ENABLE_CUSTOM_MENU_LAYOUT    ON)
set(ENABLE_KEEP_MEM_NAME         ON)
set(ENABLE_WIDE_RX               ON)
set(ENABLE_SQUELCH_MORE_SENSITIVE ON)
set(ENABLE_FASTER_CHANNEL_SCAN    ON)
set(ENABLE_RSSI_BAR              ON)
set(ENABLE_AUDIO_BAR             ON)
set(ENABLE_COPY_CHAN_TO_VFO      ON)
set(ENABLE_SCAN_RANGES           ON)
set(ENABLE_FEAT_F4HWN            ON)
set(ENABLE_FEAT_F4HWN_SPECTRUM   ON)
set(ENABLE_FEAT_F4HWN_RX_TX_TIMER ON)
set(ENABLE_FEAT_F4HWN_SLEEP      ON)
set(ENABLE_FEAT_F4HWN_RESUME_STATE ON)
set(ENABLE_FEAT_F4HWN_NARROWER   ON)
set(ENABLE_FEAT_F4HWN_INV        ON)
set(ENABLE_FEAT_F4HWN_CTR        ON)
set(ENABLE_FEAT_F4HWN_RESET_VFO  ON)
set(ENABLE_FEAT_F4HWN_CA         ON)

# every feature that is ON becomes a -D, exactly as the K1's enable_feature()
# macro would have done for its own preset
foreach(_f
        ENABLE_VOX ENABLE_TX1750 ENABLE_SPECTRUM
        ENABLE_NOAA ENABLE_VOICE ENABLE_AIRCOPY ENABLE_FMRADIO
        ENABLE_BIG_FREQ ENABLE_SMALL_BOLD ENABLE_CUSTOM_MENU_LAYOUT
        ENABLE_KEEP_MEM_NAME ENABLE_WIDE_RX ENABLE_SQUELCH_MORE_SENSITIVE
        ENABLE_FASTER_CHANNEL_SCAN ENABLE_RSSI_BAR ENABLE_AUDIO_BAR
        ENABLE_COPY_CHAN_TO_VFO ENABLE_SCAN_RANGES
        ENABLE_FEAT_F4HWN ENABLE_FEAT_F4HWN_SPECTRUM ENABLE_FEAT_F4HWN_RX_TX_TIMER
        ENABLE_FEAT_F4HWN_SLEEP ENABLE_FEAT_F4HWN_RESUME_STATE
        ENABLE_FEAT_F4HWN_NARROWER ENABLE_FEAT_F4HWN_INV ENABLE_FEAT_F4HWN_CTR
        ENABLE_FEAT_F4HWN_RESET_VFO ENABLE_FEAT_F4HWN_CA)
    f4hwn_feature(${_f})
endforeach()

# UART's own sources (upstream conditionals), plus the crc/eeprom helpers it
# needs; the K1's own `enable_feature(ENABLE_UART driver/uart.c)` shape.
f4hwn_feature(ENABLE_UART
    f4hwn/App/driver/uart.c
    f4hwn/App/app/uart.c)

if(ENABLE_UART OR ENABLE_USB)
    target_sources(f4hwn_fw PRIVATE
        f4hwn/App/driver/crc.c
        f4hwn/App/driver/eeprom_compat.c)
endif()

# Version identity (same values the K1 default carries unless overridden).
set(AUTHOR_STRING_1 "EGZUMER")
set(AUTHOR_STRING_2 "F4HWN")
set(VERSION_STRING_1 "v0.22")
set(VERSION_STRING_2 "v6.2.0")
set(EDITION_STRING  "RA89R")
set(AUTHOR_STRING "${AUTHOR_STRING_1}+${AUTHOR_STRING_2}")
set(VERSION_STRING ${VERSION_STRING_2})
set(DISPLAY_VERSION_STRING_2 "${VERSION_STRING_2}")

# ---------------------------------------------------------------------------
# sources: the snapshot, in the K1's own list order
# ---------------------------------------------------------------------------

target_sources(f4hwn_fw PRIVATE
    # --- the snapshot (upstream App/CMakeLists.txt's list, minus
    #     compact_div.S: the M4 has hardware sdiv/sdiv, the M0 stub is dead
    #     weight) -----------------------------------------------------------
    f4hwn/App/driver/backlight.c
    f4hwn/App/driver/bk4819.c
    f4hwn/board/main.c
    f4hwn/App/driver/py25q16.c
    f4hwn/App/driver/gpio.c
    f4hwn/App/driver/keyboard.c
    f4hwn/App/driver/st7565.c
    f4hwn/App/driver/system.c
    f4hwn/App/driver/systick.c
    f4hwn/App/app/action.c
    f4hwn/App/app/app.c
    f4hwn/App/app/chFrScanner.c
    f4hwn/App/app/common.c
    f4hwn/App/app/dtmf.c
    f4hwn/App/app/generic.c
    f4hwn/App/app/main.c
    f4hwn/App/app/menu.c
    f4hwn/App/app/scanner.c
    f4hwn/App/audio.c
    f4hwn/App/bitmaps.c
    f4hwn/App/board.c
    f4hwn/App/dcs.c
    f4hwn/App/font.c
    f4hwn/App/frequencies.c
    f4hwn/App/functions.c
    f4hwn/App/helper/battery.c
    f4hwn/App/helper/boot.c
    f4hwn/App/misc.c
    f4hwn/App/radio.c
    f4hwn/App/settings.c
    f4hwn/App/ui/battery.c
    f4hwn/App/ui/helper.c
    f4hwn/App/ui/inputbox.c
    f4hwn/App/ui/main.c
    f4hwn/App/ui/menu.c
    f4hwn/App/ui/scanner.c
    f4hwn/App/ui/status.c
    f4hwn/App/ui/ui.c
    f4hwn/App/ui/welcome.c
    f4hwn/App/version.c
    f4hwn/App/main.c
    f4hwn/App/external/printf/printf.c
    # --- board shim: the validated register-level drivers of the old port ---
    # --- the old port's validated register-level drivers, as App/driver ------
    f4hwn/App/driver/clock.c
    f4hwn/App/driver/early.c
    f4hwn/App/driver/scheduler.c
    f4hwn/App/driver/lcd_st7565.c
    f4hwn/App/driver/font_8x16.c
    f4hwn/App/driver/font_5x7.c
    f4hwn/App/driver/rf_bus.c
    f4hwn/App/driver/led.c
    f4hwn/board/pa.c
    f4hwn/board/uart_console.c
    f4hwn/App/driver/audio_path.c
    f4hwn/App/driver/keypad.c
    f4hwn/App/driver/battery.c
    f4hwn/App/driver/spi_flash.c
)

f4hwn_feature(ENABLE_VOICE       f4hwn/App/driver/voice.c)
f4hwn_feature(ENABLE_SPECTRUM    f4hwn/App/app/spectrum.c)
f4hwn_feature(ENABLE_FMRADIO     f4hwn/App/driver/bk1080.c)
f4hwn_feature(ENABLE_FMRADIO     f4hwn/App/app/fm.c)

if(ENABLE_UART OR ENABLE_USB)
    target_sources(f4hwn_fw PRIVATE
        f4hwn/App/driver/crc.c
        f4hwn/App/driver/eeprom_compat.c)
endif()

# ---------------------------------------------------------------------------
# includes: compat FIRST (it carries the py32f071_* header names), then the
# snapshot's own include root, then the old port's device-header-free drivers
# that the board facts live in
# ---------------------------------------------------------------------------

target_include_directories(f4hwn_fw PRIVATE
    ${F4HWN_DIR}/compat
    ${F4HWN_APP}
    ${F4HWN_APP}/driver
    ${F4HWN_APP}/helper
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    "${PY32_DEVICE_INC}"
    "${PY32_SDK}/Drivers/CMSIS/Include"
    "${PY32_SDK}/Drivers/PY32F403_HAL_Driver/Inc")

target_compile_definitions(f4hwn_fw PRIVATE PY32F403xD)

target_compile_definitions(f4hwn_fw PRIVATE
    "AUTHOR_STRING=\"${AUTHOR_STRING}\""
    "VERSION_STRING=\"${VERSION_STRING}\""
    "BUILD_COMMIT=\"port2\""
    "AUTHOR_STRING_1=\"${AUTHOR_STRING_1}\""
    "AUTHOR_STRING_2=\"${AUTHOR_STRING_2}\""
    "VERSION_STRING_1=\"${VERSION_STRING_1}\""
    "VERSION_STRING_2=\"${VERSION_STRING_2}\""
    "DISPLAY_VERSION_STRING_2=\"${DISPLAY_VERSION_STRING_2}\""
    "EDITION_STRING=\"${EDITION_STRING}\""
    ALERT_TOT=10
    SQL_TONE=550
    $<$<CONFIG:Debug>:DEBUG>)

target_compile_options(f4hwn_fw PRIVATE
    ${MCU_FLAGS} -Wall -Wextra -fdata-sections -ffunction-sections)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(f4hwn_fw PRIVATE -Og -g3 -gdwarf-2)
else()
    target_compile_options(f4hwn_fw PRIVATE -Os -g0)
endif()

# Same application-region linker script as the old port: flash at 0x08004000,
# bootloader intact.
target_link_options(f4hwn_fw PRIVATE
    ${MCU_FLAGS}
    -T${CMAKE_CURRENT_SOURCE_DIR}/Core/py32f403xd_app.ld
    -Wl,-Map=f4hwn_fw.map,--cref
    -Wl,--gc-sections
    -specs=nano.specs
    -Wl,--no-warn-rwx-segments)

target_link_libraries(f4hwn_fw PRIVATE c m)

add_custom_command(TARGET f4hwn_fw POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary -S $<TARGET_FILE:f4hwn_fw> f4hwn_fw.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:f4hwn_fw> f4hwn_fw.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:f4hwn_fw>
    COMMENT "generating f4hwn_fw.bin / .hex")

if(PYTHON3)
    add_custom_command(TARGET f4hwn_fw POST_BUILD
        COMMAND ${PYTHON3} ${RA89R_REPO}/tools/ra89r.py mkicf
                f4hwn_fw.bin f4hwn_fw.icf --base 0x08004000
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "wrapping f4hwn_fw.bin into f4hwn_fw.icf (bootloader records)")
endif()
