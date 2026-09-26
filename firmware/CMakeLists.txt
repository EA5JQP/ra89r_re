cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

project(ra89r_fw C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# ---------------------------------------------------------------------------
# target description
# ---------------------------------------------------------------------------
set(MCU_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)

# The PY32F403 SDK is vendored in this repository (read-only, upstream clone).
get_filename_component(RA89R_REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(PY32_SDK "${RA89R_REPO}/PY32F4xx_Firmware" CACHE PATH "PY32F403 SDK root")
set(PY32_DEVICE_INC "${PY32_SDK}/Drivers/CMSIS/Device/PUYA/PY32F403/Include")
if(NOT EXISTS "${PY32_DEVICE_INC}/py32f403xD.h")
    message(FATAL_ERROR "PY32F403 SDK not found at ${PY32_SDK} (override with -DPY32_SDK=<path>)")
endif()

# ---------------------------------------------------------------------------
# sources
# ---------------------------------------------------------------------------
set(CORE_SOURCES
    Core/system_py32f403.c)
set(ASM_SOURCES
    Core/startup_py32f403xx.s)
set(APP_SOURCES
    App/main.c
    App/ui.c
    App/driver/backlight.c
    App/driver/led.c
    App/driver/clock.c
    App/driver/early.c
    App/driver/fault.c
    App/driver/gpio.c
    App/driver/uart.c
    App/driver/systick.c
    App/driver/lcd_st7565.c
    App/driver/keypad.c
    App/driver/rf_bus.c
    App/driver/bk4829.c
    App/driver/bk4815.c
    App/driver/bk4819.c
    App/driver/audio_path.c
    App/driver/pa.c
    App/driver/rx.c
    App/driver/tx.c
    App/driver/font_8x16.c
    App/driver/font_5x7.c
    # --- imported from the UV-K1/K5V3 project (see NOTICE) -------------------
    App/driver/st7565.c
    App/font.c
    App/bitmaps.c
    App/ui/helper.c
    App/ui/inputbox.c
    App/ui/main.c
    App/ui/menu.c
    App/ui/ui.c
    App/ui/status.c
    App/ui/welcome.c
    App/driver/keyboard.c
    App/port_gui.c
    App/app/menu.c
    App/app/action.c
    App/version.c
    App/dcs.c
    App/frequencies.c
    App/helper/battery.c
    App/driver/system.c
    App/radio.c
    App/functions.c
    App/audio.c
    App/misc.c
    App/port_state.c
    App/port_storage.c
    App/driver/spi_flash.c
    # --- end of imported sources ---------------------------------------------
    App/settings.c
    App/syscalls.c)

add_executable(${PROJECT_NAME} ${CORE_SOURCES} ${ASM_SOURCES} ${APP_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/App
    ${CMAKE_CURRENT_SOURCE_DIR}/App/driver
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    "${PY32_DEVICE_INC}"
    "${PY32_SDK}/Drivers/CMSIS/Include")

target_compile_definitions(${PROJECT_NAME} PRIVATE PY32F403xD)

# Feature macros for the imported K1 sources live in one header and are forced
# into every translation unit, so the imported files stay untouched.
target_compile_options(${PROJECT_NAME} PRIVATE
    -include ${CMAKE_CURRENT_SOURCE_DIR}/App/port_features.h)

target_compile_options(${PROJECT_NAME} PRIVATE
    ${MCU_FLAGS} -Wall -Wextra -fdata-sections -ffunction-sections)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${PROJECT_NAME} PRIVATE -Og -g3 -gdwarf-2)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Os -g0)
endif()

# Application linker script: flash at 0x08004000, leaving the stock bootloader
# (0x08000000-0x08003FFF) alone.
target_link_options(${PROJECT_NAME} PRIVATE
    ${MCU_FLAGS}
    -T${CMAKE_CURRENT_SOURCE_DIR}/Core/py32f403xd_app.ld
    -Wl,-Map=${PROJECT_NAME}.map,--cref
    -Wl,--gc-sections
    -specs=nano.specs
    -Wl,--no-warn-rwx-segments)

target_link_libraries(${PROJECT_NAME} PRIVATE c m)

# ---------------------------------------------------------------------------
# images + size report
# ---------------------------------------------------------------------------
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary -S $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
    COMMENT "generating ${PROJECT_NAME}.bin / .hex")

# Wrap the image in .icf records so it can be flashed with the stock bootloader
# (tools/ra89r_flash.py).  Same idea as `ra89r.py mkicf`, run automatically so
# the two never drift apart.
find_program(PYTHON3 python3)
if(PYTHON3)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${PYTHON3} ${RA89R_REPO}/ra89r.py mkicf
                ${PROJECT_NAME}.bin ${PROJECT_NAME}.icf --base 0x08004000
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "wrapping ${PROJECT_NAME}.bin into ${PROJECT_NAME}.icf (bootloader records)")
else()
    message(WARNING "python3 not found: build ${PROJECT_NAME}.icf manually with "
                    "`python3 ra89r.py mkicf ${PROJECT_NAME}.bin ${PROJECT_NAME}.icf`")
endif()

# firmware/tools/extract_fonts.py regenerates the stock font tables from a
# decoded .icf image; it is a host tool and not part of the firmware build.
