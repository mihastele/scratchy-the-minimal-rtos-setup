/*
 * mini_rtos — tests/test_queue.c
 *
 * WHAT:  Queue behavior: FIFO order, count, blocking send/receive wakes,
 *        timeouts, suspend-abandonment, and the ISR twins.
 * WHY:   The queue is the template for all blocking IPC — the send loop's
 *        re-check discipline ("a wake is a hint, not a handoff") is what the
 *        exact-sequence tests below exist to prove. If these traces hold,
 *        the shared waiter protocol holds.
 * HOW:   Direct _isr calls from main for the mechanical paths (no task
 *        context needed), scripted task pairs with exact-log assertions for
 *        every blocking direction.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mini_rtos/kernel.h"
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

static mrt_tcb_t s_tcb_a, s_tcb_b;
static mrt_queue_t s_q;
static uint8_t s_qbuf[8];

/* Receives forever, emitting each item. */
static void recv_task(void *arg)
{
    mrt_queue_handle_t q = (mrt_queue_handle_t)arg;
    for (;;) {
        char c = '?';
        if (mrt_queue_receive(q, &c, MRT_WAIT_FOREVER) == MRT_OK) {
            emit(c);
        }
    }
}

/* After a delay, sends A then B (cap 2: both fit) and parks. */
static void send_ab_task(void *arg)
{
    mrt_queue_handle_t q = (mrt_queue_handle_t)arg;
    char a = 'A';
    char b = 'B';
    mrt_task_delay_ticks(2u);
    mrt_queue_send(q, &a, 0u);
    mrt_queue_send(q, &b, 0u);
    mrt_task_delay_ticks(100u);
}

/* Fills a cap-1 queue, blocks on the second send, then marks and parks. */
static void blocking_sender(void *arg)
{
    mrt_queue_handle_t q = (mrt_queue_handle_t)arg;
    char x = 'X';
    char y = 'Y';
    mrt_queue_send(q, &x, 0u);
    mrt_queue_send(q, &y, MRT_WAIT_FOREVER);
    emit('s');
    mrt_task_delay_ticks(100u);
}

/* Receives two items with marks, then parks. */
static void recv_two_task(void *arg)
{
    mrt_queue_handle_t q = (mrt_queue_handle_t)arg;
    char c = '?';
    mrt_task_delay_ticks(1u);
    mrt_queue_receive(q, &c, MRT_WAIT_FOREVER);
    emit(c);
    mrt_queue_receive(q, &c, MRT_WAIT_FOREVER);
    emit(c);
    mrt_task_delay_ticks(100u);
}

static mrt_err_t s_recorded;

/* One timed receive on an empty queue, recording the verdict. */
static void timeout_recver(void *arg)
{
    mrt_queue_handle_t q = (mrt_queue_handle_t)arg;
    char c = '?';
    s_recorded = mrt_queue_receive(q, &c, 2u);
    emit(s_recorded == MRT_ERR_TIMEOUT ? 'T' : '?');
    mrt_task_delay_ticks(100u);
}

/* One FOREVER receive, recording whatever ends it (abandon test). */
static void abandon_recver(void *arg)
{
    mrt_queue_handle_t q = (mrt_queue_handle_t)arg;
    char c = '?';
    s_recorded = mrt_queue_receive(q, &c, MRT_WAIT_FOREVER);
    emit(s_recorded == MRT_ERR_STATE ? 'S' : '?');
    mrt_task_delay_ticks(100u);
}

