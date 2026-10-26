# mini_rtos — AVR toolchain file.
#
# STATUS: stub. Phase 4 completes this: sets avr-gcc, the MCU flags
# (-mmcu=atmega328p / -mmcu=atmega2560), and the flash/size helpers.
#
# Deliberately forces MRT_TARGET=avr so that using this file today fails in
# the root CMakeLists with a clear "not ported yet" message instead of a
# confusing cross-compiled host-port error.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR avr)

find_program(CMAKE_C_COMPILER avr-gcc REQUIRED
  DOC "AVR GCC (install: see docs/TOOLCHAINS.md)")

set(MRT_TARGET avr CACHE STRING "Port to build" FORCE)

# Phase 4 fills in from here: MRT_AVR_MCU cache var, -mmcu flags,
# avr-objcopy .hex generation, avr-size reporting.
