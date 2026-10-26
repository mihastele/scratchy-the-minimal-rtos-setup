/*
 * mini_rtos — tests/test_task_delete.c
 *
 * WHAT:  Task deletion: sync reap of others, idle reap of self, slot and
 *        storage reuse, blocked-task detachment, the mutex-owner guard,
 *        zombie-state guards, and slot/pool churn without leaks.
 * WHY:   Delete is where lifetime bugs hide — use-after-reap, leaked slots,
 *        orphaned mutexes, resurrected zombies. Each case below pins one
 *        promise: immediate reuse for others, exactly-one-idle-pass for
 *        self, and errors (never corruption) for every misuse.
 * HOW:   Exact-log sequencing for ordering, handle-validity probes for
 *        lifetime, and a churn loop that would exhaust any leaked resource.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mini_rtos/config.h"
#include "mini_rtos/kernel.h"
#include "mini_rtos/mutex.h"
#include "mini_rtos/queue.h"
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

static char s_log[80];
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

static mrt_tcb_t s_tcb[4];
static mrt_tcb_t s_churn[MRT_MAX_TASKS];
static mrt_tcb_t s_park[7];
static mrt_mutex_t s_mutex;
static mrt_queue_t s_q;
static uint8_t s_qbuf[4];

static void emit_loop_task(void *arg)
{
    char mark = *(char *)arg;
    for (;;) {
        emit(mark);
        mrt_task_yield();
    }
}

static void emit_once_task(void *arg)
{
    char mark = *(char *)arg;
    emit(mark);
    mrt_task_delay_ticks(100u);
    for (;;) {
        mrt_task_yield();
    }
}

/* Emits, deletes itself, then (unreachably) emits X. */
static void suicide_task(void *arg)
{
    (void)arg;
    emit('d');
    mrt_task_delete(NULL);
    emit('X'); /* must never run */
}

static void park_task(void *arg)
{
    (void)arg;
    mrt_task_delay_ticks(100u);
    for (;;) {
        mrt_task_yield();
    }
}

static void block_recv_task(void *arg)
{
    mrt_queue_handle_t q = (mrt_queue_handle_t)arg;
    char c = '?';
    mrt_queue_receive(q, &c, MRT_WAIT_FOREVER);
    emit('?'); /* must never run: deleted while blocked */
    mrt_task_delay_ticks(100u);
}

static void lock_park_task(void *arg)
{
    mrt_mutex_handle_t m = (mrt_mutex_handle_t)arg;
    mrt_mutex_lock(m, MRT_WAIT_FOREVER);
    mrt_task_delay_ticks(100u);
    for (;;) {
        mrt_task_yield();
    }
}

static mrt_err_t s_recorded;

static void lock_probe_task(void *arg)
{
    mrt_mutex_handle_t m = (mrt_mutex_handle_t)arg;
    mrt_task_delay_ticks(2u);
    s_recorded = mrt_mutex_lock(m, 0u);
    mrt_task_delay_ticks(100u);
}

static void churn_task(void *arg)
{
    (void)arg;
    mrt_task_yield();
    mrt_task_delay_ticks(100u);
}

static char s_mark_a = 'a';
static char s_mark_b = 'b';
static char s_mark_c = 'c';
static char s_mark_e = 'e';
static char s_mark_f = 'f';
static mrt_tcb_t s_rogue;

