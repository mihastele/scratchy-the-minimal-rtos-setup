/*
 * mini_rtos — tests/test_priority.c
 *
 * WHAT:  Priority get/set: mid-run changes from outside, and self changes
 *        from inside a task.
 * WHY:   Priorities are only useful if they can change — aging, mode
 *        switches, and (Phase 2) mutex priority inheritance all write them.
 *        This pins the semantics: validated range, next-pick effect, and
 *        the cooperative caveat that self-demotion applies at the next yield
 *        on the host.
 * HOW:   Exact-log rotation checks plus recorded in-task observations.
 */
#include <stdio.h>
#include <string.h>

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

static void emitting_task(void *arg)
{
    char mark = *(char *)arg;
    for (;;) {
        emit(mark);
        mrt_task_yield();
    }
}

/* Raises its own priority mid-run and records before/after for the test. */
static mrt_prio_t s_seen[2];

static void self_prio_task(void *arg)
{
    (void)arg;
    s_seen[0] = mrt_task_get_priority(NULL);
    mrt_task_set_priority(NULL, 5u);
    s_seen[1] = mrt_task_get_priority(NULL);
    for (;;) {
        emit('T');
        mrt_task_yield();
    }
}

static mrt_tcb_t s_tcb_a, s_tcb_b;
static char s_mark_a, s_mark_b;

int main(void)
{
    CHECK(mrt_kernel_init() == MRT_OK);

    /* A. Mid-run change from outside: rotate, monopolize, rotate.
     * Trace: AB, raise A to 3, AAA, lower A to 1, BA (cursor resumes
     * past A, so B goes first). */
    mrt_test_reset();
    log_reset();
    s_mark_a = 'A';
    s_mark_b = 'B';
    mrt_task_handle_t hA = NULL;
    mrt_task_handle_t hB = NULL;
    CHECK(mrt_task_create_static("a", emitting_task, &s_mark_a, 1u,
                                 &s_tcb_a, &hA) == MRT_OK);
    CHECK(mrt_task_create_static("b", emitting_task, &s_mark_b, 1u,
                                 &s_tcb_b, &hB) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK);
    CHECK(mrt_task_set_priority(hA, 3u) == MRT_OK);
    CHECK(mrt_task_get_priority(hA) == 3u);
    CHECK(mrt_kernel_run_ticks(3u) == MRT_OK);
    CHECK(mrt_task_set_priority(hA, 1u) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK);
    check_log("ABAAABA");
    CHECK(mrt_task_get_priority(hA) == 1u);
    CHECK(mrt_task_get_priority(hB) == 1u);
    CHECK(mrt_task_set_priority(hA, MRT_MAX_PRIORITY + 1u) == MRT_ERR_INVAL);

    /* B. A task changes its own priority: sees 2, sets 5, then starves the
     * lower peer until the run ends. */
    mrt_test_reset();
    log_reset();
    s_seen[0] = s_seen[1] = 0u;
    mrt_task_handle_t hT = NULL;
    mrt_task_handle_t hU = NULL;
    CHECK(mrt_task_create_static("t", self_prio_task, NULL, 2u,
                                 &s_tcb_a, &hT) == MRT_OK);
    s_mark_b = 'U';
    CHECK(mrt_task_create_static("u", emitting_task, &s_mark_b, 1u,
                                 &s_tcb_b, &hU) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(3u) == MRT_OK);
    check_log("TTT");
    CHECK(s_seen[0] == 2u);
    CHECK(s_seen[1] == 5u);
    CHECK(mrt_task_get_priority(hT) == 5u);
    (void)hU;

    if (s_failures == 0) {
        printf("PASS test_priority\n");
    }
    return s_failures != 0;
}
