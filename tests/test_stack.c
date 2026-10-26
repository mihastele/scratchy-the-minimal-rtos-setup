/*
 * mini_rtos — tests/test_stack.c
 *
 * WHAT:  Stack high-water marks: deeper call trees leave smaller "unused"
 *        readings, a fresh task reads nearly full, and the query validates.
 * WHY:   Watermarks turn stack sizing from folklore into measurement — but
 *        only if the paint/count machinery is truthful. Recursion depth is
 *        the honest signal: exact bytes depend on codegen, ORDER does not,
 *        so the test asserts order with wide margins, never magic numbers.
 * HOW:   One task measures itself at two recursion depths (10 vs 30 frames
 *        of 256 bytes), one virgin task measures on arrival, main checks
 *        the error paths.
 */
#include <stdint.h>
#include <stdio.h>

#include "mini_rtos/config.h"
#include "mini_rtos/kernel.h"
#include "mini_rtos/task.h"
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

static mrt_tcb_t s_tcb_a, s_tcb_b;
static mrt_tcb_t s_rogue;
static uint32_t s_shallow, s_deep, s_virgin;

/* Recurse to `target`, measuring at the deepest frame. The volatile
 * 256-byte frame forces real stack residency at any optimization level,
 * and the post-call touch defeats tail-call elimination. */
static void burn(int depth, int target, uint32_t *out)
{
    volatile uint8_t buf[256];
    buf[0] = (uint8_t)depth;
    buf[255] = (uint8_t)(depth + 1);
    if (depth < target) {
        burn(depth + 1, target, out);
        buf[(uint8_t)depth] += 1u;
    } else {
        uint32_t u = 0u;
        if (mrt_task_stack_unused(NULL, &u) == MRT_OK) {
            *out = u;
        }
    }
}

static void measure_task(void *arg)
{
    (void)arg;
    s_shallow = 0u;
    s_deep = 0u;
    burn(0, 10, &s_shallow);
    burn(0, 30, &s_deep);
    mrt_task_delay_ticks(100u);
}

static void virgin_task(void *arg)
{
    (void)arg;
    s_virgin = 0u;
    mrt_task_stack_unused(NULL, &s_virgin);
    mrt_task_delay_ticks(100u);
}

int main(void)
{
    CHECK(mrt_kernel_init() == MRT_OK);

    mrt_test_reset();
    s_shallow = s_deep = s_virgin = 0u;
    mrt_task_handle_t hv = NULL;
    mrt_task_handle_t hm = NULL;
    CHECK(mrt_task_create_static("v", virgin_task, NULL, 1u,
                                 &s_tcb_a, &hv) == MRT_OK);
    CHECK(mrt_task_create_static("m", measure_task, NULL, 1u,
                                 &s_tcb_b, &hm) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(3u) == MRT_OK);

    /* Order with margin: 20 extra 256-byte frames must cost far more than
     * 1 KB of paint, whatever the codegen. */
    CHECK(s_shallow > 0u && s_deep > 0u);
    CHECK(s_deep + 1024u < s_shallow);
    /* Sanity bounds: barely dented overall, virgin nearly pristine. */
    CHECK(s_shallow < MRT_HOST_TASK_STACK_SIZE);
    CHECK(s_deep > MRT_HOST_TASK_STACK_SIZE / 2u);
    CHECK(s_virgin > MRT_HOST_TASK_STACK_SIZE - 4096u);

    /* Query from outside the task, plus error paths. */
    {
        uint32_t u = 0u;
        CHECK(mrt_task_stack_unused(hm, &u) == MRT_OK);
        CHECK(u > 0u);
        CHECK(mrt_task_stack_unused(hm, NULL) == MRT_ERR_INVAL);
        CHECK(mrt_task_stack_unused(&s_rogue, &u) == MRT_ERR_INVAL);
        CHECK(mrt_task_stack_unused(NULL, &u) == MRT_ERR_STATE);
    }

    if (s_failures == 0) {
        printf("PASS test_stack\n");
    }
    return s_failures != 0;
}
