/*
 * mini_rtos — kernel/sem.c
 *
 * WHAT:  Counting semaphores (see sem.h for the contract).
 * WHY NO LOOP HERE (compare queue.c): a semaphore wake-up is a HANDOFF, not
 * a hint. give() either wakes exactly one waiter (the token is the woken
 * task's the moment it runs — count untouched) or banks the token in the
 * count. Either way there is nothing to re-check after yielding: OK means
 * "token yours", TIMEOUT means "deadline passed". One attempt, at most one
 * wait, then interpret the report.
 */
#include <stdbool.h>
#include <stddef.h>

#include "mini_rtos/port_api.h"
#include "mini_rtos/sem.h"
#include "mini_rtos/task.h"
#include "mini_rtos/tick.h"
#include "mini_rtos/types.h"

#include "kernel_priv.h"

mrt_err_t mrt_sem_create_static(uint16_t max_count,
                                uint16_t initial,
                                mrt_sem_t *storage,
                                mrt_sem_handle_t *out_handle)
{
    if (max_count == 0u || storage == NULL || out_handle == NULL) {
        return MRT_ERR_INVAL;
    }
    if (!mrt_priv_is_inited()) {
        return MRT_ERR_STATE;
    }
    if (initial > max_count) {
        return MRT_ERR_INVAL;
    }
    storage->count = initial;
    storage->max_count = max_count;
    *out_handle = storage;
    return MRT_OK;
}

mrt_err_t mrt_sem_take(mrt_sem_handle_t s, mrt_tick_t timeout)
{
    if (s == NULL) {
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
    if (s->count > 0u) {
        s->count--;
        mrt_port_exit_critical();
        return MRT_OK;
    }
    if (!forever && !mrt_time_before(mrt_priv_tick_now(), deadline)) {
        mrt_port_exit_critical();
        return MRT_ERR_TIMEOUT;
    }
    mrt_tick_t remaining = forever
        ? MRT_WAIT_FOREVER
        : mrt_time_diff(mrt_priv_tick_now(), deadline);
    mrt_priv_wait_arm(s, MRT_WAIT_SEM, remaining);
    mrt_port_exit_critical();

    mrt_port_yield_to_sched();
    /* Handoff semantics: the report IS the answer — no re-check needed. */
    return self->wait_result;
}

mrt_err_t mrt_sem_give(mrt_sem_handle_t s)
{
    if (s == NULL) {
        return MRT_ERR_INVAL;
    }
    if (mrt_port_in_isr()) {
        return MRT_ERR_ISR;
    }
    mrt_port_enter_critical();
    if (mrt_priv_wait_wake_one(s, MRT_WAIT_SEM) == NULL) {
        /* Nobody waiting: bank the token — past max is an error, because a
         * give with no matching take is almost always a logic bug. */
        if (s->count >= s->max_count) {
            mrt_port_exit_critical();
            return MRT_ERR_STATE;
        }
        s->count++;
    }
    /* else: the token went straight to the woken task; count untouched. */
    mrt_port_exit_critical();
    return MRT_OK;
}

mrt_err_t mrt_sem_take_isr(mrt_sem_handle_t s)
{
    if (s == NULL) {
        return MRT_ERR_INVAL;
    }
    mrt_port_enter_critical();
    if (s->count == 0u) {
        mrt_port_exit_critical();
        return MRT_ERR_TIMEOUT;
    }
    s->count--;
    mrt_port_exit_critical();
    return MRT_OK;
}

mrt_err_t mrt_sem_give_isr(mrt_sem_handle_t s, bool *woke)
{
    if (s == NULL) {
        return MRT_ERR_INVAL;
    }
    mrt_port_enter_critical();
    bool w = (mrt_priv_wait_wake_one(s, MRT_WAIT_SEM) != NULL);
    if (!w) {
        if (s->count >= s->max_count) {
            mrt_port_exit_critical();
            if (woke != NULL) {
                *woke = false;
            }
            return MRT_ERR_STATE;
        }
        s->count++;
    }
    mrt_port_exit_critical();
    if (woke != NULL) {
        *woke = w;
    }
    return MRT_OK;
}

uint16_t mrt_sem_count(mrt_sem_handle_t s)
{
    /* NULL reads 0 in all builds (same rule as mrt_pool_alloc). */
    if (s == NULL) {
        return 0u;
    }
    mrt_port_enter_critical();
    uint16_t n = s->count;
    mrt_port_exit_critical();
    return n;
}
