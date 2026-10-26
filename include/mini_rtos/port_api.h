/*
 * mini_rtos — port_api.h
 *
 * WHAT:  THE porting contract. These few functions are everything a new chip
 *        must implement; the kernel calls only these and nothing else.
 * WHY:   A small contract means small ports, and small ports mean fewer
 *        places for hardware-specific bugs to hide. If you are porting to a
 *        new MCU, this file is your checklist — implement each function,
 *        then run the host test suite's twins on your board.
 * HOW:   Each port directory (port/host, port/avr, ...) provides definitions
 *        for all of these. The kernel includes this header; applications
 *        normally never do.
 */
#ifndef MINI_RTOS_PORT_API_H
#define MINI_RTOS_PORT_API_H

#include <stdbool.h>

#include "mini_rtos/types.h"

/* Forward declaration only — the full struct lives in task.h. The port
 * layer includes task.h for the real definition; the declaration here keeps
 * this header light so kernel.c files control include order. */
struct mrt_tcb;

/*
 * Prepare the port: timers, interrupt priorities, any static port state.
 * Called once from mrt_kernel_init(), before any task exists.
 */
mrt_err_t mrt_port_init(void);

/*
 * Attach machine state to a freshly created task: allocate/set up its stack
 * and initial register frame so the first switch "returns" into the task
 * body. The kernel has already filled the TCB; the port stores its private
 * state and points tcb->port_state at it.
 */
mrt_err_t mrt_port_task_init(struct mrt_tcb *tcb);

/*
 * Switch from the scheduler to task `next`. Saves the scheduler context,
 * restores the task's. Returns to the scheduler when the task yields,
 * delays, or finishes.
 */
void mrt_port_switch_to(struct mrt_tcb *next);

/*
 * Switch from the current task back to the scheduler. Saves the task
 * context, restores the scheduler's. Only the running task calls this
 * (via yield/delay/task-return paths).
 */
void mrt_port_yield_to_sched(void);

/*
 * Enter/leave a critical section: code between the two runs atomically with
 * respect to the scheduler and (on hardware) interrupts. May nest — the
 * port counts nesting and re-enables only at the outermost exit. The host
 * port's version is a nesting counter only (single-threaded, nothing to
 * mask); MCU ports mask interrupts. Always pair them; unbalanced exits are
 * a kernel panic via MRT_ASSERT (see below).
 */
void mrt_port_enter_critical(void);
void mrt_port_exit_critical(void);

/*
 * Idle action, called from the idle task on every idle pass (after the
 * application idle hook, if any). This is where ports sleep: the host
 * version does nothing (it has no power bill), while MCU ports execute
 * their wait-for-interrupt with the wake sources the application needs.
 * Must return promptly when an interrupt arrives and must never block.
 */
void mrt_port_idle(void);

/*
 * Free a reaped task's port resources (the mirror of mrt_port_task_init).
 * Called exactly once per successful task_init, from the reap path — never
 * for the running task, so freeing its stack/context is always safe. After
 * this returns, the TCB's port_state must be treated as garbage.
 */
void mrt_port_task_delete(struct mrt_tcb *tcb);

/*
 * Stack high-water query: bytes of `tcb`'s stack still holding the
 * creation-time paint pattern (see mrt_task_stack_unused for the user
 * story). Ports that cannot track stacks return 0 — honestly, not guessed.
 * NULL asserts in debug builds and reads 0 in release.
 */
uint32_t mrt_port_stack_unused(const struct mrt_tcb *tcb);

/*
 * True while the calling context is an ISR. Blocking IPC calls check this
 * and refuse with MRT_ERR_ISR (an ISR must never sleep — it has no task
 * context to resume); their _isr twins exist for exactly that context.
 * Hardware ports track real interrupt nesting; the host port exposes test
 * hooks (mrt_test_isr_enter/exit) that flip the same flag.
 */
bool mrt_port_in_isr(void);

/*
 * Kernel panic: the port stops everything and reports `file:line`. Used for
 * "this can never happen" internal failures (unbalanced critical section,
 * null current task). On host it prints and aborts; on MCU it typically
 * disables interrupts and blinks a fault LED forever. Applications never
 * call this directly — they use MRT_ASSERT.
 */
void mrt_port_panic(const char *file, int line);

/* Internal invariant check. Compiles to nothing when NDEBUG is defined
 * (release firmware), so keep the condition side-effect free. */
#ifdef NDEBUG
#define MRT_ASSERT(cond) ((void)0)
#else
#define MRT_ASSERT(cond)                              \
    do {                                              \
        if (!(cond)) {                                \
            mrt_port_panic(__FILE__, __LINE__);       \
        }                                             \
    } while (0)
#endif

#endif /* MINI_RTOS_PORT_API_H */
