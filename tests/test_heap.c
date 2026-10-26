/*
 * mini_rtos — tests/test_heap.c
 *
 * WHAT:  The optional application heap: split, coalescing, OOM, and every
 *        misuse the magic stamps exist to catch.
 * WHY:   A heap without coalescing is a leak with extra steps: the key
 *        assertion below (a freed pair fits a double-sized request) fails
 *        on any allocator that cannot fuse neighbors. Header-size-agnostic
 *        accounting keeps the test honest across layout changes.
 * HOW:   Main-line calls with exact byte accounting, where the per-block
 *        overhead is measured empirically instead of hardcoded.
 */
#include <stdint.h>
#include <stdio.h>

#include "mini_rtos/heap.h"
#include "mini_rtos/kernel.h"
#include "mini_rtos/types.h"

static int s_failures;

#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                         \
        }                                                         \
    } while (0)

static _Alignas(8) uint8_t s_mem[512];
static mrt_heap_t s_heap;

static uint32_t free_of(mrt_heap_handle_t h)
{
    uint32_t n = 0u;
    if (mrt_heap_free_bytes(h, &n) != MRT_OK) {
        printf("FAIL %s:%d: free_bytes failed\n", __FILE__, __LINE__);
        s_failures++;
    }
    return n;
}

int main(void)
{
    mrt_heap_handle_t h = NULL;

    CHECK(mrt_heap_create_static(s_mem, sizeof(s_mem),
                                 &s_heap, &h) == MRT_ERR_STATE);
    CHECK(mrt_kernel_init() == MRT_OK);

    /* Validation. */
    CHECK(mrt_heap_create_static(NULL, sizeof(s_mem),
                                 &s_heap, &h) == MRT_ERR_INVAL);
    CHECK(mrt_heap_create_static(s_mem, sizeof(s_mem),
                                 NULL, &h) == MRT_ERR_INVAL);
    CHECK(mrt_heap_create_static(s_mem, sizeof(s_mem),
                                 &s_heap, NULL) == MRT_ERR_INVAL);
    CHECK(mrt_heap_create_static(s_mem, 16u,
                                 &s_heap, &h) == MRT_ERR_INVAL);
    CHECK(mrt_heap_create_static(s_mem, sizeof(s_mem),
                                 &s_heap, &h) == MRT_OK);
    CHECK(mrt_heap_free_bytes(NULL, NULL) == MRT_ERR_INVAL);
    {
        uint32_t n = 0u;
        CHECK(mrt_heap_free_bytes(h, NULL) == MRT_ERR_INVAL);
        CHECK(mrt_heap_free_bytes(h, &n) == MRT_OK);
    }

    /* Measure the per-block overhead empirically: one 8-byte alloc. */
    uint32_t f0 = free_of(h);
    void *probe = mrt_heap_alloc(h, 8u);
    CHECK(probe != NULL);
    uint32_t overhead = f0 - free_of(h) - 8u;
    CHECK(mrt_heap_release(h, probe) == MRT_OK);
    CHECK(free_of(h) == f0); /* release restores exactly */

    /* Split + coalesce: a, b, c out; b, a home (fusing a 2-block run); a
     * double-sized request fits ONLY if the fusion really happened. */
    void *a = mrt_heap_alloc(h, 64u);
    void *b = mrt_heap_alloc(h, 64u);
    void *c = mrt_heap_alloc(h, 64u);
    CHECK(a != NULL && b != NULL && c != NULL);
    CHECK(a != b && b != c && a != c);
    CHECK(free_of(h) == f0 - 3u * (64u + overhead));
    CHECK(mrt_heap_release(h, b) == MRT_OK);
    CHECK(mrt_heap_release(h, a) == MRT_OK);
    void *big = mrt_heap_alloc(h, 128u);
    CHECK(big != NULL);
    CHECK(big == a); /* first-fit reuses the fused run at its address */
    CHECK(mrt_heap_release(h, big) == MRT_OK);
    CHECK(mrt_heap_release(h, c) == MRT_OK);
    CHECK(free_of(h) == f0); /* everything home: byte-exact */

    /* OOM and misuse. */
    CHECK(mrt_heap_alloc(h, 100000u) == NULL);
    CHECK(mrt_heap_alloc(h, 0u) == NULL);
    CHECK(mrt_heap_alloc(NULL, 8u) == NULL);
    CHECK(mrt_heap_release(h, NULL) == MRT_OK); /* C-compatible no-op */
    CHECK(mrt_heap_release(NULL, a) == MRT_ERR_INVAL);
    {
        uint8_t foreign[32];
        CHECK(mrt_heap_release(h, foreign) == MRT_ERR_INVAL);
    }
    /* Alignment contract: every payload starts at an 8-multiple offset
     * from the arena base, on EVERY platform. Bare sizeof(header) is 16
     * on 64-bit hosts (a multiple — invisible) but 10 on AVR / 12 on
     * 32-bit ARM, where the old code served misaligned payloads that
     * release() then rejected (Uno selftest fail 9). A 32-bit host build
     * fails this test without the header-stride fix. */
    void *d = mrt_heap_alloc(h, 32u);
    CHECK(d != NULL);
    CHECK((((uintptr_t)d - (uintptr_t)s_mem) % 8u) == 0u);
    CHECK(mrt_heap_release(h, d) == MRT_OK);
    CHECK(mrt_heap_release(h, d) == MRT_ERR_INVAL); /* double release */
    CHECK(mrt_heap_release(h, (uint8_t *)d + 1) == MRT_ERR_INVAL);
    CHECK(free_of(h) == f0);

    if (s_failures == 0) {
        printf("PASS test_heap\n");
    }
    return s_failures != 0;
}
