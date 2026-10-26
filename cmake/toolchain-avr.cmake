# mini_rtos — AVR toolchain file.
#
# WHAT: Cross-compile the kernel + AVR port + AVR examples for one board.
# HOW:  cmake -S . -B build-uno -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-avr.cmake \
#         -DMRT_BOARD=uno        # or -DMRT_BOARD=mega
#       cmake --build build-uno
#       Run tools/avr_sim_check.sh to execute the firmware under simavr.
#
# PREREQUISITES: avr-gcc, avr-objcopy, avr-size on PATH — or point at them:
#   -DMRT_AVR_GCC=/path/to/avr-gcc  (objcopy/size are found beside it)
# No install needed: extracting the avr-gcc/avr-binutils/avr-libc packages
# anywhere and adding usr/bin to PATH works (that is how CI does it — see
# docs/TOOLCHAINS.md).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR avr)

# Bare metal: test programs cannot link (no OS) or run — compile-only checks.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXECUTABLE_SUFFIX .elf)

# Locate the compiler: explicit path wins, else PATH search, else a loud
# error naming the missing piece (not a cryptic "compiler NOTFOUND" later).
if(MRT_AVR_GCC)
  set(CMAKE_C_COMPILER "${MRT_AVR_GCC}" CACHE FILEPATH "AVR C compiler" FORCE)
else()
  find_program(MRT_AVR_GCC_FOUND avr-gcc)
  if(NOT MRT_AVR_GCC_FOUND)
    message(FATAL_ERROR
      "avr-gcc not found. Install avr-gcc+avr-libc (see docs/TOOLCHAINS.md) "
      "or pass -DMRT_AVR_GCC=/path/to/avr-gcc.")
  endif()
  set(CMAKE_C_COMPILER "${MRT_AVR_GCC_FOUND}" CACHE FILEPATH "AVR C compiler" FORCE)
endif()

# Binutils beside the compiler (same prefix): objcopy for .hex, size for
# the SRAM budget report.
get_filename_component(MRT_AVR_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
find_program(MRT_AVR_OBJCOPY avr-objcopy HINTS "${MRT_AVR_BIN_DIR}" REQUIRED)
find_program(MRT_AVR_SIZE avr-size HINTS "${MRT_AVR_BIN_DIR}" REQUIRED)

# This file selects the AVR port; the board (mcu + budgets) is chosen in the
# root CMakeLists from -DMRT_BOARD (default uno).
set(MRT_TARGET avr CACHE STRING "Port to build" FORCE)
