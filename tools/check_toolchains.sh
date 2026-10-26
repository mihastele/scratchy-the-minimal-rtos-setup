#!/bin/sh
# mini_rtos — toolchain check.
#
# WHAT: Prints the versions of every compiler this project cares about.
# WHY:  "Works on my machine" starts with knowing which machine. Run this
#       before reporting a build failure, and paste the output.
for t in gcc cmake make python3 avr-gcc arm-none-eabi-gcc size; do
    if command -v "$t" >/dev/null 2>&1; then
        # First line of --version is the human summary on all of these.
        printf '%-18s %s\n' "$t:" "$("$t" --version 2>/dev/null | head -n1)"
    else
        printf '%-18s %s\n' "$t:" "MISSING"
    fi
done
