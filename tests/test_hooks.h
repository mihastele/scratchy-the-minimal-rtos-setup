/*
 * mini_rtos — tests/test_hooks.h
 *
 * WHAT:  Narrow test-only hooks into kernel state (reset, time travel).
 * WHY:   Tests need isolation (a fresh kernel per case) and edge control
 *        (tick wraparound) that no application should ever touch. A separate
 *        header keeps that power out of the public API: firmware that
 *        includes only the public mini_rtos headers cannot even name these.
 * HOW:   Tests include this header; sched.c implements the functions. Never
 *        include from applications, examples, drivers, or the kernel itself.
 */
#ifndef MINI_RTOS_TEST_HOOKS_H
#define MINI_RTOS_TEST_HOOKS_H

#include "mini_rtos/types.h"

/* Reset kernel to fresh-booted state: no tasks, tick 0, no stop pending. */
void mrt_test_reset(void);

/* Set the tick counter to an arbitrary value (wraparound tests). */
void mrt_test_set_tick(mrt_tick_t t);

/*
 * Fake ISR context for discipline tests: between enter and exit,
 * mrt_port_in_isr() reads true, so blocking calls must refuse with
 * MRT_ERR_ISR. Nests like real interrupts (enter twice needs two exits).
 */
void mrt_test_isr_enter(void);
void mrt_test_isr_exit(void);

#endif /* MINI_RTOS_TEST_HOOKS_H */
