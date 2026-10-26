/*
 * mini_rtos — tests/test_mutex.c
 *
 * WHAT:  Mutex behavior: basic lock/unlock, self-deadlock and non-owner
 *        errors, contended handoff, suspend-abandonment, and — the center
 *        piece — priority inheritance ending an inversion.
 * WHY:   Inheritance is the subtlest code in Phase 2: a boost applied on
 *        block, preserved across picks, and cleared exactly at unlock. The
 *        inversion trace below proves all three observably: the high task's
 *        mark jumps ahead of the medium task's whole burst, which is
 *        impossible without the boost (the "without PI" order is derived in
 *        the case comment for contrast).
 * HOW:   Exact-log scripting plus recorded observations (wait spans, the
 *        boosted priority read mid-hold, post-unlock restoration).
 */
#include <stdio.h>
#include <string.h>

#include "mini_rtos/kernel.h"
#include "mini_rtos/mutex.h"
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

static mrt_tcb_t s_tcb_a, s_tcb_b, s_tcb_c;
static mrt_mutex_t s_mutex;

static mrt_err_t s_r1, s_r2, s_u1, s_u2;

/* Exercises every basic outcome and parks. */
static void ops_task(void *arg)
{
    mrt_mutex_handle_t m = (mrt_mutex_handle_t)arg;
    s_r1 = mrt_mutex_lock(m, MRT_WAIT_FOREVER);
    s_r2 = mrt_mutex_lock(m, 0u); /* self-deadlock */
    s_u1 = mrt_mutex_unlock(m);
    s_u2 = mrt_mutex_unlock(m); /* not owner (free) */
    emit('M');
    mrt_task_delay_ticks(100u);
}

/* Holds the mutex across a 3-tick delay, then releases and parks. */
static void holder_task(void *arg)
{
    mrt_mutex_handle_t m = (mrt_mutex_handle_t)arg;
    mrt_mutex_lock(m, MRT_WAIT_FOREVER);
    emit('l');
    mrt_task_delay_ticks(3u);
    mrt_mutex_unlock(m);
    mrt_task_delay_ticks(100u);
}

static mrt_tick_t s_wait_before, s_wait_after;

/* Arrives late, blocks, measures the wait, releases, parks. */
static void waiter_task(void *arg)
{
    mrt_mutex_handle_t m = (mrt_mutex_handle_t)arg;
    mrt_task_delay_ticks(1u);
    s_wait_before = mrt_task_get_tick();
    mrt_mutex_lock(m, MRT_WAIT_FOREVER);
    s_wait_after = mrt_task_get_tick();
    emit('H');
    mrt_mutex_unlock(m);
    mrt_task_delay_ticks(100u);
}

/* Inversion cast: low holds while READY-spinning (two yields); mid hogs
 * three passes; high arrives mid-hold. Priorities 1/2/3. */
static mrt_prio_t s_low_mid_hold_prio;

static void low_task(void *arg)
{
    mrt_mutex_handle_t m = (mrt_mutex_handle_t)arg;
    mrt_mutex_lock(m, MRT_WAIT_FOREVER);
    emit('l');
    mrt_task_yield(); /* pass 1 of spinning while holding */
    s_low_mid_hold_prio = mrt_task_get_priority(NULL); /* boosted? */
    mrt_task_yield(); /* pass 2 */
    mrt_mutex_unlock(m);
    mrt_task_delay_ticks(100u);
}

static void mid_task(void *arg)
{
    (void)arg;
    mrt_task_delay_ticks(2u); /* let low acquire first */
    emit('M');
    mrt_task_yield();
    emit('M');
    mrt_task_yield();
    emit('M');
    mrt_task_delay_ticks(100u);
}

static void high_task(void *arg)
{
    mrt_mutex_handle_t m = (mrt_mutex_handle_t)arg;
    mrt_task_delay_ticks(3u); /* arrive mid-hold */
    mrt_mutex_lock(m, MRT_WAIT_FOREVER);
    emit('H');
    mrt_mutex_unlock(m);
    mrt_task_delay_ticks(100u);
}

/* Tries to unlock someone else's mutex, recording the verdict. */
static mrt_err_t s_unlock_recorded;

static void rogue_unlock_task(void *arg)
{
    mrt_mutex_handle_t m = (mrt_mutex_handle_t)arg;
    s_unlock_recorded = mrt_mutex_unlock(m);
    mrt_task_delay_ticks(100u);
}

/* Locks and parks forever holding (abandon-test holder). */
static void park_holding_task(void *arg)
{
    mrt_mutex_handle_t m = (mrt_mutex_handle_t)arg;
    mrt_mutex_lock(m, MRT_WAIT_FOREVER);
    emit('l');
    mrt_task_delay_ticks(100u);
}

/* Blocks on the mutex, recording whatever ends the wait. */
static mrt_err_t s_lock_recorded;

static void abandon_lock_task(void *arg)
{
    mrt_mutex_handle_t m = (mrt_mutex_handle_t)arg;
    s_lock_recorded = mrt_mutex_lock(m, MRT_WAIT_FOREVER);
    emit('h');
    mrt_task_delay_ticks(100u);
}

