# mini_rtos — root build.
#
# WHAT: Host simulator AND AVR firmware from one tree, picked by toolchain:
#         host:  cmake -S . -B build-host && cmake --build build-host
#         uno:   cmake -S . -B build-uno -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-avr.cmake -DMRT_BOARD=uno
#         mega:  ... -DMRT_BOARD=mega
# HOW:   MRT_TARGET selects the port (host default; the AVR toolchain file
#        forces avr). AVR builds skip the host test suite (it needs a
#        hosted libc) and build .elf firmware + .hex images instead; the
#        simavr harness in tools/ runs them.
cmake_minimum_required(VERSION 3.16)
project(mini_rtos C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# NOTE: C extensions stay ON (gnu11), so the host port's POSIX ucontext
# stays visible. Firmware builds switch compilers via cmake/toolchain-*.cmake
# and never touch the host port.

add_compile_options(-Wall -Wextra -Werror)

set(MRT_TARGET "host" CACHE STRING "Port to build: host or avr")
if(NOT MRT_TARGET STREQUAL "host" AND NOT MRT_TARGET STREQUAL "avr")
  message(FATAL_ERROR "MRT_TARGET=${MRT_TARGET} is not ported yet — see port/${MRT_TARGET}/README.md (STM32: Phase 5).")
endif()

option(MRT_BUILD_TESTS "Build the host test suite (host only)" ON)
option(MRT_BUILD_EXAMPLES "Build the examples" ON)

if(MRT_TARGET STREQUAL "avr")
  # The context switch is assembly: without this, CMake silently skips .S
  # files and the link fails on mrt_port_switch_to/yield_to_sched.
  enable_language(ASM)
  # Board presets: MCU + the SRAM budget the pool must fit. Uno's 2 KB is
  # the binding constraint (4x192 pool = 960 B); Mega fits the defaults.
  set(MRT_BOARD "uno" CACHE STRING "AVR board: uno (ATmega328P) or mega (ATmega2560)")
  if(MRT_BOARD STREQUAL "uno")
    set(MRT_AVR_MCU "atmega328p")
    add_compile_definitions(MRT_MAX_TASKS=4 MRT_AVR_TASK_STACK_SIZE=192)
  elseif(MRT_BOARD STREQUAL "mega")
    set(MRT_AVR_MCU "atmega2560")
  else()
    message(FATAL_ERROR "Unknown MRT_BOARD=${MRT_BOARD} (uno|mega).")
  endif()
  add_compile_options(-mmcu=${MRT_AVR_MCU} -Os)
  add_link_options(-mmcu=${MRT_AVR_MCU})
  add_compile_definitions(F_CPU=16000000UL)
endif()

# Port first: the kernel links against it (kernel calls port, never the
# reverse), so the port target must exist before kernel/ is processed.
if(MRT_TARGET STREQUAL "avr")
  add_subdirectory(port/avr)
else()
  add_subdirectory(port/host)
endif()
add_subdirectory(kernel)

if(MRT_BUILD_TESTS AND MRT_TARGET STREQUAL "host")
  enable_testing()
  add_subdirectory(tests)
endif()
if(MRT_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# `cmake --build <dir> --target size-report`: firmware-size discipline.
if(MRT_BUILD_EXAMPLES)
  if(MRT_TARGET STREQUAL "avr")
    add_custom_target(size-report
      COMMAND ${MRT_AVR_SIZE} -C --mcu=${MRT_AVR_MCU} "$<TARGET_FILE:blinky_avr>"
      DEPENDS blinky_avr
      COMMENT "SRAM/flash budget (${MRT_BOARD})")
  else()
    find_program(MRT_SIZE_TOOL size)
    if(MRT_SIZE_TOOL)
      add_custom_target(size-report
        COMMAND ${MRT_SIZE_TOOL} -B "$<TARGET_FILE:blinky_host>"
        DEPENDS blinky_host
        COMMENT "Sizes of example firmware")
    endif()
  endif()
endif()
