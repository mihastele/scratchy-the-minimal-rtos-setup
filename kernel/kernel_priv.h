/*
 * mini_rtos — kernel/kernel_priv.h
 *
 * WHAT:  Private sharing between kernel .c files (task.c, sched.c).
 * WHY:   The task table lives in task.c but the scheduler in sched.c must
 *        scan it; the tick and "current task" live in sched.c but delay()
 *        in task.c must read them. Rather than globals shared by accident,
 *        this header lists the exact private accessors — a miniature
 *        version of the kernel/port split. Tests and applications must
 *        never include this file; tests get their own narrow hooks in
 *        tests/test_hooks.h.
 */
#ifndef MINI_RTOS_KERNEL_PRIV_H
#define MINI_RTOS_KERNEL_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include "mini_rtos/types.h"

struct mrt_tcb; /* Full definition in mini_rtos/task.h. */

/* --- Owned by task.c: the task table --- */

/* Number of created tasks (0..MRT_MAX_TASKS). */
uint8_t mrt_priv_task_count(void);

/* Table entry by slot index (slot < count). Never NULL in range. */
struct mrt_tcb *mrt_priv_task_at(uint8_t slot);

/* Forget all tasks. Test-only path behind mrt_test_reset(). */
void mrt_priv_task_reset_table(void);

/* --- Owned by sched.c: lifecycle, time, and the running task --- */

/* True once mrt_kernel_init() has succeeded. Gates creation order. */
bool mrt_priv_is_inited(void);

/* Currently running task, or NULL while the scheduler itself runs. */
struct mrt_tcb *mrt_priv_current(void);

/* Current tick count (same value mrt_task_get_tick() reports). */
mrt_tick_t mrt_priv_tick_now(void);

#endif /* MINI_RTOS_KERNEL_PRIV_H */
