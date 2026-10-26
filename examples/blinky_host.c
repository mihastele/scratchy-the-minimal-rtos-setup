/*
 * mini_rtos — examples/blinky_host.c
 *
 * WHAT:  The "hello world" of the OS: two tasks printing at different rates
 *        on the host simulator.
 * WHY:   The smallest program that proves the whole stack — init, create,
 *        schedule, delay — works end to end. Run this first on every new
 *        port: if the interleave looks right, the port's context switch and
 *        tick both work.
 * HOW:   Task A prints every 2 ticks, B every 3, same priority so they share
 *        fairly. The run is bounded to 12 ticks so the program exits; on a
 *        microcontroller main() would call mrt_kernel_start() instead and
 *        never return.
 */
#include <stdio.h>

#include "mini_rtos/kernel.h"
#include "mini_rtos/task.h"
#include "mini_rtos/types.h"

/* TCBs live in static storage: they must outlive the tasks, and on an MCU
 * there is no heap to put them on anyway. */
static mrt_tcb_t s_tcb_a;
static mrt_tcb_t s_tcb_b;

static void task_a(void *arg)
{
    (void)arg; /* Unused: this task needs no context. */
    for (;;) {
        printf("A tick=%lu\n", (unsigned long)mrt_task_get_tick());
        mrt_task_delay_ticks(2u);
    }
}

static void task_b(void *arg)
{
    (void)arg;
    for (;;) {
        printf("B tick=%lu\n", (unsigned long)mrt_task_get_tick());
        mrt_task_delay_ticks(3u);
    }
}

int main(void)
{
    /* Boot order is a contract, not a suggestion: init, then create, then
     * run. Every step is checked — firmware that ignores boot errors fails
     * in the field instead of on the bench. */
    if (mrt_kernel_init() != MRT_OK) {
        fprintf(stderr, "kernel init failed\n");
        return 1;
    }

    mrt_task_handle_t h = NULL;
    if (mrt_task_create_static("blink_a", task_a, NULL, 1u,
                               &s_tcb_a, &h) != MRT_OK) {
        fprintf(stderr, "create A failed\n");
        return 1;
    }
    if (mrt_task_create_static("blink_b", task_b, NULL, 1u,
                               &s_tcb_b, &h) != MRT_OK) {
        fprintf(stderr, "create B failed\n");
        return 1;
    }

    if (mrt_kernel_run_ticks(12u) != MRT_OK) {
        fprintf(stderr, "run failed\n");
        return 1;
    }

    printf("done at tick=%lu\n", (unsigned long)mrt_task_get_tick());
    return 0;
}
