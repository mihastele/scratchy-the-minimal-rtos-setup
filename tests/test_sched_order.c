/*
 * mini_rtos — tests/test_sched_order.c
 *
 * WHAT:  Scheduler behavior tests: priority order, round-robin, delays,
 *        32-bit tick wraparound, task-return, and the delay(0) convention.
 * WHY:   Scheduling ORDER is the kernel's core promise. Exact-sequence
 *        assertions ("the log must read DYDYDY") are the strongest form —
 *        order bugs cannot hide behind "eventually ran" vagueness, and the
 *        host's deterministic virtual tick makes sequences reproducible.
 * HOW:   Tasks append one char per action to a log; each case resets the
 *        kernel, builds a tiny task set, runs a bounded tick count, and
 *        asserts the exact log. One binary, fresh process, no flakes.
 */
#include <stdint.h>
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

/* --- Event log --- */

static char s_log[64];
static size_t s_len;

static void log_reset(void)
{
    s_len = 0u;
    s_log[0] = '\0';
}

static void emit(char c)
{
    /* Silently cap instead of overflowing: a runaway log means the case is
     * already broken, and the mismatch below will report it. */
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

/* --- Task bodies --- */

typedef struct {
    char mark;         /* Char to emit each iteration. */
    mrt_tick_t delay;  /* 0 = yield, else delay this many ticks. */
} task_cfg_t;

static void emitting_task(void *arg)
{
    task_cfg_t *cfg = (task_cfg_t *)arg;
    for (;;) {
        emit(cfg->mark);
        if (cfg->delay == 0u) {
            mrt_task_yield();
        } else {
            mrt_task_delay_ticks(cfg->delay);
        }
    }
}

/* Exercises the delay(0)-is-a-yield path specifically (emitting_task with
 * delay 0 would take the yield branch instead — different code path). */
static void delay_zero_task(void *arg)
{
    char mark = *(char *)arg;
    for (;;) {
        emit(mark);
        mrt_task_delay_ticks(0u);
    }
}

/* Emits once and RETURNS — exercises the task-finished path. */
static void oneshot_task(void *arg)
{
    emit(*(char *)arg);
}

/* --- Fixture --- */

static mrt_tcb_t s_tcb_a, s_tcb_b;
static task_cfg_t s_cfg_a, s_cfg_b;

static void make(const char *name, mrt_tcb_t *tcb, task_cfg_t *cfg,
                 char mark, mrt_tick_t delay, mrt_prio_t prio)
{
    cfg->mark = mark;
    cfg->delay = delay;
    mrt_task_handle_t h = NULL;
    if (mrt_task_create_static(name, emitting_task, cfg, prio,
                               tcb, &h) != MRT_OK) {
        printf("FAIL %s:%d: create %s failed\n", __FILE__, __LINE__, name);
        s_failures++;
    }
}

int main(void)
{
    CHECK(mrt_kernel_init() == MRT_OK);

    /* A. Priority beats rotation: the hotter task wins every pass. */
    mrt_test_reset();
    log_reset();
    make("hi", &s_tcb_a, &s_cfg_a, 'H', 0u, 2u);
    make("lo", &s_tcb_b, &s_cfg_b, 'L', 0u, 1u);
    CHECK(mrt_kernel_run_ticks(3u) == MRT_OK);
    check_log("HHH");

    /* B. Equal priority rotates strictly. */
    mrt_test_reset();
    log_reset();
    make("a", &s_tcb_a, &s_cfg_a, 'A', 0u, 1u);
    make("b", &s_tcb_b, &s_cfg_b, 'B', 0u, 1u);
    CHECK(mrt_kernel_run_ticks(4u) == MRT_OK);
    check_log("ABAB");

    /* C. Delay sleeps exactly 2 ticks while the peer runs. Trace: D runs
     * (wake=2), Y runs, D wakes and runs (wake=4), Y, D (wake=6), Y. */
    mrt_test_reset();
    log_reset();
    make("d", &s_tcb_a, &s_cfg_a, 'D', 2u, 1u);
    make("y", &s_tcb_b, &s_cfg_b, 'Y', 0u, 1u);
    CHECK(mrt_kernel_run_ticks(6u) == MRT_OK);
    check_log("DYDYDY");
    CHECK(mrt_task_get_tick() == 6u);

    /* D. Delay across the 32-bit wrap wakes on time. Start at MAX-1, delay
     * 3 (wake tick wraps to 1): W runs, idles through MAX and 0, runs at 1,
     * idles at 2. A naive `now >= wake` comparison would sleep 49 days. */
    mrt_test_reset();
    log_reset();
    mrt_test_set_tick(UINT32_MAX - 1u);
    make("w", &s_tcb_a, &s_cfg_a, 'W', 3u, 1u);
    CHECK(mrt_kernel_run_ticks(5u) == MRT_OK);
    check_log("WW");
    CHECK(mrt_task_get_tick() == 3u);

    /* E. A returned task never runs again; the peer continues. */
    mrt_test_reset();
    log_reset();
    static char mark_r = 'R';
    mrt_task_handle_t h = NULL;
    CHECK(mrt_task_create_static("r", oneshot_task, &mark_r, 1u,
                                 &s_tcb_a, &h) == MRT_OK);
    make("y", &s_tcb_b, &s_cfg_b, 'Y', 0u, 1u);
    CHECK(mrt_kernel_run_ticks(4u) == MRT_OK);
    check_log("RYYY");

    /* F. delay(0) behaves exactly like a yield in rotation order. */
    mrt_test_reset();
    log_reset();
    static char mark_z = 'Z';
    CHECK(mrt_task_create_static("z", delay_zero_task, &mark_z, 1u,
                                 &s_tcb_a, &h) == MRT_OK);
    make("y", &s_tcb_b, &s_cfg_b, 'Y', 0u, 1u);
    CHECK(mrt_kernel_run_ticks(4u) == MRT_OK);
    check_log("ZYZY");

    if (s_failures == 0) {
        printf("PASS test_sched_order\n");
    }
    return s_failures != 0;
}
