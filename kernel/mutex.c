/*
 * mini_rtos — kernel/mutex.c
 *
 * WHAT:  Mutexes with ownership and single-level priority inheritance.
 * WHY INHERITANCE: without it, a low task holding a mutex can be preempted
 * by a medium task while a high task waits — the high task starves for as
 * long as medium work exists (UNBOUNDED inversion). Lending the owner the
 * waiter's priority bounds that wait to one critical section. The boost is
 * stored separately from the base priority (TCB.boost_prio), so unlock and
 * set_priority can always recompute the effective level exactly.
 * HOW:   Lock handoff is exclusive like a semaphore's: unlock() transfers
 * ownership directly to the best waiter before waking it, so a woken locker
 * owns the mutex the moment it runs — no re-check loop (compare queue.c).
 * The nesting caveat (only the unlocked mutex recomputes boost) is the
 * documented v1 boundary — see mutex.h.
 */
#include <stddef.h>

#include "mini_rtos/mutex.h"
#include "mini_rtos/port_api.h"
#include "mini_rtos/task.h"
#include "mini_rtos/tick.h"
#include "mini_rtos/types.h"

#include "kernel_priv.h"

mrt_err_t mrt_mutex_create_static(mrt_mutex_t *storage,
                                  mrt_mutex_handle_t *out_handle)
{
    if (storage == NULL || out_handle == NULL) {
        return MRT_ERR_INVAL;
    }
    if (!mrt_priv_is_inited()) {
        return MRT_ERR_STATE;
    }
    storage->owner = NULL;
    *out_handle = storage;
    return MRT_OK;
}

mrt_err_t mrt_mutex_lock(mrt_mutex_handle_t m, mrt_tick_t timeout)
{
    if (m == NULL) {
        return MRT_ERR_INVAL;
    }
    if (mrt_port_in_isr()) {
        return MRT_ERR_ISR;
    }
    struct mrt_tcb *self = mrt_priv_current();
    if (self == NULL) {
        return MRT_ERR_STATE;
    }

    bool forever = (timeout == MRT_WAIT_FOREVER);
    mrt_tick_t deadline = mrt_priv_tick_now() + timeout;

    mrt_port_enter_critical();
    if (m->owner == NULL) {
        m->owner = self;
        /* The delete guard counts held mutexes; 255 nested holds would mean
         * the stack died long ago, so debug-assert instead of wrapping. */
        MRT_ASSERT(self->owned_mutexes < 255u);
        self->owned_mutexes++;
        mrt_port_exit_critical();
        return MRT_OK;
    }
    if (m->owner == self) {
        /* Non-recursive by design: re-locking would deadlock (nobody can
         * unlock for us while we wait on ourselves), so say so now. */
        mrt_port_exit_critical();
        return MRT_ERR_STATE;
    }
    if (!forever && !mrt_time_before(mrt_priv_tick_now(), deadline)) {
        mrt_port_exit_critical();
        return MRT_ERR_TIMEOUT;
    }
    /* Inheritance: lend the owner our effective priority (max wins — a
     * hotter waiter may already have boosted past us). The boost outlives
     * our own timeout: it persists until unlock recomputes it. That window
     * is bounded by the owner's hold time and far cheaper than untangling
     * exactly whose boost is whose on every timeout. */
    if (self->prio > m->owner->boost_prio) {
        m->owner->boost_prio = self->prio;
        if (self->prio > m->owner->prio) {
            m->owner->prio = self->prio;
        }
    }
    mrt_tick_t remaining = forever
        ? MRT_WAIT_FOREVER
        : mrt_time_diff(mrt_priv_tick_now(), deadline);
    mrt_priv_wait_arm(m, MRT_WAIT_MUTEX, remaining);
    mrt_port_exit_critical();

    mrt_port_yield_to_sched();
    /* Exclusive handoff: OK means unlock() already made us the owner. */
    return self->wait_result;
}

/*
 * Highest effective priority among the tasks still waiting on `m`. Read at
 * unlock to pass the remaining boost to the new owner. Callers hold
 * critical. (Suspended/timed-out waiters detached already, so they cannot
 * inflate the answer — the scan self-heals.)
 */
static mrt_prio_t remaining_waiter_prio(mrt_mutex_handle_t m)
{
    mrt_prio_t top = 0u;
    uint8_t n = mrt_priv_task_count();
    for (uint8_t i = 0u; i < n; i++) {
        struct mrt_tcb *t = mrt_priv_task_at(i);
        if (t->wait_obj == m && t->wait_kind == MRT_WAIT_MUTEX &&
            (t->state == MRT_TASK_DELAYED || t->state == MRT_TASK_BLOCKED) &&
            t->prio > top) {
            top = t->prio;
        }
    }
    return top;
}

mrt_err_t mrt_mutex_unlock(mrt_mutex_handle_t m)
{
    if (m == NULL) {
        return MRT_ERR_INVAL;
    }
    struct mrt_tcb *self = mrt_priv_current();

    mrt_port_enter_critical();
    /* Ownership is identity: only the owner unlocks. The NULL-self check
     * matters — without it, main() "unlocking" a free mutex (NULL == NULL)
     * would wrongly succeed. */
    if (self == NULL || m->owner != self) {
        mrt_port_exit_critical();
        return MRT_ERR_STATE;
    }

    struct mrt_tcb *next = mrt_priv_wait_wake_one(m, MRT_WAIT_MUTEX);
    mrt_prio_t rest = remaining_waiter_prio(m);
    if (next != NULL) {
        /* Direct handoff: the woken task owns the mutex before it runs. It
         * also inherits from whoever still waits on it. */
        m->owner = next;
        MRT_ASSERT(next->owned_mutexes < 255u);
        next->owned_mutexes++;
        if (rest > next->boost_prio) {
            next->boost_prio = rest;
        }
        next->prio = (next->base_prio > next->boost_prio)
            ? next->base_prio
            : next->boost_prio;
    } else {
        m->owner = NULL;
    }
    /* The releaser keeps no boost from this mutex. Nesting caveat: boost
     * owed by OTHER mutexes still held is dropped too — see mutex.h for
     * the design rule that keeps this correct in practice. */
    MRT_ASSERT(self->owned_mutexes > 0u);
    self->owned_mutexes--;
    self->boost_prio = 0u;
    self->prio = self->base_prio;
    mrt_port_exit_critical();
    return MRT_OK;
}
