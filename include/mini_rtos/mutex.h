/*
 * mini_rtos — mutex.h
 *
 * WHAT:  Mutual-exclusion locks with ownership and priority inheritance.
 * WHY:   A mutex is a binary semaphore that knows its owner — and that one
 *        fact fixes two real bugs: unlock-by-non-owner is caught (STATE),
 *        and the owner inherits a waiter's priority so a medium task cannot
 *        starve a high one through a low owner (priority inversion).
 *
 * INHERITANCE CONTRACT (single-level, no chains):
 * - Blocking on a held mutex lends the owner your effective priority until
 *   it unlocks; unlock restores the owner's base priority (or hands the
 *   remaining boost to the new owner when waiters remain).
 * - Nested contended mutexes (holding M while blocking on N) are NOT
 *   transitively tracked: only the mutex being unlocked recomputes boost,
 *   so a task holding several contended mutexes may run transiently
 *   under-boosted. Design rule: never hold more than one contended mutex —
 *   order locks globally, hold them briefly, unlock in reverse order.
 *   Full transitive PI is a documented v1 non-goal (see ARCHITECTURE.md).
 *
 * HOW:   Non-recursive: locking twice from the owner returns MRT_ERR_STATE
 *        (a self-deadlock caught at the source, not a hang). Task-only: no
 *        ISR twins exist — an ISR has no owner identity, so lock/unlock
 *        from an ISR is MRT_ERR_STATE/MRT_ERR_ISR respectively... precisely:
 *        lock refuses with MRT_ERR_ISR in an ISR, and unlock fails the
 *        ownership check. Signal ISRs with binary semaphores instead.
 */
#ifndef MINI_RTOS_MUTEX_H
#define MINI_RTOS_MUTEX_H

#include "mini_rtos/task.h"
#include "mini_rtos/types.h"

typedef struct mrt_mutex {
    mrt_task_handle_t owner; /* NULL = free. Written only by mutex.c. */
} mrt_mutex_t;

typedef struct mrt_mutex *mrt_mutex_handle_t;

/* Create a free mutex. NULL storage/out is INVAL; before init is STATE. */
mrt_err_t mrt_mutex_create_static(mrt_mutex_t *storage,
                                  mrt_mutex_handle_t *out_handle);

/*
 * Lock, waiting up to `timeout` ticks. Owners inherit waiter priority
 * while contended (see contract above). Returns OK owning the mutex,
 * TIMEOUT on expiry, STATE when the owner re-locks (self-deadlock),
 * ISR when called from an ISR, STATE with no current task.
 */
mrt_err_t mrt_mutex_lock(mrt_mutex_handle_t m, mrt_tick_t timeout);

/*
 * Unlock. Only the owner may unlock — anyone else (or an unlocked mutex)
 * gets MRT_ERR_STATE. Hands ownership directly to the best waiter when one
 * waits; otherwise the mutex goes free.
 */
mrt_err_t mrt_mutex_unlock(mrt_mutex_handle_t m);

#endif /* MINI_RTOS_MUTEX_H */