int main(void)
{
    mrt_queue_handle_t h = NULL;

    /* Creating before init fails, like every other create. */
    CHECK(mrt_queue_create_static(2u, 1u, s_qbuf, sizeof(s_qbuf),
                                  &s_q, &h) == MRT_ERR_STATE);
    CHECK(mrt_kernel_init() == MRT_OK);

    /* Validation. */
    CHECK(mrt_queue_create_static(0u, 1u, s_qbuf, sizeof(s_qbuf),
                                  &s_q, &h) == MRT_ERR_INVAL);
    CHECK(mrt_queue_create_static(2u, 0u, s_qbuf, sizeof(s_qbuf),
                                  &s_q, &h) == MRT_ERR_INVAL);
    CHECK(mrt_queue_create_static(2u, 1u, NULL, sizeof(s_qbuf),
                                  &s_q, &h) == MRT_ERR_INVAL);
    CHECK(mrt_queue_create_static(8u, 1u, s_qbuf, 4u,
                                  &s_q, &h) == MRT_ERR_INVAL);
    CHECK(mrt_queue_create_static(2u, 1u, s_qbuf, sizeof(s_qbuf),
                                  NULL, &h) == MRT_ERR_INVAL);

    /* Mechanical FIFO via the _isr twins (no task context needed): a, b, c
     * in; d refused; a, b, c out in order; empty refused. */
    CHECK(mrt_queue_create_static(3u, 1u, s_qbuf, sizeof(s_qbuf),
                                  &s_q, &h) == MRT_OK);
    {
        char a = 'a', b = 'b', c = 'c', d = 'd', out = '?';
        bool woke = true;
        CHECK(mrt_queue_send_isr(h, &a, &woke) == MRT_OK && woke == false);
        CHECK(mrt_queue_send_isr(h, &b, NULL) == MRT_OK);
        CHECK(mrt_queue_send_isr(h, &c, NULL) == MRT_OK);
        CHECK(mrt_queue_count(h) == 3u);
        CHECK(mrt_queue_send_isr(h, &d, &woke) == MRT_ERR_TIMEOUT);
        CHECK(mrt_queue_receive_isr(h, &out, NULL) == MRT_OK && out == 'a');
        CHECK(mrt_queue_receive_isr(h, &out, NULL) == MRT_OK && out == 'b');
        CHECK(mrt_queue_receive_isr(h, &out, NULL) == MRT_OK && out == 'c');
        CHECK(mrt_queue_receive_isr(h, &out, NULL) == MRT_ERR_TIMEOUT);
        CHECK(mrt_queue_count(h) == 0u);
    }

    /* Blocking receiver woken: R blocks, S delivers A,B, R drains both. */
    mrt_test_reset();
    log_reset();
    CHECK(mrt_queue_create_static(2u, 1u, s_qbuf, sizeof(s_qbuf),
                                  &s_q, &h) == MRT_OK);
    mrt_task_handle_t hr = NULL;
    mrt_task_handle_t hs = NULL;
    CHECK(mrt_task_create_static("r", recv_task, h, 1u,
                                 &s_tcb_a, &hr) == MRT_OK);
    CHECK(mrt_task_create_static("s", send_ab_task, h, 1u,
                                 &s_tcb_b, &hs) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(6u) == MRT_OK);
    check_log("AB");
    CHECK(mrt_queue_count(h) == 0u);

    /* Blocking sender woken: S fills cap-1 and blocks on Y; R takes X
     * (freeing S to complete Y), then takes Y. Trace: X s Y. */
    mrt_test_reset();
    log_reset();
    CHECK(mrt_queue_create_static(1u, 1u, s_qbuf, sizeof(s_qbuf),
                                  &s_q, &h) == MRT_OK);
    CHECK(mrt_task_create_static("s", blocking_sender, h, 1u,
                                 &s_tcb_a, &hs) == MRT_OK);
    CHECK(mrt_task_create_static("r", recv_two_task, h, 1u,
                                 &s_tcb_b, &hr) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(6u) == MRT_OK);
    check_log("XsY");
    CHECK(mrt_queue_count(h) == 0u);

    /* Timeout: empty queue, 2-tick wait, exact verdict recorded. */
    mrt_test_reset();
    log_reset();
    s_recorded = MRT_OK;
    CHECK(mrt_queue_create_static(1u, 1u, s_qbuf, sizeof(s_qbuf),
                                  &s_q, &h) == MRT_OK);
    CHECK(mrt_task_create_static("t", timeout_recver, h, 1u,
                                 &s_tcb_a, &hr) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(4u) == MRT_OK);
    check_log("T");
    CHECK(s_recorded == MRT_ERR_TIMEOUT);

    /* Suspend-while-blocked: the abandoned receive returns STATE, and the
     * resumed task does NOT loop back into waiting. */
    mrt_test_reset();
    log_reset();
    s_recorded = MRT_OK;
    CHECK(mrt_queue_create_static(1u, 1u, s_qbuf, sizeof(s_qbuf),
                                  &s_q, &h) == MRT_OK);
    CHECK(mrt_task_create_static("r", abandon_recver, h, 1u,
                                 &s_tcb_a, &hr) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK); /* R blocks. */
    CHECK(mrt_task_suspend(hr) == MRT_OK);
    CHECK(mrt_task_resume(hr) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK); /* R reports STATE. */
    check_log("S");
    CHECK(s_recorded == MRT_ERR_STATE);

    if (s_failures == 0) {
        printf("PASS test_queue\n");
    }
    return s_failures != 0;
}
