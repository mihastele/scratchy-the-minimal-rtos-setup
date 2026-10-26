/*
 * mini_rtos — config.h
 *
 * WHAT:  Compile-time knobs for the whole OS: table sizes, tick rate,
 *        priority range. One file so every memory budget is visible.
 * WHY:   On a microcontroller there is no "just add RAM". Sizing every
 *        table up front is what makes the kernel fit a 2 KB-SRAM Uno.
 * HOW:   Applications may override any knob by defining it before including
 *        mini_rtos headers (e.g. `-DMRT_MAX_TASKS=4`). Defaults below fit
 *        the host simulator and small AVRs.
 */
#ifndef MINI_RTOS_CONFIG_H
#define MINI_RTOS_CONFIG_H

/* Maximum tasks in the system, including the idle task once Phase 1 adds
 * it. The task table is a fixed C array of this size — no linked-list
 * growth, no heap. 8 fits the demos with room to spare. */
#ifndef MRT_MAX_TASKS
#define MRT_MAX_TASKS 8u
#endif

/* Tick interrupts per second on real hardware. The host simulator uses
 * virtual ticks (one per scheduler pass) and ignores the wall-clock rate,
 * but delays are still expressed in these units so code stays portable. */
#ifndef MRT_TICK_HZ
#define MRT_TICK_HZ 1000u
#endif

/* Highest valid task priority. 0 is the lowest (idle level); larger numbers
 * preempt smaller ones. A small range keeps the ready-bitmap cheap on 8-bit
 * CPUs: priorities 0..7 fit one byte map. */
#ifndef MRT_MAX_PRIORITY
#define MRT_MAX_PRIORITY 7u
#endif

/* Task names are for humans reading logs, truncated (not an error) when too
 * long. 16 bytes covers names like "producer_1" with the NUL terminator. */
#ifndef MRT_MAX_TASK_NAME_LEN
#define MRT_MAX_TASK_NAME_LEN 16u
#endif

/* Host-simulator stack per task. Generous on purpose: the host port uses
 * POSIX ucontext with plain RAM, so 64 KB costs nothing and stack-size bugs
 * cannot hide here. MCU ports ignore this — each task's real stack is sized
 * by the application for its chip. */
#ifndef MRT_HOST_TASK_STACK_SIZE
#define MRT_HOST_TASK_STACK_SIZE (64u * 1024u)
#endif

/*
 * Slot index reserved for the kernel's idle task: one past the application
 * table, so all MRT_MAX_TASKS slots stay available to applications. Ports
 * size their per-task state arrays MRT_MAX_TASKS + 1 and index the idle
 * task here. Never assigned by mrt_task_create_static.
 */
#define MRT_IDLE_SLOT MRT_MAX_TASKS

#endif /* MINI_RTOS_CONFIG_H */
