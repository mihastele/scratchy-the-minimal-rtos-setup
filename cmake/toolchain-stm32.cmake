# mini_rtos — STM32 toolchain file.
#
# STATUS: stub. Phase 5 completes this: sets arm-none-eabi-gcc, the
# Cortex-M flags (-mcpu=..., -mthumb), and the .bin/size helpers.
#
# Deliberately forces MRT_TARGET=stm32 so that using this file today fails
# in the root CMakeLists with a clear "not ported yet" message instead of a
# confusing cross-compiled host-port error.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

find_program(CMAKE_C_COMPILER arm-none-eabi-gcc REQUIRED
  DOC "ARM embedded GCC (install: see docs/TOOLCHAINS.md)")

set(MRT_TARGET stm32 CACHE STRING "Port to build" FORCE)

# Phase 5 fills in from here: MRT_STM32_CPU cache var, -mcpu/-mthumb flags,
# linker script selection, objcopy .bin generation, size reporting.
