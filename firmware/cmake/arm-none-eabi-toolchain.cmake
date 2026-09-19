# Cross toolchain for the RA89R (Puya PY32F403, Cortex-M4F).
#
# Point TOOLCHAIN_ROOT at an arm-none-eabi GCC installation, or put
# arm-none-eabi-gcc on PATH and leave it empty:
#
#   cmake --preset Debug -DTOOLCHAIN_ROOT=/path/to/arm-none-eabi
#
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_PREFIX arm-none-eabi-)

if(NOT DEFINED TOOLCHAIN_ROOT AND DEFINED ENV{ARM_TOOLCHAIN_ROOT})
    set(TOOLCHAIN_ROOT "$ENV{ARM_TOOLCHAIN_ROOT}" CACHE PATH "arm-none-eabi toolchain root")
endif()

if(TOOLCHAIN_ROOT)
    set(TOOLCHAIN_BIN "${TOOLCHAIN_ROOT}/bin")
    find_program(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc PATHS "${TOOLCHAIN_BIN}" NO_DEFAULT_PATH)
    find_program(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc PATHS "${TOOLCHAIN_BIN}" NO_DEFAULT_PATH)
    find_program(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy PATHS "${TOOLCHAIN_BIN}" NO_DEFAULT_PATH)
    find_program(CMAKE_SIZE ${TOOLCHAIN_PREFIX}size PATHS "${TOOLCHAIN_BIN}" NO_DEFAULT_PATH)
else()
    find_program(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc REQUIRED)
    find_program(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc REQUIRED)
    find_program(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy REQUIRED)
    find_program(CMAKE_SIZE ${TOOLCHAIN_PREFIX}size REQUIRED)
endif()

set(CMAKE_C_COMPILER_TARGET arm-none-eabi)
set(CMAKE_ASM_COMPILER_TARGET arm-none-eabi)
# A plain executable link test cannot run on the host, so let CMake try a
# static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
