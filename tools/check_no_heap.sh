#!/bin/sh
# mini_rtos — heap-freedom gate.
#
# WHAT: Fails when kernel sources reference the libc heap, or when the built
#       kernel library has undefined heap symbols.
# WHY:  Kernel paths are static-only by design — no malloc, no free, no
#       fragmentation luck. The application heap (heap.c) is deliberately
#       named "release", so this grep stays meaningful: any hit is either a
#       real libc call or a naming accident, and both deserve attention.
# HOW:  Wired into CTest as no_heap_in_kernel; takes the kernel lib path as
#       $1 for the symbol check (source scan always runs).
set -u
root="$(dirname "$0")/.."
fail=0

# \b before the name keeps mrt_pool_free( / mrt_heap_release matching-free:
# "free" preceded by '_' has no word boundary there. Comment chatter without
# a call paren cannot match either.
if grep -rnE '\b(malloc|calloc|realloc|free)\s*\(' "$root/kernel" \
    --include='*.c' --include='*.h'; then
    echo "FAIL: libc heap calls referenced in kernel sources"
    fail=1
fi

lib="${1:-}"
if [ -n "$lib" ] && [ -f "$lib" ]; then
    if nm "$lib" 2>/dev/null | grep -E ' U (malloc|calloc|realloc|free)$'; then
        echo "FAIL: undefined heap symbols in kernel lib"
        fail=1
    fi
fi

exit "$fail"