int main(void)
{
    mrt_mutex_handle_t h = NULL;

    CHECK(mrt_mutex_create_static(&s_mutex, &h) == MRT_ERR_STATE);
    CHECK(mrt_kernel_init() == MRT_OK);

    CHECK(mrt_mutex_create_static(NULL, &h) == MRT_ERR_INVAL);
    CHECK(mrt_mutex_create_static(&s_mutex, NULL) == MRT_ERR_INVAL);

    /* Basic outcomes, all four recorded. */
    mrt_test_reset();
    log_reset();
    s_r1 = s_r2 = s_u1 = s_u2 = MRT_OK;
    CHECK(mrt_mutex_create_static(&s_mutex, &h) == MRT_OK);
    mrt_task_handle_t hm = NULL;
    CHECK(mrt_task_create_static("m", ops_task, h, 1u,
                                 &s_tcb_a, &hm) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK);
    check_log("M");
    CHECK(s_r1 == MRT_OK);
    CHECK(s_r2 == MRT_ERR_STATE);
    CHECK(s_u1 == MRT_OK);
    CHECK(s_u2 == MRT_ERR_STATE);

    /* Contended handoff: H blocks at tick 2, owns at 4 — a 2-tick wait
     * ended by direct handoff, and the log order proves serialization. */
    mrt_test_reset();
    log_reset();
    s_wait_before = s_wait_after = 0u;
    CHECK(mrt_mutex_create_static(&s_mutex, &h) == MRT_OK);
    mrt_task_handle_t hl = NULL;
    mrt_task_handle_t hh = NULL;
    CHECK(mrt_task_create_static("l", holder_task, h, 1u,
                                 &s_tcb_a, &hl) == MRT_OK);
    CHECK(mrt_task_create_static("h", waiter_task, h, 1u,
                                 &s_tcb_b, &hh) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(6u) == MRT_OK);
    check_log("lH");
    CHECK(s_wait_before == 2u);
    CHECK(s_wait_after == 4u);

    /* Priority inheritance ends the inversion. Trace WITH PI: H delays,
     * M delays, L locks+spins; H blocks and boosts L 1->3; L's spins beat
     * M(2); L unlocks handing to H; H runs; M's burst lands last: "lHMMM".
     * WITHOUT the boost, M would win passes 4-6 and the log would read
     * "lMMMH" — the test asserts the PI order, plus the mid-hold boost
     * read (3) and post-unlock restoration (1). */
    mrt_test_reset();
    log_reset();
    s_low_mid_hold_prio = 0u;
    CHECK(mrt_mutex_create_static(&s_mutex, &h) == MRT_OK);
    mrt_task_handle_t hL = NULL;
    mrt_task_handle_t hM = NULL;
    mrt_task_handle_t hH = NULL;
    CHECK(mrt_task_create_static("low", low_task, h, 1u,
                                 &s_tcb_a, &hL) == MRT_OK);
    CHECK(mrt_task_create_static("mid", mid_task, NULL, 2u,
                                 &s_tcb_b, &hM) == MRT_OK);
    CHECK(mrt_task_create_static("high", high_task, h, 3u,
                                 &s_tcb_c, &hH) == MRT_OK);
    (void)hM;
    (void)hH;
    CHECK(mrt_kernel_run_ticks(10u) == MRT_OK);
    check_log("lHMMM");
    CHECK(s_low_mid_hold_prio == 3u);
    CHECK(mrt_task_get_priority(hL) == 1u);

    /* Non-owner unlock refused while another task holds. */
    mrt_test_reset();
    s_unlock_recorded = MRT_OK;
    CHECK(mrt_mutex_create_static(&s_mutex, &h) == MRT_OK);
    CHECK(mrt_task_create_static("o", park_holding_task, h, 1u,
                                 &s_tcb_a, &hl) == MRT_OK);
    CHECK(mrt_task_create_static("x", rogue_unlock_task, h, 1u,
                                 &s_tcb_b, &hh) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(3u) == MRT_OK);
    CHECK(s_unlock_recorded == MRT_ERR_STATE);

    /* Suspend-while-blocked: H's abandoned lock returns STATE on resume. */
    mrt_test_reset();
    log_reset();
    s_lock_recorded = MRT_OK;
    CHECK(mrt_mutex_create_static(&s_mutex, &h) == MRT_OK);
    CHECK(mrt_task_create_static("l", park_holding_task, h, 1u,
                                 &s_tcb_a, &hl) == MRT_OK);
    CHECK(mrt_task_create_static("h", abandon_lock_task, h, 1u,
                                 &s_tcb_b, &hh) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK); /* L holds, H blocks. */
    CHECK(mrt_task_suspend(hh) == MRT_OK);
    CHECK(mrt_task_resume(hh) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK); /* H reports STATE. */
    check_log("lh");
    CHECK(s_lock_recorded == MRT_ERR_STATE);

    if (s_failures == 0) {
        printf("PASS test_mutex\n");
    }
    return s_failures != 0;
}
