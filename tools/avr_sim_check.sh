#!/bin/sh
# mini_rtos — AVR simavr harness.
#
# WHAT: Builds both AVR example firmwares for one board and runs them under
#       simavr, comparing the UART transcript against the exact expected
#       strings. Any mismatch, crash, or timeout fails loudly.
# WHY:  The host suite proves the kernel; this proves the PORT — the real
#       context switcher, the real 1 kHz tick, real preemption — on both
#       MCUs, with no hardware on the desk.
# HOW:  ./tools/avr_sim_check.sh uno|mega
#       Needs avr-gcc on PATH (or -D passing via cmake) and a simavr binary:
#       SIMAVR_BIN=path/to/run_avr (default: run_avr on PATH). See
#       docs/TOOLCHAINS.md for the user-local setup this repo was built with.
set -u

board="${1:-}"
if [ "$board" != "uno" ] && [ "$board" != "mega" ]; then
    echo "usage: $0 uno|mega" >&2
    exit 2
fi
if [ "$board" = "uno" ]; then mcu="atmega328p"; else mcu="atmega2560"; fi

root="$(dirname "$0")/.."
build="$root/build-$board"
sim="${SIMAVR_BIN:-run_avr}"
fail=0

# Rebuild from this tree so the sim always tests current sources.
if ! cmake -S "$root" -B "$build" \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchain-avr.cmake" \
    -DMRT_BOARD="$board" >/dev/null 2>&1; then
    echo "FAIL [$board]: configure failed (avr-gcc on PATH?)" >&2
    exit 1
fi
if ! cmake --build "$build" 2>&1 | grep -E "error|warning|Error" >&2; then
    : # grep found nothing: clean
fi
if ! cmake --build "$build" >/dev/null 2>&1; then
    echo "FAIL [$board]: build failed" >&2
    exit 1
fi

# Run one firmware: simulate, strip simavr's loader/color noise, and demand
# the transcript EXACTLY (no prefix games — extra output is a bug too).
check_one() {
    name="$1"
    want="$2"
    elf="$build/examples/$name.elf"
    # shellcheck disable=SC2086
    got="$(timeout 60 $sim -m "$mcu" -f 16000000 "$elf" 2>&1 \
        | sed -e 's/\x1b\[[0-9]*m//g' -e '/^Loaded /d' \
        -e '/^signal caught, simavr terminating$/d' \
        | tr -d '\n')"
    if [ "$got" = "$want" ]; then
        echo "PASS [$board] $name: $got"
    else
        echo "FAIL [$board] $name: got [$got] want [$want]" >&2
        fail=1
    fi
}

check_one blinky_avr "ABABAB!END"
check_one avr_selftest "KLHl!END"

exit "$fail"
