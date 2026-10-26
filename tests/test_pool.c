/*
 * mini_rtos — tests/test_pool.c
 *
 * WHAT:  Fixed-block pools: LIFO order, exhaustion, validation (sizes,
 *        alignment, short arenas), double-return and foreign-pointer
 *        detection, and ISR-context legality.
 * WHY:   The allocator is the last place you want silent corruption: every
 *        misuse below must fail AT the call with INVAL, and the happy path
 *        must be exactly LIFO (deterministic reuse, cache-hot blocks).
 * HOW:   Main-line calls (pools need no task context) plus one fake-ISR
 *        section. Pointer-identity assertions pin the LIFO order.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "mini_rtos/kernel.h"
#include "mini_rtos/pool.h"
#include "mini_rtos/types.h"
#include "test_hooks.h"

static int s_failures;

#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                         \
        }                                                         \
    } while (0)

/* Explicit alignment: the pool demands a pointer-aligned arena, so the
 * test says so instead of relying on linker luck. */
static _Alignas(sizeof(void *)) uint8_t s_mem[3u * 16u];
static mrt_pool_t s_pool;

int main(void)
{
    mrt_pool_handle_t h = NULL;

    CHECK(mrt_pool_create_static(3u, 16u, s_mem, sizeof(s_mem),
                                 &s_pool, &h) == MRT_ERR_STATE);
    CHECK(mrt_kernel_init() == MRT_OK);

    /* Validation: every bad shape refused. */
    CHECK(mrt_pool_create_static(0u, 16u, s_mem, sizeof(s_mem),
                                 &s_pool, &h) == MRT_ERR_INVAL);
    CHECK(mrt_pool_create_static(3u, 1u, s_mem, sizeof(s_mem),
                                 &s_pool, &h) == MRT_ERR_INVAL);
    CHECK(mrt_pool_create_static(3u, (uint16_t)(sizeof(void *) + 1u),
                                 s_mem, sizeof(s_mem),
                                 &s_pool, &h) == MRT_ERR_INVAL);
    CHECK(mrt_pool_create_static(3u, 16u, NULL, sizeof(s_mem),
                                 &s_pool, &h) == MRT_ERR_INVAL);
    CHECK(mrt_pool_create_static(3u, 16u, s_mem, 16u,
                                 &s_pool, &h) == MRT_ERR_INVAL);
    CHECK(mrt_pool_create_static(3u, 16u, &s_mem[1], sizeof(s_mem) - 1u,
                                 &s_pool, &h) == MRT_ERR_INVAL);
    CHECK(mrt_pool_create_static(3u, 16u, s_mem, sizeof(s_mem),
                                 NULL, &h) == MRT_ERR_INVAL);

    /* LIFO order with pointer identity: out 0,1,2 — back 1,0 — out 0,1. */
    CHECK(mrt_pool_create_static(3u, 16u, s_mem, sizeof(s_mem),
                                 &s_pool, &h) == MRT_OK);
    CHECK(mrt_pool_free_count(h) == 3u);
    void *p0 = mrt_pool_alloc(h);
    void *p1 = mrt_pool_alloc(h);
    void *p2 = mrt_pool_alloc(h);
    CHECK(p0 == &s_mem[0u]);
    CHECK(p1 == &s_mem[16u]);
    CHECK(p2 == &s_mem[32u]);
    CHECK(mrt_pool_free_count(h) == 0u);
    CHECK(mrt_pool_alloc(h) == NULL); /* empty: documented OOM */
    CHECK(mrt_pool_free(h, p1) == MRT_OK);
    CHECK(mrt_pool_free(h, p0) == MRT_OK);
    CHECK(mrt_pool_free_count(h) == 2u);
    CHECK(mrt_pool_alloc(h) == p0);
    CHECK(mrt_pool_alloc(h) == p1);
    CHECK(mrt_pool_free_count(h) == 0u);

    /* Misuse, all caught: double return, foreign, mid-block, NULLs. */
    CHECK(mrt_pool_free(h, p0) == MRT_OK);
    CHECK(mrt_pool_free(h, p0) == MRT_ERR_INVAL); /* already home */
    CHECK(mrt_pool_free(h, &s_mem[4]) == MRT_ERR_INVAL); /* mid-block */
    {
        uint8_t foreign[16];
        CHECK(mrt_pool_free(h, foreign) == MRT_ERR_INVAL);
    }
    CHECK(mrt_pool_free(h, NULL) == MRT_ERR_INVAL);
    CHECK(mrt_pool_free(NULL, p0) == MRT_ERR_INVAL);
    CHECK(mrt_pool_alloc(NULL) == NULL);

    /* ISR legality: return the last two blocks (pool full again), then
     * drain and return one block under fake interrupts. */
    CHECK(mrt_pool_free(h, p1) == MRT_OK);
    CHECK(mrt_pool_free(h, p2) == MRT_OK);
    CHECK(mrt_pool_free_count(h) == 3u);
    mrt_test_isr_enter();
    void *q0 = mrt_pool_alloc(h);
    void *q1 = mrt_pool_alloc(h);
    void *q2 = mrt_pool_alloc(h);
    CHECK(q0 != NULL && q1 != NULL && q2 != NULL);
    CHECK(q0 != q1 && q1 != q2 && q0 != q2);
    CHECK(mrt_pool_alloc(h) == NULL);
    CHECK(mrt_pool_free(h, q2) == MRT_OK);
    CHECK(mrt_pool_free_count(h) == 1u);
    mrt_test_isr_exit();

    if (s_failures == 0) {
        printf("PASS test_pool\n");
    }
    return s_failures != 0;
}
