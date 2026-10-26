# mini_rtos — root build.
#
# WHAT: Builds the portable kernel plus the selected port, then the host
#       test suite and examples.
# HOW:  Out-of-source only:
#         cmake -S . -B build-host
#         cmake --build build-host
#         ctest --test-dir build-host -V
#       MRT_TARGET picks the port. Only "host" exists in Phase 0; asking for
#       avr/stm32 fails fast with a pointer to the right README instead of a
#       page of linker errors.
cmake_minimum_required(VERSION 3.16)
project(mini_rtos C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# NOTE: C extensions stay ON (gnu11), so the host port's POSIX ucontext
# stays visible. Firmware builds switch compilers via cmake/toolchain-*.cmake
# and never touch the host port.

add_compile_options(-Wall -Wextra -Werror)

set(MRT_TARGET "host" CACHE STRING "Port to build: host (avr/stm32 land in Phase 4/5)")
if(NOT MRT_TARGET STREQUAL "host")
  message(FATAL_ERROR "MRT_TARGET=${MRT_TARGET} is not ported yet — see port/${MRT_TARGET}/README.md (AVR: Phase 4, STM32: Phase 5).")
endif()

option(MRT_BUILD_TESTS "Build the host test suite" ON)
option(MRT_BUILD_EXAMPLES "Build the host examples" ON)

# Port first: the kernel links against it (kernel calls port, never the
# reverse), so the port target must exist before kernel/ is processed.
add_subdirectory(port/host)
add_subdirectory(kernel)

if(MRT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(MRT_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# `cmake --build build-host --target size-report`: firmware-size discipline
# starts on day one, even for host binaries. AVR/STM32 extend this with
# their section breakdowns in Phase 4/5.
find_program(MRT_SIZE_TOOL size)
if(MRT_BUILD_EXAMPLES AND MRT_SIZE_TOOL)
  add_custom_target(size-report
    COMMAND ${MRT_SIZE_TOOL} -B "$<TARGET_FILE:blinky_host>"
    DEPENDS blinky_host
    COMMENT "Sizes of example firmware")
endif()
