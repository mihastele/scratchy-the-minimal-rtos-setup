/*
 * mini_rtos — tests/test_suspend_resume.c
 *
 * WHAT:  Suspend/resume behavior: parking another task, self-suspend with
 *        peer resume, delay abandonment, and every error path.
 * WHY:   Suspend is the lifecycle primitive everything else builds on
 *        (finished tasks, delete reaping, power management). Its edge table
 *        — self vs other, READY vs DELAYED, rogue handles — must be pinned
 *        before IPC and delete lean on it in later phases.
 * HOW:   Event-log sequences for ordering plus direct return-code checks.
 *        Cross-task calls (suspend/resume from main between runs) are legal:
 *        they only flip state, never block.
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

static void self_suspender(void *arg)
{
    (void)arg;
    for (;;) {
        emit('S');
        mrt_task_suspend(NULL); /* Park ourselves; the peer wakes us. */
    }
}

static int s_p_runs;
static mrt_task_handle_t s_h_s;

static void resumer(void *arg)
{
    (void)arg;
    for (;;) {
        emit('P');
        s_p_runs++;
        if (s_p_runs % 2 == 0) {
            mrt_task_resume(s_h_s);
        }
        mrt_task_yield();
    }
}

/* --- Fixture --- */

static mrt_tcb_t s_tcb_a, s_tcb_b;
static task_cfg_t s_cfg_a, s_cfg_b;
static mrt_tcb_t s_rogue; /* Never created: NOT a valid handle. */

static mrt_task_handle_t make(const char *name, mrt_tcb_t *tcb,
                              task_cfg_t *cfg, char mark, mrt_tick_t delay,
                              mrt_prio_t prio)
{
    cfg->mark = mark;
    cfg->delay = delay;
    mrt_task_handle_t h = NULL;
    if (mrt_task_create_static(name, emitting_task, cfg, prio,
                               tcb, &h) != MRT_OK) {
        printf("FAIL %s:%d: create %s failed\n", __FILE__, __LINE__, name);
        s_failures++;
    }
    return h;
}

int main(void)
{
    CHECK(mrt_kernel_init() == MRT_OK);

    /* A. Suspend another task from outside: it vanishes from the rotation
     * until resumed. Trace: XY, suspend X, YY, resume X, XY. */
    mrt_test_reset();
    log_reset();
    mrt_task_handle_t hX = make("x", &s_tcb_a, &s_cfg_a, 'X', 0u, 1u);
    make("y", &s_tcb_b, &s_cfg_b, 'Y', 0u, 1u);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK);
    CHECK(mrt_task_suspend(hX) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK);
    CHECK(mrt_task_resume(hX) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK);
    check_log("XYYYXY");

    /* B. Self-suspend with peer resume. S runs, parks; P runs twice and
     * wakes S on its second run; repeat. Trace over 7 passes: SPPSPPS. */
    mrt_test_reset();
    log_reset();
    s_p_runs = 0;
    mrt_task_handle_t hP = NULL;
    CHECK(mrt_task_create_static("s", self_suspender, NULL, 1u,
                                 &s_tcb_a, &s_h_s) == MRT_OK);
    CHECK(mrt_task_create_static("p", resumer, NULL, 1u,
                                 &s_tcb_b, &hP) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(7u) == MRT_OK);
    check_log("SPPSPPS");

    /* C. State machine + error paths. */
    mrt_test_reset();
    mrt_task_handle_t hA = make("a", &s_tcb_a, &s_cfg_a, 'A', 0u, 1u);
    mrt_task_handle_t hD = make("d", &s_tcb_b, &s_cfg_b, 'D', 5u, 1u);
    CHECK(mrt_task_suspend(hA) == MRT_OK);  /* READY -> SUSPENDED */
    CHECK(mrt_task_resume(hA) == MRT_OK);   /* SUSPENDED -> READY */
    CHECK(mrt_task_resume(hA) == MRT_OK);   /* READY: idempotent OK */
    CHECK(mrt_task_suspend(hA) == MRT_OK);  /* Park A so D runs alone. */
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK); /* D emits, delays 5. */
    CHECK(mrt_task_resume(hD) == MRT_ERR_STATE); /* DELAYED: must wait. */
    CHECK(mrt_task_suspend(hD) == MRT_OK);  /* Suspend abandons the delay. */
    CHECK(mrt_task_resume(hD) == MRT_OK);   /* ...so resume is immediate. */
    CHECK(mrt_task_suspend(&s_rogue) == MRT_ERR_INVAL);
    CHECK(mrt_task_resume(&s_rogue) == MRT_ERR_INVAL);
    CHECK(mrt_task_set_priority(&s_rogue, 1u) == MRT_ERR_INVAL);
    CHECK(mrt_task_suspend(NULL) == MRT_ERR_STATE); /* Main is no task. */
    CHECK(mrt_task_set_priority(NULL, 1u) == MRT_ERR_STATE);
    CHECK(mrt_task_resume(NULL) == MRT_ERR_INVAL);

    if (s_failures == 0) {
        printf("PASS test_suspend_resume\n");
    }
    return s_failures != 0;
}
