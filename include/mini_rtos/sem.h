/*
 * mini_rtos — sem.h
 *
 * WHAT:  Counting semaphores: tokens for resource pools and signaling.
 * WHY:   A queue of zero-size items would do, but tokens deserve a lighter,
 *        clearer object: take() consumes one, give() produces one, and
 *        blocking works exactly like everywhere else. Binary semaphores
 *        (max_count 1) are the standard ISR-to-task signal — the ISR gives,
 *        the task takes with a timeout.
 * HOW:   RULE OF THUMB — signaling/event? semaphore. Mutual exclusion?
 *        mutex (it has ownership + priority inheritance; this has neither).
 *        A semaphore given from an ISR wakes the best waiter with the token
 *        handed over directly (no count detour), so the woken task owns it
 *        the moment it runs.
 */
#ifndef MINI_RTOS_SEM_H
#define MINI_RTOS_SEM_H

#include <stdbool.h>
#include <stdint.h>

#include "mini_rtos/types.h"

typedef struct mrt_sem {
    uint16_t count;    /* Tokens available. */
    uint16_t max_count;/* Ceiling: give() past this is an error, not silent. */
} mrt_sem_t;

typedef struct mrt_sem *mrt_sem_handle_t;

/*
 * Create with `initial` tokens (0..max_count). max_count 0 is INVAL (a
 * semaphore that can never hold anything is a bug, not an object).
 */
mrt_err_t mrt_sem_create_static(uint16_t max_count,
                                uint16_t initial,
                                mrt_sem_t *storage,
                                mrt_sem_handle_t *out_handle);

/*
 * Take one token, waiting up to `timeout` ticks. Same ISR/task rules as
 * queues (MRT_ERR_ISR in ISR — use take_isr; MRT_ERR_STATE with no task).
 */
mrt_err_t mrt_sem_take(mrt_sem_handle_t s, mrt_tick_t timeout);

/*
 * Give one token. If a task waits, the token passes to the best waiter
 * directly (count untouched); otherwise count++ unless already at max —
 * giving past max returns MRT_ERR_STATE (unbalanced give: almost always a
 * logic bug, so it fails loudly instead of saturating quietly).
 */
mrt_err_t mrt_sem_give(mrt_sem_handle_t s);

/*
 * Take without ever blocking: OK with a token consumed, or TIMEOUT when
 * empty. Usable from tasks and ISRs alike.
 */
mrt_err_t mrt_sem_take_isr(mrt_sem_handle_t s);

/*
 * ISR twin of give: same handoff, plus *woke (NULL-tolerated) reporting
 * whether a task was unblocked. Safe from tasks too.
 */
mrt_err_t mrt_sem_give_isr(mrt_sem_handle_t s, bool *woke);

/* Tokens currently available (NULL handle reads 0). ISR-safe. */
uint16_t mrt_sem_count(mrt_sem_handle_t s);

#endif /* MINI_RTOS_SEM_H */
