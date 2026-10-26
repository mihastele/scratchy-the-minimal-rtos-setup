/*
 * mini_rtos — tests/test_isr_discipline.c
 *
 * WHAT:  The ISR contract: blocking calls refuse with MRT_ERR_ISR inside
 *        interrupt context, _isr twins work there, and non-waking queries
 *        stay legal everywhere.
 * WHY:   "Don't sleep in an ISR" is the most violated embedded rule because
 *        most kernels only document it. mini_rtos enforces it: the port
 *        reports ISR context and every blocking call checks. This test
 *        drives the fake-ISR hooks to prove the enforcement — including
 *        nesting, which is where naive flag implementations break.
 * HOW:   Main-line calls only (no tasks needed): flip into fake-ISR context,
 *        assert every refusal and every twin, flip out, prove normality.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "mini_rtos/event.h"
#include "mini_rtos/kernel.h"
#include "mini_rtos/mutex.h"
#include "mini_rtos/queue.h"
#include "mini_rtos/sem.h"
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

static mrt_queue_t s_q;
static uint8_t s_qbuf[4];
static mrt_sem_t s_sem;
static mrt_mutex_t s_mutex;
static mrt_event_t s_event;

int main(void)
{
    CHECK(mrt_kernel_init() == MRT_OK);

    mrt_queue_handle_t hq = NULL;
    mrt_sem_handle_t hs = NULL;
    mrt_mutex_handle_t hm = NULL;
    mrt_event_handle_t he = NULL;
    CHECK(mrt_queue_create_static(2u, 1u, s_qbuf, sizeof(s_qbuf),
                                  &s_q, &hq) == MRT_OK);
    CHECK(mrt_sem_create_static(2u, 1u, &s_sem, &hs) == MRT_OK);
    CHECK(mrt_mutex_create_static(&s_mutex, &hm) == MRT_OK);
    CHECK(mrt_event_create_static(&s_event, &he) == MRT_OK);

    /* Inside ISR context, every blocking-or-waking call refuses. */
    mrt_test_isr_enter();
    {
        char item = 'x';
        char out = '?';
        CHECK(mrt_queue_send(hq, &item, 0u) == MRT_ERR_ISR);
        CHECK(mrt_queue_receive(hq, &out, 0u) == MRT_ERR_ISR);
        CHECK(mrt_sem_take(hs, 0u) == MRT_ERR_ISR);
        CHECK(mrt_sem_give(hs) == MRT_ERR_ISR);
        CHECK(mrt_mutex_lock(hm, 0u) == MRT_ERR_ISR);
        CHECK(mrt_event_wait(he, 0x01u, MRT_EVENT_WAIT_ANY, 0u, NULL)
              == MRT_ERR_ISR);
        CHECK(mrt_event_set(he, 0x01u) == MRT_ERR_ISR);
    }

    /* ...while the _isr twins do real work there. */
    {
        char item = 'i';
        char out = '?';
        bool woke = true;
        CHECK(mrt_queue_send_isr(hq, &item, &woke) == MRT_OK);
        CHECK(woke == false); /* no waiters */
        CHECK(mrt_queue_receive_isr(hq, &out, NULL) == MRT_OK);
        CHECK(out == 'i');
        CHECK(mrt_sem_give_isr(hs, &woke) == MRT_OK && woke == false);
        CHECK(mrt_sem_take_isr(hs) == MRT_OK);
        CHECK(mrt_sem_take_isr(hs) == MRT_OK);
        CHECK(mrt_sem_take_isr(hs) == MRT_ERR_TIMEOUT); /* drained */
        CHECK(mrt_event_set_isr(he, 0x05u, &woke) == MRT_OK);
        CHECK(woke == false);
    }

    /* Non-waking queries never refuse, even in an ISR. */
    {
        uint32_t bits = 0u;
        CHECK(mrt_event_clear(he, 0x01u) == MRT_OK);
        CHECK(mrt_event_get(he, &bits) == MRT_OK && bits == 0x04u);
        CHECK(mrt_queue_count(hq) == 0u);
        CHECK(mrt_sem_count(hs) == 0u);
    }

    /* Nesting: one exit of two still counts as in-ISR. */
    mrt_test_isr_enter();
    mrt_test_isr_exit();
    {
        char item = 'x';
        CHECK(mrt_queue_send(hq, &item, 0u) == MRT_ERR_ISR);
    }
    mrt_test_isr_exit();

    /* Back in task context the same call gets past the ISR gate: from main
     * (no current task) it now fails as STATE — the changed verdict proves
     * the context flag really cleared. */
    {
        char item = 'x';
        CHECK(mrt_queue_send(hq, &item, 0u) == MRT_ERR_STATE);
    }

    /* Ownership is orthogonal to context: main owns nothing, so unlocking
     * a free mutex is STATE, not a context complaint. */
    CHECK(mrt_mutex_unlock(hm) == MRT_ERR_STATE);

    if (s_failures == 0) {
        printf("PASS test_isr_discipline\n");
    }
    return s_failures != 0;
}
