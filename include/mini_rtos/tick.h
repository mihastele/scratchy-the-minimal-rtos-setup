/*
 * mini_rtos — tick.h
 *
 * WHAT:  Overflow-safe comparisons for 32-bit modular tick time.
 * WHY:   `mrt_tick_t` wraps every ~49 days at 1 kHz. Code that writes
 *        `if (now >= wake_tick)` breaks across the wrap: just after the
 *        wrap, `now` is small and a pre-wrap `wake_tick` is huge, so a task
 *        that should wake "now" sleeps another 49 days. Subtraction in
 *        two's complement has no such edge: the *difference* is always
 *        correct no matter where the wrap falls, as long as the two times
 *        being compared are less than 2^31 ticks (~24 days) apart — always
 *        true for real delays.
 * HOW:   Header-only `static inline` helpers so they cost nothing and work
 *        identically on host and MCU. Never compare raw ticks with `<`,
 *        `>`, `<=`, `>=` outside these helpers.
 */
#ifndef MINI_RTOS_TICK_H
#define MINI_RTOS_TICK_H

#include <stdbool.h>
#include <stdint.h>

#include "mini_rtos/types.h"

/*
 * True when `now` has reached or passed `when` on the modular timeline.
 * Read "(int32_t)(now - when) >= 0" as: "the signed distance from `when`
 * to `now` is forward". Wrapping changes both values equally, so the
 * distance — and the answer — stays right across the wrap.
 */
static inline bool mrt_time_after_eq(mrt_tick_t now, mrt_tick_t when)
{
    return (int32_t)(now - when) >= 0;
}

/* True while `now` is still strictly before `when`. */
static inline bool mrt_time_before(mrt_tick_t now, mrt_tick_t when)
{
    return (int32_t)(when - now) > 0;
}

/* Ticks from `from` until `to`, going forward (wraps correctly). */
static inline mrt_tick_t mrt_time_diff(mrt_tick_t from, mrt_tick_t to)
{
    return to - from; /* Unsigned subtraction IS modular arithmetic. */
}

#endif /* MINI_RTOS_TICK_H */
