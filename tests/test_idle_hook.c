/*
 * mini_rtos — tests/test_idle_hook.c
 *
 * WHAT:  Idle-task behavior: the hook runs exactly on idle passes, never
 *        when work is ready, and reset clears it.
 * WHY:   The idle task is invisible by design (no table slot, no handle),
 *        so the hook is the only window into it. These tests prove the
 *        window is truthful: one call per idle pass, zero otherwise — the
 *        property sleep accounting and watchdog kicks will rely on.
 * HOW:   Counting hook plus exact-log checks that idle never disturbs
 *        rotation or timing.
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
    mrt_tick_t delay; /* 0 = yield, else delay this many ticks. */
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

static int s_hook_calls;

static void counting_hook(void)
{
    s_hook_calls++;
}

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

    /* A. Lone delay-3 task over 6 passes: W, idle, idle, W, idle, idle —
     * the hook fires exactly on the 4 idle passes. */
    mrt_test_reset();
    log_reset();
    s_hook_calls = 0;
    mrt_kernel_set_idle_hook(counting_hook);
    make("w", &s_tcb_a, &s_cfg_a, 'W', 3u, 1u);
    CHECK(mrt_kernel_run_ticks(6u) == MRT_OK);
    check_log("WW");
    CHECK(s_hook_calls == 4);

    /* B. Always-ready tasks: rotation is untouched and the hook never runs. */
    mrt_test_reset();
    log_reset();
    s_hook_calls = 0;
    mrt_kernel_set_idle_hook(counting_hook);
    make("a", &s_tcb_a, &s_cfg_a, 'A', 0u, 1u);
    make("b", &s_tcb_b, &s_cfg_b, 'B', 0u, 1u);
    CHECK(mrt_kernel_run_ticks(4u) == MRT_OK);
    check_log("ABAB");
    CHECK(s_hook_calls == 0);

    /* C. Reset clears the hook: set it, reset, run idle-heavy — zero calls,
     * and the idle task still keeps time flowing (WW on schedule). */
    mrt_kernel_set_idle_hook(counting_hook);
    mrt_test_reset();
    log_reset();
    s_hook_calls = 0;
    make("w", &s_tcb_a, &s_cfg_a, 'W', 2u, 1u);
    CHECK(mrt_kernel_run_ticks(4u) == MRT_OK);
    check_log("WW");
    CHECK(s_hook_calls == 0);

    /* D. Clearing mid-run stops the calls; the kernel does not care. */
    mrt_test_reset();
    log_reset();
    s_hook_calls = 0;
    mrt_kernel_set_idle_hook(counting_hook);
    make("w", &s_tcb_a, &s_cfg_a, 'W', 2u, 1u);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK); /* W, idle: hook == 1. */
    CHECK(s_hook_calls == 1);
    mrt_kernel_set_idle_hook(NULL);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK); /* W, idle: hook stays 1. */
    CHECK(s_hook_calls == 1);
    check_log("WW");

    if (s_failures == 0) {
        printf("PASS test_idle_hook\n");
    }
    return s_failures != 0;
}
