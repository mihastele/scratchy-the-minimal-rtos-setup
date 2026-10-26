/*
 * mini_rtos — kernel/event.c
 *
 * WHAT:  Event flag groups (see event.h for the contract).
 * WHY BROADCAST: a queue post has exactly one taker, but an event is a
 * FACT ("shutdown requested") — every task waiting on it must observe it.
 * So set() scans for itself instead of wake_one(): each matching waiter
 * gets a snapshot of the full 32 bits taken at wake time, and CLEAR_ON_EXIT
 * is applied later, in each waiter's own return path, in scheduler pick
 * order. A snapshot — never a re-read — is what makes "woke + peer cleared
 * first" still report the bits that genuinely caused the wake.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mini_rtos/event.h"
#include "mini_rtos/port_api.h"
#include "mini_rtos/task.h"
#include "mini_rtos/tick.h"
#include "mini_rtos/types.h"

#include "kernel_priv.h"

/* Exactly one of ANY/ALL, and no unknown bits. Strict on purpose: option
 * garbage is a caller bug, and INVAL at the call site beats a mystery wait. */
static bool opts_valid(uint8_t opts)
{
    if ((opts & (uint8_t)~(MRT_EVENT_WAIT_ANY | MRT_EVENT_WAIT_ALL |
                           MRT_EVENT_CLEAR_ON_EXIT)) != 0u) {
        return false;
    }
    uint8_t mode = opts & (MRT_EVENT_WAIT_ANY | MRT_EVENT_WAIT_ALL);
    return mode == MRT_EVENT_WAIT_ANY || mode == MRT_EVENT_WAIT_ALL;
}

static bool cond_met(uint32_t mask, uint8_t opts, uint32_t bits)
{
    if ((opts & MRT_EVENT_WAIT_ANY) != 0u) {
        return (bits & mask) != 0u;
    }
    return (bits & mask) == mask; /* ALL (opts pre-validated). */
}

mrt_err_t mrt_event_create_static(mrt_event_t *storage,
                                  mrt_event_handle_t *out_handle)
{
    if (storage == NULL || out_handle == NULL) {
        return MRT_ERR_INVAL;
    }
    if (!mrt_priv_is_inited()) {
        return MRT_ERR_STATE;
    }
    storage->bits = 0u;
    *out_handle = storage;
    return MRT_OK;
}

mrt_err_t mrt_event_wait(mrt_event_handle_t e,
                         uint32_t bits,
                         uint8_t opts,
                         mrt_tick_t timeout,
                         uint32_t *out_bits)
{
    /* Pure validation first: bad arguments report INVAL from any context
     * (even main()), never a context error. */
    if (e == NULL || bits == 0u || !opts_valid(opts)) {
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
    if (cond_met(bits, opts, e->bits)) {
        uint32_t snap = e->bits;
        if ((opts & MRT_EVENT_CLEAR_ON_EXIT) != 0u) {
            e->bits &= ~bits;
        }
        mrt_port_exit_critical();
        if (out_bits != NULL) {
            *out_bits = snap;
        }
        return MRT_OK;
    }
    if (!forever && !mrt_time_before(mrt_priv_tick_now(), deadline)) {
        mrt_port_exit_critical();
        return MRT_ERR_TIMEOUT;
    }
    /* Stash the request where the set-side scan can test it, then arm. All
     * under critical: a set landing between would otherwise miss us. */
    self->wait_bits = bits;
    self->wait_opts = opts;
    mrt_tick_t remaining = forever
        ? MRT_WAIT_FOREVER
        : mrt_time_diff(mrt_priv_tick_now(), deadline);
    mrt_priv_wait_arm(e, MRT_WAIT_EVENT, remaining);
    mrt_port_exit_critical();

    mrt_port_yield_to_sched();
    if (self->wait_result != MRT_OK) {
        /* TIMEOUT (out_bits untouched by contract) or STATE (abandoned). */
        return self->wait_result;
    }
    /* Woken with a snapshot: apply OUR clear now. Multiple clearers run in
     * pick order — deterministic, highest priority first. */
    mrt_port_enter_critical();
    uint32_t snap = self->wait_aux;
    if ((opts & MRT_EVENT_CLEAR_ON_EXIT) != 0u) {
        e->bits &= ~bits;
    }
    mrt_port_exit_critical();
    if (out_bits != NULL) {
        *out_bits = snap;
    }
    return MRT_OK;
}

/* Broadcast core shared by set/set_isr. Callers hold critical. */
static void event_set_locked(mrt_event_handle_t e, uint32_t bits, bool *woke)
{
    e->bits |= bits;
    bool any = false;
    uint8_t n = mrt_priv_task_count();
    for (uint8_t i = 0u; i < n; i++) {
        struct mrt_tcb *t = mrt_priv_task_at(i);
        if (t->wait_obj != e || t->wait_kind != MRT_WAIT_EVENT) {
            continue;
        }
        if (t->state != MRT_TASK_DELAYED && t->state != MRT_TASK_BLOCKED) {
            continue;
        }
        if (!cond_met(t->wait_bits, t->wait_opts, e->bits)) {
            continue;
        }
        /* Snapshot the CURRENT bits — before any clearer runs — detach,
         * and report OK. Every matching waiter wakes: broadcast. */
        t->wait_aux = e->bits;
        t->wait_obj = NULL;
        t->wait_kind = MRT_WAIT_NONE;
        t->wait_result = MRT_OK;
        t->state = MRT_TASK_READY;
        any = true;
    }
    if (woke != NULL) {
        *woke = any;
    }
}

mrt_err_t mrt_event_set(mrt_event_handle_t e, uint32_t bits)
{
    if (e == NULL) {
        return MRT_ERR_INVAL;
    }
    if (mrt_port_in_isr()) {
        return MRT_ERR_ISR;
    }
    mrt_port_enter_critical();
    event_set_locked(e, bits, NULL);
    mrt_port_exit_critical();
    return MRT_OK;
}

mrt_err_t mrt_event_set_isr(mrt_event_handle_t e,
                            uint32_t bits,
                            bool *woke)
{
    if (e == NULL) {
        return MRT_ERR_INVAL;
    }
    mrt_port_enter_critical();
    event_set_locked(e, bits, woke);
    mrt_port_exit_critical();
    return MRT_OK;
}

mrt_err_t mrt_event_clear(mrt_event_handle_t e, uint32_t bits)
{
    /* No in-ISR refusal: clearing never blocks and never wakes, so it is
     * safe from any context — no _isr twin needed. */
    if (e == NULL) {
        return MRT_ERR_INVAL;
    }
    mrt_port_enter_critical();
    e->bits &= ~bits;
    mrt_port_exit_critical();
    return MRT_OK;
}

mrt_err_t mrt_event_get(mrt_event_handle_t e, uint32_t *out)
{
    if (e == NULL || out == NULL) {
        return MRT_ERR_INVAL;
    }
    mrt_port_enter_critical();
    *out = e->bits;
    mrt_port_exit_critical();
    return MRT_OK;
}
