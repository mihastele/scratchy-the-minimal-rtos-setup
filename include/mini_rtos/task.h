/*
 * mini_rtos — task.h
 *
 * WHAT:  The task API: creating tasks, yielding, delaying, reading time.
 * WHY:   Tasks are the unit of concurrency — each is an independent thread
 *        of execution with its own stack. Everything else in an RTOS (IPC,
 *        timeouts, drivers) exists to let tasks cooperate safely.
 * HOW:   The application owns a `mrt_tcb_t` per task (usually `static`), the
 *        kernel fills it in, and the port layer attaches machine state.
 *        Phase 0 offers static creation only — dynamic creation would need
 *        a heap the kernel refuses to own (see ARCHITECTURE.md).
 */
#ifndef MINI_RTOS_TASK_H
#define MINI_RTOS_TASK_H

#include <stdint.h>

#include "mini_rtos/config.h"
#include "mini_rtos/types.h"

/* A task body: runs on its own stack, usually loops forever. `arg` is the
 * pointer given at creation. Returning from this function suspends the task
 * (it has finished); see the scheduler notes in sched.c. */
typedef void (*mrt_task_func_t)(void *arg);

/* Lifecycle states. A task is always in exactly one of these. */
typedef enum mrt_task_state {
    MRT_TASK_READY,    /* May run; the scheduler picks among READY tasks. */
    MRT_TASK_DELAYED,  /* Sleeping until wake_tick (plain delay or IPC wait
                           with a timeout — see wait_obj below). */
    MRT_TASK_BLOCKED,  /* Waiting on an IPC object with MRT_WAIT_FOREVER:
                           only the object (or a suspend) can wake it. */
    MRT_TASK_SUSPENDED, /* Parked: finished, or explicitly suspended. */
    MRT_TASK_DELETED   /* Dead but not yet reaped (self-delete only): never
                          picked, freed by the idle task. No API acts on it. */
} mrt_task_state_t;

/*
 * Task Control Block. Deliberately transparent (not opaque) so learners can
 * inspect every field in a debugger; production RTOSes often hide this.
 *
 * OWNERSHIP: the application allocates the struct, the kernel fills the
 * scheduling fields, and the port layer owns `port_state`. Nobody else
 * writes these fields directly — always go through the API so invariants
 * (e.g. "a DELAYED task always has a valid wake_tick") hold.
 */
typedef struct mrt_tcb {
    char name[MRT_MAX_TASK_NAME_LEN]; /* Human label, NUL-terminated. */
    mrt_task_func_t func;             /* The task body. */
    void *arg;                        /* Passed to func. */
    /* Effective priority: max(base_prio, boost_prio). The scheduler reads
     * only this field, so inheritance works without touching the picker. */
    mrt_prio_t prio;
    mrt_prio_t base_prio;            /* Owned priority (create/set_priority). */
    mrt_prio_t boost_prio;            /* Inheritance boost, 0 when none. */
    uint8_t owned_mutexes;            /* Mutexes held (delete guard). */
    volatile mrt_task_state_t state;  /* volatile: ISRs change it on MCU. */
    mrt_tick_t wake_tick;             /* Valid only while DELAYED. */
    void *port_state;                 /* Opaque machine state, port-owned. */
    uint8_t slot;                     /* Index in the kernel task table. */
    /* --- IPC wait fields (see kernel/wait.c for the protocol) --- */
    void *wait_obj;                   /* Object blocked on, NULL if none. */
    uint8_t wait_kind;                /* MRT_WAIT_* flavor (kernel_priv.h). */
    mrt_err_t wait_result;            /* Wake report: OK, TIMEOUT, or STATE
                                         (suspended mid-wait: wait dead). */
    uint32_t wait_seq;                /* FIFO order among equal-prio waiters. */
    uint32_t wait_bits;               /* Event wait mask (MRT_WAIT_EVENT). */
    uint32_t wait_aux;                /* Event observed-bits snapshot. */
    uint8_t wait_opts;                /* Event options (MRT_EVENT_*). */
} mrt_tcb_t;

/* Opaque handle: a pointer in disguise, so the type can evolve. */
typedef struct mrt_tcb *mrt_task_handle_t;

/*
 * Create a task using caller-owned storage.
 *
 * - name: copied, truncated to MRT_MAX_TASK_NAME_LEN-1 chars + NUL.
 * - prio: 0..MRT_MAX_PRIORITY, higher number = more important.
 * - tcb_storage: application-owned `mrt_tcb_t` (must outlive the task;
 *   typically `static`). The kernel zeroes and fills it.
 * - out_handle: receives the handle on success.
 *
 * Returns MRT_OK, MRT_ERR_INVAL (NULL func/storage/handle, prio too high),
 * MRT_ERR_NOMEM (table full — raise MRT_MAX_TASKS), or MRT_ERR_STATE
 * (kernel not inited — call mrt_kernel_init() first).
 */
mrt_err_t mrt_task_create_static(const char *name,
                                 mrt_task_func_t func,
                                 void *arg,
                                 mrt_prio_t prio,
                                 mrt_tcb_t *tcb_storage,
                                 mrt_task_handle_t *out_handle);

/*
 * Give up the CPU voluntarily. The caller stays READY, so a higher-priority
 * READY task runs next; among equal priority, the next task in round-robin
 * order runs. Yields are the polite way to share one priority level.
 */
