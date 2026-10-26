/*
 * mini_rtos — event.h
 *
 * WHAT:  Event flag groups: 32 sticky bits tasks can wait on by mask.
 * WHY:   Sometimes one signal must wake MANY tasks (shutdown, mode change,
 *        "sensors ready"), or a task must sleep until several things are
 *        ALL true. Queues wake one waiter; events broadcast to every waiter
 *        whose condition holds — the many-to-many complement.
 * HOW:   set() ORs bits and wakes every matching waiter with a snapshot of
 *        the full 32 bits taken at wake time. CLEAR_ON_EXIT waiters clear
 *        only their own mask bits when they resume (in scheduler pick order,
 *        so the highest-priority clearer wins ties deterministically).
 *        Snapshots — not re-reads — are returned, so a waiter always sees
 *        the bits that actually woke it even if a peer cleared first.
 */
#ifndef MINI_RTOS_EVENT_H
#define MINI_RTOS_EVENT_H

#include <stdbool.h>
#include <stdint.h>

#include "mini_rtos/types.h"

/* Wait options (bitmask, uint8_t): exactly one of ANY/ALL is required. */
#define MRT_EVENT_WAIT_ANY      0x01u /* Wake when ANY mask bit is set. */
#define MRT_EVENT_WAIT_ALL      0x02u /* Wake when ALL mask bits are set. */
#define MRT_EVENT_CLEAR_ON_EXIT 0x04u /* Clear our mask bits on wake. */

typedef struct mrt_event {
    uint32_t bits; /* Sticky flags. Written only by event.c. */
} mrt_event_t;

typedef struct mrt_event *mrt_event_handle_t;

/* Create with all bits clear. NULLs are INVAL; before init is STATE. */
mrt_err_t mrt_event_create_static(mrt_event_t *storage,
                                  mrt_event_handle_t *out_handle);

/*
 * Wait until `bits` satisfies the ANY/ALL condition, up to `timeout` ticks.
 * On success *out_bits (NULL-tolerated) receives the wake-time snapshot;
 * on timeout it is left untouched. bits==0 or a bad option mix is INVAL
 * (checked before anything else, so even main() gets INVAL, not STATE).
 * Same ISR/task rules as queues (use set_isr to signal from an ISR).
 */
mrt_err_t mrt_event_wait(mrt_event_handle_t e,
                         uint32_t bits,
                         uint8_t opts,
                         mrt_tick_t timeout,
                         uint32_t *out_bits);

/* OR `bits` in and wake every waiter whose condition now holds. Never
 * blocks — but refuses with MRT_ERR_ISR in an ISR (use set_isr so the
 * caller learns whether a reschedule is due). */
mrt_err_t mrt_event_set(mrt_event_handle_t e, uint32_t bits);

/*
 * ISR twin of set: same broadcast, plus *woke (NULL-tolerated). Safe from
 * tasks too.
 */
mrt_err_t mrt_event_set_isr(mrt_event_handle_t e,
                            uint32_t bits,
                            bool *woke);

/*
 * Clear `bits` (AND with ~bits). Never blocks, never wakes, safe from
 * tasks AND ISRs alike — no _isr twin needed.
 */
mrt_err_t mrt_event_clear(mrt_event_handle_t e, uint32_t bits);

/* Snapshot the current bits into *out. ISR-safe like clear. */
mrt_err_t mrt_event_get(mrt_event_handle_t e, uint32_t *out);

#endif /* MINI_RTOS_EVENT_H */
