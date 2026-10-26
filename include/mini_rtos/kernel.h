/*
 * mini_rtos — kernel.h
 *
 * WHAT:  Whole-OS lifecycle: init once, create tasks, start the scheduler.
 * WHY:   A kernel needs a defined boot order — port first (so tick and
 *        stacks exist), then tasks, then the scheduler loop. Skipping init
 *        is the most common bring-up bug, so start() refuses to run before
 *        init() succeeded.
 * HOW:   Typical main(): mrt_kernel_init() -> create tasks -> either
 *        mrt_kernel_start() (runs until stopped — the MCU shape) or
 *        mrt_kernel_run_ticks(n) (bounded run — the simulator/test shape).
 */
#ifndef MINI_RTOS_KERNEL_H
#define MINI_RTOS_KERNEL_H

#include <stdbool.h>

#include "mini_rtos/types.h"

/*
 * Initialize the kernel and its port layer. Call exactly once before any
 * other mrt_ call. Safe to call again only after a full reset (tests use
 * the test hooks in tests/test_hooks.h instead of re-init).
 */
mrt_err_t mrt_kernel_init(void);

/*
 * Enter the scheduler loop; returns only after mrt_kernel_request_stop().
 * On a microcontroller this never returns (there is nothing to return to —
 * main's stack is recycled). Never call from inside a task or ISR.
 */
mrt_err_t mrt_kernel_start(void);

/*
 * Run the scheduler for at most `n` ticks, then return. This is the
 * simulator/test entry point: deterministic, bounded, and reviewable. On
 * hardware it is still valid (runs n ticks of real time) but rarely used.
 */
mrt_err_t mrt_kernel_run_ticks(mrt_tick_t n);

/*
 * Ask a running scheduler loop (start() or run_ticks()) to return at the
 * next convenient point. Safe from tasks; the flag is checked between task
 * switches, never mid-task, so no task is left half-switched.
 */
void mrt_kernel_request_stop(void);

/*
 * Install (or clear, with NULL) the application idle hook: called from the
 * idle task on every idle pass, before the port's own idle action. Use it
 * for background work that must never block — stats, watchdog kicks,
 * sleep accounting. It runs in the idle task's context: blocking or
 * deleting here starves the hook itself, and long work delays nothing else
 * (by definition nothing else wants the CPU) but postpones sleep.
 */
void mrt_kernel_set_idle_hook(void (*hook)(void));

/*
 * Tick ISR entry: advance time by one tick and wake due tasks. Called by
 * the hardware timer ISR on real ports (Phase 4/5), and once per scheduler
 * pass by the host simulator — same function, same semantics, which is
 * what keeps host tests honest about hardware timing behavior.
 *
 * Returns true when at least one task was woken. Ports use this to decide
 * whether the ISR must trigger a reschedule (on hardware, waking a
 * higher-priority task preempts the interrupted one).
 *
 * ISR-SAFE by construction: touches only the tick and task states under the
 * port's ISR discipline. Never blocks, never yields, never touches the
 * idle hook.
 */
bool mrt_kernel_on_tick_isr(void);

#endif /* MINI_RTOS_KERNEL_H */