int main(void)
{
    CHECK(mrt_kernel_init() == MRT_OK);
    mrt_task_handle_t h = NULL;

    /* A. Delete-other is synchronous: B compacts into A's slot and keeps
     * running; C reuses the freed slot. Trace: ab, bb, bc. */
    mrt_test_reset();
    log_reset();
    mrt_task_handle_t hA = NULL;
    mrt_task_handle_t hB = NULL;
    mrt_task_handle_t hC = NULL;
    CHECK(mrt_task_create_static("a", emit_loop_task, &s_mark_a, 1u,
                                 &s_tcb[0], &hA) == MRT_OK);
    CHECK(mrt_task_create_static("b", emit_loop_task, &s_mark_b, 1u,
                                 &s_tcb[1], &hB) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK);
    CHECK(mrt_task_delete(hA) == MRT_OK);
    CHECK(mrt_task_delete(hA) == MRT_ERR_INVAL); /* gone: second delete */
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK);
    CHECK(mrt_task_create_static("c", emit_loop_task, &s_mark_c, 1u,
                                 &s_tcb[2], &hC) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK);
    check_log("abbbbc");

    /* F. Storage reuse: A's old TCB struct backs a new task after B and C
     * are deleted. Appends 'f' to the same log. */
    CHECK(mrt_task_delete(hB) == MRT_OK);
    CHECK(mrt_task_delete(hC) == MRT_OK);
    mrt_task_handle_t hF = NULL;
    CHECK(mrt_task_create_static("f", emit_once_task, &s_mark_f, 1u,
                                 &s_tcb[0], &hF) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK);
    check_log("abbbbcf");

    /* B. Self-delete: D dies on pass 0, parkers delay, idle reaps on pass
     * 8 — E then reuses the slot. No 'X' may ever appear. */
    mrt_test_reset();
    log_reset();
    mrt_task_handle_t hD = NULL;
    CHECK(mrt_task_create_static("d", suicide_task, NULL, 1u,
                                 &s_tcb[0], &hD) == MRT_OK);
    for (uint8_t i = 0u; i < 7u; i++) {
        mrt_task_handle_t hp = NULL;
        CHECK(mrt_task_create_static("p", park_task, NULL, 1u,
                                     &s_park[i], &hp) == MRT_OK);
    }
    CHECK(mrt_kernel_run_ticks(9u) == MRT_OK);
    mrt_task_handle_t hE = NULL;
    CHECK(mrt_task_create_static("e", emit_once_task, &s_mark_e, 1u,
                                 &s_tcb[1], &hE) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(2u) == MRT_OK);
    check_log("de");

    /* G. Zombie guards: suspend is a no-op, resume/priority refuse, and the
     * reaper still collects (slot reusable right after). */
    mrt_test_reset();
    log_reset();
    CHECK(mrt_task_create_static("d", suicide_task, NULL, 1u,
                                 &s_tcb[0], &hD) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK); /* D marks DELETED. */
    CHECK(mrt_task_suspend(hD) == MRT_OK);
    CHECK(mrt_task_resume(hD) == MRT_ERR_STATE);
    CHECK(mrt_task_set_priority(hD, 3u) == MRT_ERR_STATE);
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK); /* idle reaps. */
    CHECK(mrt_task_create_static("e", emit_once_task, &s_mark_e, 1u,
                                 &s_tcb[1], &hE) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK);
    check_log("de");

    /* C. Deleting a blocked task detaches its wait: a later post wakes
     * nobody (woke == false) and simply banks the item. */
    mrt_test_reset();
    mrt_queue_handle_t hq = NULL;
    CHECK(mrt_queue_create_static(1u, 1u, s_qbuf, sizeof(s_qbuf),
                                  &s_q, &hq) == MRT_OK);
    mrt_task_handle_t hR = NULL;
    CHECK(mrt_task_create_static("r", block_recv_task, hq, 1u,
                                 &s_tcb[0], &hR) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK); /* R blocks. */
    CHECK(mrt_task_delete(hR) == MRT_OK);
    {
        char item = 'q';
        bool woke = true;
        CHECK(mrt_queue_send_isr(hq, &item, &woke) == MRT_OK);
        CHECK(woke == false);
        CHECK(mrt_queue_count(hq) == 1u);
    }
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK); /* idle: R is gone. */

    /* D. A mutex owner cannot die: delete refuses, ownership survives (a
     * later try-lock still finds it held). */
    mrt_test_reset();
    s_recorded = MRT_OK;
    mrt_mutex_handle_t hm = NULL;
    CHECK(mrt_mutex_create_static(&s_mutex, &hm) == MRT_OK);
    mrt_task_handle_t hL = NULL;
    mrt_task_handle_t hX = NULL;
    CHECK(mrt_task_create_static("l", lock_park_task, hm, 1u,
                                 &s_tcb[0], &hL) == MRT_OK);
    CHECK(mrt_task_create_static("x", lock_probe_task, hm, 1u,
                                 &s_tcb[1], &hX) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK); /* L locks and parks. */
    CHECK(mrt_task_delete(hL) == MRT_ERR_STATE);
    CHECK(mrt_kernel_run_ticks(3u) == MRT_OK); /* X probes at tick 3. */
    CHECK(s_recorded == MRT_ERR_TIMEOUT);

    /* E. Error paths: rogue handle, and NULL with no current task. */
    CHECK(mrt_task_delete(&s_rogue) == MRT_ERR_INVAL);
    CHECK(mrt_task_delete(NULL) == MRT_ERR_STATE);

    /* H. Churn: three rounds of fill-all/kill-all keep every slot and
     * every pool entry reusable — any leak would NOMEM here. */
    for (int round = 0; round < 3; round++) {
        mrt_test_reset();
        mrt_task_handle_t hh[MRT_MAX_TASKS];
        for (uint8_t i = 0u; i < MRT_MAX_TASKS; i++) {
            hh[i] = NULL;
            CHECK(mrt_task_create_static("c", churn_task, NULL, 1u,
                                         &s_churn[i], &hh[i]) == MRT_OK);
        }
        CHECK(mrt_kernel_run_ticks(MRT_MAX_TASKS) == MRT_OK);
        for (uint8_t i = 0u; i < MRT_MAX_TASKS; i++) {
            CHECK(mrt_task_delete(hh[i]) == MRT_OK);
        }
    }
    mrt_test_reset();
    log_reset();
    CHECK(mrt_task_create_static("p", emit_once_task, &s_mark_e, 1u,
                                 &s_tcb[0], &h) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK);
    check_log("e");

    if (s_failures == 0) {
        printf("PASS test_task_delete\n");
    }
    return s_failures != 0;
}
