/*
 * mini_rtos — tests/test_delay_until.c
 *
 * WHAT:  mrt_task_delay_until(): phase-locked periodic delays, overrun
 *        behavior, and the period-0 convention.
 * WHY:   delay_ticks() drifts (loop-body time adds up); delay_until() is
 *        how real firmware keeps a 1 kHz loop at 1 kHz. The overrun rule —
 *        skip the block but keep the phase — is the subtle half and needs
 *        an exact test, not a comment.
 * HOW:   Exact-log interleave checks plus recorded tick observations for
 *        the overrun case (before == after proves "returned immediately").
 */
#include <stdio.h>
#include <string.h>

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

static char s_log[64];
static size_t s_len;

static void log_reset(void)
{
    s_len = 0u;
    s_log[0] = '\0';
}

static void emit(char c)
{
    if (s_len + 1u < sizeof(s_log)) {
        s_log[s_len++] = c;
        s_log[s_len] = '\0';
    }
}

static void check_log(const char *want)
{
    if (strcmp(s_log, want) != 0) {
        printf("FAIL %s:%d: log \"%s\", want \"%s\"\n",
               __FILE__, __LINE__, s_log, want);
        s_failures++;
    }
}

typedef struct {
    char mark;
    mrt_tick_t period;
} task_cfg_t;

/* Textbook periodic loop: anchor on entry, delay_until each round. */
static void periodic_task(void *arg)
{
    task_cfg_t *cfg = (task_cfg_t *)arg;
    mrt_tick_t prev = mrt_task_get_tick();
    for (;;) {
        emit(cfg->mark);
        mrt_task_delay_until(&prev, cfg->period);
    }
}

static void yielding_task(void *arg)
{
    char mark = *(char *)arg;
    for (;;) {
        emit(mark);
        mrt_task_yield();
    }
}

/* Forces an overrun: sleeps 5 with a stale prev anchored at 0, so the
 * period-2 deadline (tick 2) is long past when delay_until runs. */
static mrt_tick_t s_over_before;
static mrt_tick_t s_over_after;
static mrt_tick_t s_over_prev;

static void overrun_task(void *arg)
{
    (void)arg;
    mrt_tick_t prev = 0u;
    mrt_task_delay_ticks(5u);
    s_over_before = mrt_task_get_tick();
    mrt_task_delay_until(&prev, 2u);
    s_over_after = mrt_task_get_tick();
    s_over_prev = prev;
    emit('O');
    for (;;) {
        mrt_task_yield();
    }
}

static void zero_period_task(void *arg)
{
    char mark = *(char *)arg;
    mrt_tick_t prev = 0u;
    for (;;) {
        emit(mark);
        mrt_task_delay_until(&prev, 0u);
    }
}

static mrt_tcb_t s_tcb_a, s_tcb_b;
static task_cfg_t s_cfg_a;
static char s_mark_b;

int main(void)
{
    CHECK(mrt_kernel_init() == MRT_OK);
    mrt_task_handle_t h = NULL;

    /* A. Lone period-3 task fires at ticks 0, 3, 6, 9 over a 10-tick run:
     * exact phase, no drift. */
    mrt_test_reset();
    log_reset();
    s_cfg_a.mark = 'P';
    s_cfg_a.period = 3u;
    CHECK(mrt_task_create_static("p", periodic_task, &s_cfg_a, 1u,
                                 &s_tcb_a, &h) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(10u) == MRT_OK);
    check_log("PPPP");
    CHECK(mrt_task_get_tick() == 10u);

    /* B. Period-3 task beside a yield loop: PYY repeating, phase intact. */
    mrt_test_reset();
    log_reset();
    s_cfg_a.mark = 'P';
    s_cfg_a.period = 3u;
    CHECK(mrt_task_create_static("p", periodic_task, &s_cfg_a, 1u,
                                 &s_tcb_a, &h) == MRT_OK);
    s_mark_b = 'Y';
    CHECK(mrt_task_create_static("y", yielding_task, &s_mark_b, 1u,
                                 &s_tcb_b, &h) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(9u) == MRT_OK);
    check_log("PYYPYYPYY");
    CHECK(mrt_task_get_tick() == 9u);

    /* C. Overrun: no time passes across the call, but prev still advances
     * by exactly one period (phase preserved for the next round). */
    mrt_test_reset();
    log_reset();
    s_over_before = s_over_after = s_over_prev = 0u;
    CHECK(mrt_task_create_static("o", overrun_task, NULL, 1u,
                                 &s_tcb_a, &h) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(8u) == MRT_OK);
    check_log("O");
    CHECK(s_over_before == 5u);
    CHECK(s_over_after == 5u);
    CHECK(s_over_prev == 2u);

    /* D. Period 0 rotates exactly like a yield. */
    mrt_test_reset();
    log_reset();
    s_mark_b = 'Z';
    CHECK(mrt_task_create_static("z", zero_period_task, &s_mark_b, 1u,
                                 &s_tcb_a, &h) == MRT_OK);
    static char mark_y = 'Y';
    CHECK(mrt_task_create_static("y", yielding_task, &mark_y, 1u,
                                 &s_tcb_b, &h) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(4u) == MRT_OK);
    check_log("ZYZY");

    if (s_failures == 0) {
        printf("PASS test_delay_until\n");
    }
    return s_failures != 0;
}
