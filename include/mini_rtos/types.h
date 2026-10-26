/*
 * mini_rtos — types.h
 *
 * WHAT:  The vocabulary every other header shares: error codes, tick and
 *        priority types.
 * WHY:   One canonical spelling avoids the classic embedded mess where the
 *        kernel, port, and drivers each invent their own `err_t`.
 * HOW:   Include this (directly or via other mini_rtos headers) anywhere a
 *        public API type is needed. Depends only on <stdint.h>, so it works
 *        on 8-bit AVRs and 64-bit hosts alike.
 */
#ifndef MINI_RTOS_TYPES_H
#define MINI_RTOS_TYPES_H

#include <stdint.h>

/*
 * Every fallible kernel call returns one of these. Zero means success so
 * callers can write the usual `if (mrt_...(...) != MRT_OK)` checks.
 *
 * Rule: ports and drivers reuse these codes instead of inventing their own.
 * A new code is added here only when no existing one fits — check twice.
 */
typedef enum mrt_err {
    MRT_OK = 0,       /* Success. The only "everything is fine" value. */
    MRT_ERR_INVAL = 1,/* Bad argument: NULL pointer, priority too high, ... */
    MRT_ERR_NOMEM = 2,/* No room: task table full, queue full, pool empty. */
    MRT_ERR_TIMEOUT = 3,/* A blocking call ran out of patience (Phase 2+). */
    MRT_ERR_ISR = 4,  /* Called from an ISR a variant that may block. */
    MRT_ERR_STATE = 5 /* Object is in the wrong state for this call. */
} mrt_err_t;

/*
 * Tick count: unsigned 32-bit modular time. It wraps about every 49 days at
 * 1 kHz, which is fine because all comparisons use subtraction (see
 * tick.h). Never compare ticks with `<` directly — always go through the
 * mrt_time_* helpers so wraparound stays correct.
 */
typedef uint32_t mrt_tick_t;

/* Task priority: 0 (lowest/idle) .. MRT_MAX_PRIORITY. Fits a uint8_t on AVR. */
typedef uint8_t mrt_prio_t;

#endif /* MINI_RTOS_TYPES_H */
