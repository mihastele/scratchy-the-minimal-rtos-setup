/*
 * mini_rtos — kernel/kernel_priv.h
 *
 * WHAT:  Private sharing between kernel .c files (task.c, sched.c, wait.c,
 *        and the IPC objects). The waiter protocol lives here.
 * WHY:   The task table lives in task.c but the scheduler, the tick ISR,
 *        and every IPC object must scan it; the tick and "current task" live
 *        in sched.c but delays and waits must read them. Rather than globals
 *        shared by accident, this header lists the exact private accessors —
 *        a miniature version of the kernel/port split. Tests and
 *        applications must never include this file; tests get their own
 *        narrow hooks in tests/test_hooks.h. PORTS may include it —
 *        preemptive tick ISRs need the current-task and pick helpers, and
 *        there is nowhere else honest to get them.
 *
 * THE WAITER PROTOCOL (how blocking works — read this before touching IPC):
 *
 * A task blocked on an IPC object is DELAYED (timed wait) or BLOCKED
 * (MRT_WAIT_FOREVER) with wait_obj pointing at the object and wait_kind
 * saying what it waits for. Exactly one of three things ends the wait:
 *   1. The object wakes it (mrt_priv_wait_wake_one or the event broadcast):
 *      state READY, wait_result OK, wait fields detached.
 *   2. The deadline passes (tick ISR): state READY, wait_result TIMEOUT,
 *      wait fields detached.
 *   3. Someone suspends it: state SUSPENDED, wait_result STATE ("the wait
 *      is dead"), wait fields detached. On resume, the abandoned blocking
 *      call returns STATE instead of re-blocking.
 * wait_obj != NULL always means "actively blocked" — suspend and the ISR
 * both detach, so a stale wake can never resurrect a dead wait.
 */
#ifndef MINI_RTOS_KERNEL_PRIV_H
#define MINI_RTOS_KERNEL_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include "mini_rtos/types.h"

struct mrt_tcb; /* Full definition in mini_rtos/task.h. */

/*
 * Wait flavors: what an IPC-blocked task waits FOR. Stored in
 * TCB.wait_kind; matched by the waking side. One flavor per blocking
 * direction so a queue's senders and receivers never steal wakes.
 */
enum mrt_wait_kind {
    MRT_WAIT_NONE = 0, /* Not blocked on any IPC object. */
    MRT_WAIT_QUEUE_SEND, /* Queue has no free slot. */
    MRT_WAIT_QUEUE_RECV, /* Queue has no item. */
    MRT_WAIT_SEM,        /* Semaphore count is zero. */
    MRT_WAIT_MUTEX,       /* Mutex owned by someone else. */
    MRT_WAIT_EVENT        /* Event bits do not satisfy the mask yet. */
};

/* --- Owned by task.c: the task table --- */

/* Number of created tasks (0..MRT_MAX_TASKS). */
uint8_t mrt_priv_task_count(void);

/* Table entry by slot index (slot < count). Never NULL in range. */
struct mrt_tcb *mrt_priv_task_at(uint8_t slot);

/* Forget all tasks. Test-only path behind mrt_test_reset(). */
void mrt_priv_task_reset_table(void);

/*
 * Reap tasks marked MRT_TASK_DELETED (self-deletes): free port state and
 * compact them out of the table. Called from the idle task every pass;
 * returns immediately when nothing died. No-op safe before init.
 */
void mrt_priv_reap_deleted(void);

/* --- Owned by sched.c: lifecycle, time, and the running task --- */

/* True once mrt_kernel_init() has succeeded. Gates creation order. */
bool mrt_priv_is_inited(void);

/* Currently running task, or NULL while the scheduler itself runs. */
struct mrt_tcb *mrt_priv_current(void);

/*
 * Pick the next task and make it current (falling back to the idle task).
 * Shared by the scheduler loop AND preemptive tick ISRs so host and
 * hardware always agree on who runs next.
 */
void mrt_priv_pick_and_set_current(void);

/* Current tick count (same value mrt_task_get_tick() reports). */
mrt_tick_t mrt_priv_tick_now(void);

/* --- Owned by wait.c: the waiter registry --- */

/*
 * Arm the current task as a waiter on `obj`. The caller MUST hold the
 * port critical section across its check-then-arm sequence: on preemptive
 * hardware, an ISR posting between "queue full?" and "block" would
 * otherwise strand the task until its timeout (the classic missed-wakeup
 * race). timeout==MRT_WAIT_FOREVER blocks without a deadline; timeout==0
 * is never armed (callers return directly instead).
 */
void mrt_priv_wait_arm(void *obj, uint8_t kind, mrt_tick_t timeout);

/*
 * Wake the best waiter on `obj`/`kind`: highest effective priority wins,
 * ties broken by longest waiting (wait_seq FIFO). Detaches the waiter,
 * marks it READY with result OK, and returns it — or NULL when nobody
 * waits. The caller MUST hold the critical section (the scan-and-wake is
 * one atomic decision). Events do NOT use this (they broadcast to every
 * matching waiter — see event.c).
 */
struct mrt_tcb *mrt_priv_wait_wake_one(void *obj, uint8_t kind);

#endif /* MINI_RTOS_KERNEL_PRIV_H */
