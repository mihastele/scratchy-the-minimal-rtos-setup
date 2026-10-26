/*
 * mini_rtos — port/host/port.c
 *
 * WHAT:  The simulator port: implements port_api.h on Linux/macOS using
 *        POSIX ucontext for context switches and a virtual tick.
 * WHY:   Real context switches are assembly + timers. ucontext gives the
 *        same shape in plain C — each task owns a stack and a saved register
 *        set, and switching swaps one for the other — so the kernel and its
 *        tests run deterministically on a laptop years before silicon.
 * HOW:   - mrt_port_task_init() builds a task context on a private stack
 *          whose entry point is task_trampoline().
 *        - The scheduler runs on s_sched_ctx. switch_to() saves it and
 *          restores the task; yield_to_sched() does the reverse.
 *        - Critical sections are a nesting counter only: the simulator is
 *          single-threaded with no interrupts, so there is nothing to mask.
 *          The counter still catches unbalanced enter/exit pairs.
 *        - The kernel calls mrt_kernel_on_tick_isr() once per pass to stand
 *          in for the hardware timer interrupt (see sched.c).
 *
 * HOST-ONLY: ucontext never ships on a microcontroller. The AVR port
 * (Phase 4) replaces this file with assembly save/restore + a timer ISR,
 * and the kernel never notices.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

#include "mini_rtos/config.h"
#include "mini_rtos/port_api.h"
#include "mini_rtos/task.h"

/*
 * One task's machine state. Lives in the port layer (the kernel only holds
 * an opaque `port_state` pointer) so kernel code can never depend on what a
 * "context" looks like — on AVR this struct will be a stack pointer.
 */
typedef struct host_task_state {
    ucontext_t ctx;                        /* Saved registers + stack. */
    uint8_t stack[MRT_HOST_TASK_STACK_SIZE]; /* The task's private stack. */
    struct mrt_tcb *tcb;                   /* Back-pointer for trampoline. */
} host_task_state_t;

/* Indexed by task slot: app slots 0..MRT_MAX_TASKS-1 plus the kernel idle
 * task at MRT_IDLE_SLOT. Statically allocated like everything else: the
 * simulator mirrors the no-heap rule. (Yes, this is ~512 KB of BSS on the
 * host — free on a laptop, and MCU ports size stacks per task instead.) */
static host_task_state_t s_state[MRT_MAX_TASKS + 1u];

/* The scheduler's own context: switch_to() saves here, yield restores. */
static ucontext_t s_sched_ctx;

/* Task currently switched in, from the port's point of view. The kernel
 * tracks its own copy too; both change at the same switch boundaries. */
static struct mrt_tcb *s_port_current;

/* Critical-section nesting depth. Counter only — see file header. */
static unsigned s_crit_nesting;

/*
 * Entry point of every task context. makecontext() only passes `int`
 * arguments, so it receives the slot index (0..255 survives int
 * round-trips; a 64-bit pointer would not) and looks the TCB up.
 */
static void task_trampoline(int slot)
{
    host_task_state_t *st = &s_state[slot];
    struct mrt_tcb *tcb = st->tcb;

    tcb->func(tcb->arg);

    /* The task function returned: the task is finished. Suspend it so the
     * scheduler never resumes this dead context, then yield away for the
     * last time. swapcontext saves here, but SUSPENDED means we never come
     * back — the abort below is unreachable by construction. */
    tcb->state = MRT_TASK_SUSPENDED;
    mrt_port_yield_to_sched();
    abort();
}

mrt_err_t mrt_port_init(void)
{
    /* Nothing hardware to set up; just establish a known state. s_sched_ctx
     * needs no init — the first swapcontext() fills it by saving. */
    s_port_current = NULL;
    s_crit_nesting = 0u;
    for (unsigned i = 0u; i < MRT_MAX_TASKS + 1u; i++) {
        s_state[i].tcb = NULL;
    }
    return MRT_OK;
}

mrt_err_t mrt_port_task_init(struct mrt_tcb *tcb)
{
    /* MRT_IDLE_SLOT is the one legal out-of-table slot (the kernel idle
     * task); anything beyond it is a kernel bug, caught here. */
    if (tcb == NULL || tcb->slot > MRT_IDLE_SLOT) {
        return MRT_ERR_INVAL;
    }

    /* Fully rebuild the slot's context every time, so re-creating a task in
     * a reused slot (tests do this after mrt_test_reset()) cannot inherit
     * stale register state. */
    host_task_state_t *st = &s_state[tcb->slot];
    if (getcontext(&st->ctx) != 0) {
        return MRT_ERR_STATE;
    }
    /* ss_sp is the LOW address of the stack area; the CPU grows down from
     * the top automatically. Getting this backwards is the classic ucontext
     * crash — stack grows into unmapped memory on first push. */
    st->ctx.uc_stack.ss_sp = st->stack;
    st->ctx.uc_stack.ss_size = sizeof(st->stack);
    /* Backstop: if the trampoline ever returned instead of yielding, resume
     * the scheduler rather than falling off the stack into nowhere. */
    st->ctx.uc_link = &s_sched_ctx;
    st->tcb = tcb;

    /* The (void (*)(void)) cast is the documented makecontext idiom: the
     * call actually passes one int, matching task_trampoline(int). */
    makecontext(&st->ctx, (void (*)(void))task_trampoline, 1, (int)tcb->slot);

    tcb->port_state = st;
    return MRT_OK;
}

void mrt_port_switch_to(struct mrt_tcb *next)
{
    MRT_ASSERT(next != NULL && next->port_state != NULL);
    s_port_current = next;
    host_task_state_t *st = (host_task_state_t *)next->port_state;
    /* Save scheduler context into s_sched_ctx, restore the task's. When the
     * task later yields, execution resumes on the line below. */
    if (swapcontext(&s_sched_ctx, &st->ctx) != 0) {
        mrt_port_panic(__FILE__, __LINE__);
    }
}

void mrt_port_yield_to_sched(void)
{
    MRT_ASSERT(s_port_current != NULL && s_port_current->port_state != NULL);
    host_task_state_t *st = (host_task_state_t *)s_port_current->port_state;
    s_port_current = NULL;
    /* Save task context, restore scheduler. The task resumes after this
     * line the next time it is picked. */
    if (swapcontext(&st->ctx, &s_sched_ctx) != 0) {
        mrt_port_panic(__FILE__, __LINE__);
    }
}

void mrt_port_enter_critical(void)
{
    s_crit_nesting++;
}

void mrt_port_exit_critical(void)
{
    /* Unbalanced exit (more exits than enters) is always a bug — fail
     * loudly in debug builds, and clamp in release so one mistake does not
     * wrap the counter into a permanently-"critical" state. */
    MRT_ASSERT(s_crit_nesting > 0u);
    if (s_crit_nesting > 0u) {
        s_crit_nesting--;
    }
}

void mrt_port_idle(void)
{
    /* The simulator has no power modes: idling is a no-op. Deliberately
     * silent — idle observation belongs to the application idle hook, and a
     * print here would spam every all-delayed test. MCU ports sleep here. */
}

void mrt_port_panic(const char *file, int line)
{
    /* Host panic: report and stop the world. MCU ports replace this with
     * interrupt-disable + fault LED, but the call sites never change. */
    fprintf(stderr, "mini_rtos PANIC at %s:%d\n", file != NULL ? file : "?", line);
    fflush(stderr);
    abort();
}
