/*
 * mini_rtos — kernel/queue.c
 *
 * WHAT:  Bounded FIFO queues (see queue.h for the user-facing contract).
 * WHY THE SEND LOOP: a woken queue waiter owns NOTHING yet — by the time it
 * runs, another task may have taken the freed slot (senders) or the new
 * item (receivers). So send/receive loop: attempt under critical, arm,
 * yield, and on wake look again. The deadline is fixed outside the loop, so
 * re-waiting never extends the total; re-arming puts the task at the FIFO
 * back, which is exactly fair (it lost the race, it rejoins the line).
 * Contrast semaphores/mutexes/events, whose wake-ups hand ownership over
 * directly and need no loop — the difference is a deliberate lesson in
 * "hint" vs "handoff" wakeups, documented at each site.
 */
#include <stdbool.h>
#include <string.h>

#include "mini_rtos/port_api.h"
#include "mini_rtos/queue.h"
#include "mini_rtos/task.h"
#include "mini_rtos/tick.h"
#include "mini_rtos/types.h"

#include "kernel_priv.h"

/* Ring-buffer primitives. Offsets use size_t: even where counts fit u16,
 * pointer arithmetic must span the whole buffer. Callers hold critical. */
static void copy_in(mrt_queue_handle_t q, const void *item)
{
    memcpy(&q->buf[(size_t)q->tail * q->item_size], item, q->item_size);
    q->tail = (uint16_t)((q->tail + 1u) % q->capacity);
    q->count++;
}

static void copy_out(mrt_queue_handle_t q, void *item)
{
    memcpy(item, &q->buf[(size_t)q->head * q->item_size], q->item_size);
    q->head = (uint16_t)((q->head + 1u) % q->capacity);
    q->count--;
}

mrt_err_t mrt_queue_create_static(uint16_t capacity,
                                  uint16_t item_size,
                                  uint8_t *buf,
                                  uint32_t buf_bytes,
                                  mrt_queue_t *storage,
                                  mrt_queue_handle_t *out_handle)
{
    if (capacity == 0u || item_size == 0u ||
        buf == NULL || storage == NULL || out_handle == NULL) {
        return MRT_ERR_INVAL;
    }
    if (!mrt_priv_is_inited()) {
        return MRT_ERR_STATE;
    }
    /* u32 product on purpose: two u16s overflow 16 bits (1000 x 100 needs
     * 100000 bytes) and a wrapped comparison would bless a short buffer. */
    if (buf_bytes < (uint32_t)capacity * (uint32_t)item_size) {
        return MRT_ERR_INVAL;
    }

    memset(storage, 0, sizeof(*storage));
    storage->buf = buf;
    storage->item_size = item_size;
    storage->capacity = capacity;
    *out_handle = storage;
    return MRT_OK;
}

mrt_err_t mrt_queue_send(mrt_queue_handle_t q,
                         const void *item,
                         mrt_tick_t timeout)
{
    if (q == NULL || item == NULL) {
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

    for (;;) {
        mrt_port_enter_critical();
        if (q->count < q->capacity) {
            copy_in(q, item);
            /* A filled slot may satisfy a receiver — wake the best one. */
            mrt_priv_wait_wake_one(q, MRT_WAIT_QUEUE_RECV);
            mrt_port_exit_critical();
            return MRT_OK;
        }
        /* No room. A zero timeout lands here on the first pass (deadline
         * == now is not "before"), so non-blocking needs no special case. */
        if (!forever && !mrt_time_before(mrt_priv_tick_now(), deadline)) {
            mrt_port_exit_critical();
            return MRT_ERR_TIMEOUT;
        }
        mrt_tick_t remaining = forever
            ? MRT_WAIT_FOREVER
            : mrt_time_diff(mrt_priv_tick_now(), deadline);
        mrt_priv_wait_arm(q, MRT_WAIT_QUEUE_SEND, remaining);
        mrt_port_exit_critical();

        mrt_port_yield_to_sched();
        if (self->wait_result == MRT_ERR_STATE) {
            /* Suspended mid-wait: the wait is abandoned, do NOT loop. */
            return MRT_ERR_STATE;
        }
        /* Else: OK (space was freed) or TIMEOUT (deadline passed) — either
         * way look again; the loop's checks tell them apart. */
    }
}

mrt_err_t mrt_queue_receive(mrt_queue_handle_t q,
                            void *item_out,
                            mrt_tick_t timeout)
{
    if (q == NULL || item_out == NULL) {
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

    for (;;) {
        mrt_port_enter_critical();
        if (q->count > 0u) {
            copy_out(q, item_out);
            /* A freed slot may satisfy a sender — wake the best one. */
            mrt_priv_wait_wake_one(q, MRT_WAIT_QUEUE_SEND);
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
        mrt_priv_wait_arm(q, MRT_WAIT_QUEUE_RECV, remaining);
        mrt_port_exit_critical();

        mrt_port_yield_to_sched();
        if (self->wait_result == MRT_ERR_STATE) {
            return MRT_ERR_STATE;
        }
    }
}

mrt_err_t mrt_queue_send_isr(mrt_queue_handle_t q,
                             const void *item,
                             bool *woke)
{
    if (q == NULL || item == NULL) {
        return MRT_ERR_INVAL;
    }
    mrt_port_enter_critical();
    if (q->count >= q->capacity) {
        mrt_port_exit_critical();
        if (woke != NULL) {
            *woke = false;
        }
        return MRT_ERR_TIMEOUT;
    }
    copy_in(q, item);
    bool w = (mrt_priv_wait_wake_one(q, MRT_WAIT_QUEUE_RECV) != NULL);
    mrt_port_exit_critical();
    if (woke != NULL) {
        *woke = w;
    }
    return MRT_OK;
}

mrt_err_t mrt_queue_receive_isr(mrt_queue_handle_t q,
                                void *item_out,
                                bool *woke)
{
    if (q == NULL || item_out == NULL) {
        return MRT_ERR_INVAL;
    }
    mrt_port_enter_critical();
    if (q->count == 0u) {
        mrt_port_exit_critical();
        if (woke != NULL) {
            *woke = false;
        }
        return MRT_ERR_TIMEOUT;
    }
    copy_out(q, item_out);
    bool w = (mrt_priv_wait_wake_one(q, MRT_WAIT_QUEUE_SEND) != NULL);
    mrt_port_exit_critical();
    if (woke != NULL) {
        *woke = w;
    }
    return MRT_OK;
}

uint16_t mrt_queue_count(mrt_queue_handle_t q)
{
    /* NULL reads 0 in all builds (same rule as mrt_pool_alloc): the
     * failure value is testable, so no assert. The read is critical-guarded
     * because a u16 load is not atomic on 8-bit targets. */
    if (q == NULL) {
        return 0u;
    }
    mrt_port_enter_critical();
    uint16_t n = q->count;
    mrt_port_exit_critical();
    return n;
}