void mrt_task_yield(void);

/*
 * Sleep for `ticks` ticks, then become READY again. The delay is measured
 * from the moment of the call. `mrt_task_delay_ticks(0)` is defined as a
 * yield (same convention as "sleep 0" yielding in many systems) — pass 0
 * when you want to be explicit that you expect rescheduling.
 */
void mrt_task_delay_ticks(mrt_tick_t ticks);

/* Current tick count. Monotonic modulo 2^32; compare with tick.h helpers. */
mrt_tick_t mrt_task_get_tick(void);

/*
 * Suspend a task: it stops being scheduled until mrt_task_resume().
 * Pass NULL to suspend the calling task itself (valid only inside a task).
 *
 * Suspending abandons any in-progress delay or IPC wait — a DELAYED or
 * BLOCKED task becomes SUSPENDED with its wake tick / wait forgotten. When
 * it is resumed, the abandoned blocking call returns MRT_ERR_STATE instead
 * of silently re-blocking. Suspending an already-SUSPENDED task is a
 * harmless no-op. Suspending a mutex owner does NOT release the mutex (the
 * owner still owns it when resumed) — coordinate before parking owners.
 *
 * Returns MRT_OK, MRT_ERR_INVAL (rogue handle — not a created task), or
 * MRT_ERR_STATE (NULL with no current task).
 */
mrt_err_t mrt_task_suspend(mrt_task_handle_t task);

/*
 * Make a SUSPENDED task READY again. The handle must be explicit (NULL is
 * MRT_ERR_INVAL — "resume myself" is meaningless since a running task is
 * never suspended). Resuming a READY task is a harmless no-op; resuming a
 * DELAYED or BLOCKED task is MRT_ERR_STATE (delays and waits must run
 * their course — suspend first if you truly mean to cancel one).
 */
mrt_err_t mrt_task_resume(mrt_task_handle_t task);

/*
 * Delete a task, freeing its slot (and port resources) for reuse. Pass NULL
 * to delete the calling task itself.
 *
 * Two paths, one rule — never free a running stack:
 * - Deleting ANOTHER task reaps it synchronously: its wait is detached,
 *   its port state freed, and the table compacted, all before returning.
 * - Deleting YOURSELF only marks the task DELETED and yields; the idle
 *   task reaps it on its next pass. Code after delete(NULL) NEVER runs —
 *   structure self-deleting tasks as "cleanup, delete, (no return)".
 *
 * Guards: a task holding any mutex cannot be deleted (STATE — unlock
 * first, so no mutex is ever orphaned); rogue handles are INVAL; NULL with
 * no current task is STATE. The idle task has no handle and cannot die.
 * A deleted task's TCB storage belongs to the application and may back a
 * future create once reaping completed (immediately for others, after an
 * idle pass for self).
 */
mrt_err_t mrt_task_delete(mrt_task_handle_t task);

/*
 * Change a task's priority; takes effect at the next scheduling decision.
 * Pass NULL to change the calling task's own priority (valid only inside a
 * task). Note the host/cooperative caveat: a task that lowers its own
 * priority below another READY task keeps running until it yields —
 * preemptive ports (Phase 4/5) reschedule immediately instead.
 *
 * Returns MRT_OK, MRT_ERR_INVAL (rogue handle or prio > MRT_MAX_PRIORITY),
 * or MRT_ERR_STATE (NULL with no current task, or task already deleted).
 */
mrt_err_t mrt_task_set_priority(mrt_task_handle_t task, mrt_prio_t prio);

/*
 * Read a task's priority. NULL reads the caller's own (valid only inside a
 * task). Rogue handles and NULL-without-current are assertion failures in
 * debug builds — this accessor returns a value, so it has no error channel;
 * validate handles with the calls above when they come from outside.
 */
mrt_prio_t mrt_task_get_priority(mrt_task_handle_t task);

/*
 * Delay until an absolute periodic deadline: the fixed-phase alternative to
 * mrt_task_delay_ticks(). Call with *prev_wake holding the previous wake
 * tick (initialize it with mrt_task_get_tick() before the loop) and the
 * desired period; the call blocks until *prev_wake + period, then advances
 * *prev_wake by exactly one period. Because the deadline derives from the
 * last one rather than "now", loop execution time does not accumulate drift
 * — the task stays phase-locked.
 *
 * Overrun: if the deadline already passed (the loop body took longer than
 * one period), the call returns immediately without blocking but still
 * advances *prev_wake by one period, preserving phase for the next round.
 * period == 0 is defined as a yield (same convention as delay_ticks(0)).
 */
void mrt_task_delay_until(mrt_tick_t *prev_wake, mrt_tick_t period);

/*
 * Stack high-water mark: bytes of the task's stack never touched since
 * creation. Ports paint stacks with a pattern at task_init and count the
 * survivors. Use it to size stacks with evidence instead of guesses: run
 * the worst case, read the mark, keep a margin (25-50%), shrink.
 *
 * Pass NULL for the calling task (valid only inside a task). Measuring
 * yourself reads slightly pessimistic — the query's own call frame counts
 * as used. Rogue handles are INVAL, NULL out is INVAL, NULL with no
 * current task is STATE.
 */
mrt_err_t mrt_task_stack_unused(mrt_task_handle_t task, uint32_t *out_bytes);

#endif /* MINI_RTOS_TASK_H */
