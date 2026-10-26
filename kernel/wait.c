/*
 * mini_rtos — kernel/wait.c
 *
 * WHAT:  The waiter registry: arming tasks as IPC waiters and waking the
 *        best one. The two primitives every blocking object builds on.
 * WHY:   Without a shared registry, each IPC object would reimplement
 *        "find the best waiter" — five copies of priority+tiebreak logic to
 *        keep identical. Here it exists once, with one ordering rule: the
 *        highest effective priority wins, ties broken by longest waiting
 *        (FIFO via a sequence stamp). Queues, semaphores, and mutexes wake
 *        ONE waiter per post (a token/item has exactly one taker); events
 *        scan for themselves because they broadcast (see event.c).
 * HOW:   Callers hold the port critical section across check-arm and
 *        scan-wake pairs (see kernel_priv.h for why). wait_seq wraps safely:
 *        only live waiters are ever compared, and the comparison is modular.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini_rtos/port_api.h"
#include "mini_rtos/task.h"
#include "mini_rtos/types.h"

#include "kernel_priv.h"

/* FIFO stamp source. Wraps modulo 2^32 — compared modularly, so the wrap
 * is harmless (same trick as tick time, same reasoning). */
static uint32_t s_wait_seq;

/* True when waiter stamped `a` has waited longer than `b`. */
static bool seq_earlier(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

void mrt_priv_wait_arm(void *obj, uint8_t kind, mrt_tick_t timeout)
{
    struct mrt_tcb *self = mrt_priv_current();
    MRT_ASSERT(self != NULL && obj != NULL &&
               kind != MRT_WAIT_NONE && timeout != 0u);

    self->wait_obj = obj;
    self->wait_kind = kind;
    self->wait_seq = s_wait_seq++;
    /* Dirtied on purpose: every wake path (object, timeout, suspend) sets
     * the real report, so whatever the task reads after yielding is fresh —
     * a stale OK from a previous wait can never leak into this one. */
    self->wait_result = MRT_ERR_STATE;
    if (timeout == MRT_WAIT_FOREVER) {
        self->state = MRT_TASK_BLOCKED;
    } else {
        self->wake_tick = mrt_priv_tick_now() + timeout;
        self->state = MRT_TASK_DELAYED;
    }
}

struct mrt_tcb *mrt_priv_wait_wake_one(void *obj, uint8_t kind)
{
    struct mrt_tcb *best = NULL;
    uint8_t n = mrt_priv_task_count();
    for (uint8_t i = 0u; i < n; i++) {
        struct mrt_tcb *t = mrt_priv_task_at(i);
        /* wait_obj is the live-wait marker: suspend and the timeout ISR
         * both detach it, so matching it cannot resurrect a dead wait.
         * The state check is belt-and-braces for the same reason. */
        if (t->wait_obj != obj || t->wait_kind != kind) {
            continue;
        }
        if (t->state != MRT_TASK_DELAYED && t->state != MRT_TASK_BLOCKED) {
            continue;
        }
        if (best == NULL || t->prio > best->prio ||
            (t->prio == best->prio &&
             seq_earlier(t->wait_seq, best->wait_seq))) {
            best = t;
        }
    }

    if (best != NULL) {
        best->wait_obj = NULL;
        best->wait_kind = MRT_WAIT_NONE;
        best->wait_result = MRT_OK;
        best->state = MRT_TASK_READY;
    }
    return best;
}
