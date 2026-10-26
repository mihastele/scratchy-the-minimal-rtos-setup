/*
 * mini_rtos — kernel/sched.c
 *
 * WHAT:  The scheduler: which READY task runs next, the tick ISR that wakes
 *        DELAYED tasks, the idle task that runs when nothing else can, and
 *        the kernel lifecycle (init/start/run).
 * WHY:   Scheduling policy is the heart of an RTOS. Ours is deliberately the
 *        classic one — fixed priorities with round-robin inside each level —
 *        because it is simple to reason about and matches what learners meet
 *        in FreeRTOS ("preemptive priority + time slicing").
 * HOW:   One scheduler pass = pick the best READY task (or idle), switch to
 *        it until it yields, then run the tick ISR. Time therefore advances
 *        once per pass at the pass boundary — the same order as hardware,
 *        where tasks run during tick T and the timer fires at the quantum's
 *        end. The host calls the tick ISR from the loop; real ports call the
 *        identical function from the timer interrupt (Phase 4/5).
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "mini_rtos/config.h"
#include "mini_rtos/kernel.h"
#include "mini_rtos/port_api.h"
#include "mini_rtos/task.h"
#include "mini_rtos/tick.h"

#include "kernel_priv.h"

static mrt_tick_t s_tick;          /* Monotonic modular time. */
static bool s_inited;              /* mrt_kernel_init() ran successfully. */
static volatile bool s_stop;       /* Stop request (volatile: set by tasks). */
static struct mrt_tcb *s_current;  /* Running task, NULL while scheduling. */
static uint8_t s_cursor;           /* Round-robin cursor: next scan starts */

/* Kernel-owned idle task: NOT in the pick table (see below). */
static mrt_tcb_t s_idle_tcb;

/* Application idle hook, NULL when unset. Cleared by mrt_test_reset. */
static void (*s_idle_hook)(void);

/*
 * The idle task body. Runs only when the table has nothing READY — it is
 * not in the pick table, so it can never crowd out real work; the
 * scheduler falls back to it directly. One hook + port-idle per pass, then
 * back to the scheduler so time keeps flowing.
 */
static void idle_task_body(void *arg)
{
    (void)arg;
    for (;;) {
        /* Janitor first: collect self-deleted tasks so their slots (and
         * port resources) return to circulation on the very next pass. */
        mrt_priv_reap_deleted();
        if (s_idle_hook != NULL) {
            s_idle_hook();
        }
        mrt_port_idle();
        /* Yield every pass: spinning here would freeze the host tick (time
         * advances at the pass boundary) and burn power on hardware. */
        mrt_task_yield();
    }
}

/*
 * Pick the next task: highest priority wins; ties rotate.
 *
 * The scan starts at the cursor and the comparison is STRICTLY greater,
 * which together produce round-robin among equals: the first task found at
 * the winning priority is the longest-waiting one, because after every pick
 * the cursor moves just past it. The very first pick starts at slot 0, so
 * creation order breaks the initial tie. A higher priority anywhere in the
 * table still beats it, since every entry is examined. Returns NULL when
 * nothing is READY (all delayed/suspended) — the caller then runs idle.
 */
static struct mrt_tcb *pick_next(void)
{
    uint8_t n = mrt_priv_task_count();
    if (n == 0u) {
        return NULL; /* No tasks at all — scheduler idles. */
    }

    struct mrt_tcb *best = NULL;
    for (uint8_t i = 0u; i < n; i++) {
        /* i == 0 looks at the cursor; wraparound via %. */
        uint8_t idx = (uint8_t)((s_cursor + i) % n);
        struct mrt_tcb *cand = mrt_priv_task_at(idx);
        if (cand->state == MRT_TASK_READY &&
            (best == NULL || cand->prio > best->prio)) {
            best = cand;
        }
    }
    if (best != NULL) {
        /* Slots are table indices (no delete API yet), so slot+1 lines up
         * with the next scan position. */
        s_cursor = (uint8_t)((best->slot + 1u) % n);
    }
    return best;
}

/*
 * One scheduler pass. Time advances exactly once per pass — at the end, via
 * the tick ISR — so run_ticks(n) takes exactly n passes and the tick the
 * tasks observe during a pass is the tick the pass started in.
 */
static void step(void)
{
    /* Pick through the shared helper, switch, then close the pass. The
     * pass-closing tick is a port call: the host advances virtual time
     * here, while hardware ports no-op (their timer fires for real). */
    mrt_priv_pick_and_set_current();
    mrt_port_switch_to(mrt_priv_current());
    /* The task is back: it yielded (still READY), delayed (DELAYED with a
     * wake tick), suspended itself, or finished (SUSPENDED by the port
     * trampoline). */
    s_current = NULL; /* Between switches nobody runs — say so explicitly. */
    mrt_port_end_of_pass();
}

/* --- Public lifecycle (see kernel.h) --- */

mrt_err_t mrt_kernel_init(void)
{
    /* "Exactly once" is enforced, not just documented: double init would
     * re-run port setup (timers! interrupts!) while tasks exist. */
    if (s_inited) {
        return MRT_ERR_STATE;
    }
    mrt_err_t err = mrt_port_init();
    if (err != MRT_OK) {
        return err;
    }

    /* Build the kernel-owned idle task at MRT_IDLE_SLOT — outside the app
     * table, priority 0, never blocked, never suspended. Failing here fails
     * the boot: a kernel that cannot idle cannot run. */
    memset(&s_idle_tcb, 0, sizeof(s_idle_tcb));
    strncpy(s_idle_tcb.name, "idle", sizeof(s_idle_tcb.name) - 1u);
    s_idle_tcb.name[sizeof(s_idle_tcb.name) - 1u] = '\0';
    s_idle_tcb.func = idle_task_body;
    s_idle_tcb.prio = 0u;
    s_idle_tcb.state = MRT_TASK_READY;
    s_idle_tcb.slot = MRT_IDLE_SLOT;
    err = mrt_port_task_init(&s_idle_tcb);
    if (err != MRT_OK) {
        return err;
    }

    s_tick = 0u;
    s_current = NULL;
    s_stop = false;
    s_cursor = 0u;
    s_idle_hook = NULL;
    s_inited = true;
    return MRT_OK;
}

mrt_err_t mrt_kernel_start(void)
{
    if (!s_inited) {
        return MRT_ERR_STATE;
    }
    s_stop = false;
    while (!s_stop) {
        step();
    }
    return MRT_OK;
}

mrt_err_t mrt_kernel_run_ticks(mrt_tick_t n)
{
    if (!s_inited) {
        return MRT_ERR_STATE;
    }
    /* End tick computed with wrapping arithmetic; the loop comparison is
     * wrap-safe, so running across the 49-day boundary just works. */
    mrt_tick_t end = s_tick + n;
    while (!s_stop && mrt_time_before(s_tick, end)) {
        step();
    }
    return MRT_OK;
}

void mrt_kernel_request_stop(void)
{
    s_stop = true;
}

void mrt_kernel_set_idle_hook(void (*hook)(void))
{
    s_idle_hook = hook;
}

bool mrt_kernel_on_tick_isr(void)
{
    /* Time advances FIRST so every task woken below observes the new tick.
     * On hardware this runs inside the timer interrupt (possibly preempting
     * a task mid-function); on the host it runs at the pass boundary. Either
     * way the contract is identical: one tick elapsed, wake whoever is due,
     * report whether anyone woke. */
    s_tick++;

    bool woke = false;
    uint8_t n = mrt_priv_task_count();
    for (uint8_t i = 0u; i < n; i++) {
        struct mrt_tcb *t = mrt_priv_task_at(i);
        if (t->state == MRT_TASK_DELAYED &&
            mrt_time_after_eq(s_tick, t->wake_tick)) {
            t->state = MRT_TASK_READY;
            if (t->wait_obj != NULL) {
                /* An IPC waiter whose deadline passed: report TIMEOUT and
                 * detach, so a later post cannot "wake" an already-running
                 * task and corrupt its next wait. */
                t->wait_result = MRT_ERR_TIMEOUT;
                t->wait_obj = NULL;
                t->wait_kind = MRT_WAIT_NONE;
            }
            woke = true;
        }
    }
    return woke;
}

/* --- Private accessors for task.c (see kernel_priv.h) --- */

bool mrt_priv_is_inited(void)
{
    return s_inited;
}

struct mrt_tcb *mrt_priv_current(void)
{
    return s_current;
}

void mrt_priv_pick_and_set_current(void)
{
    s_current = pick_next();
    if (s_current == NULL) {
        /* Nothing READY: the kernel idle task. The cursor is untouched, so
         * rotation among real tasks resumes exactly where it paused. */
        s_current = &s_idle_tcb;
    }
}

mrt_tick_t mrt_priv_tick_now(void)
{
    return s_tick;
}

/* --- Test hooks (declared in tests/test_hooks.h) --- */

void mrt_test_reset(void)
{
    /* Full kernel reset for test isolation: each test case starts with no
     * tasks and tick 0, as if freshly booted. Stays "inited" — reset is not
     * un-init. The idle task survives (kernel-owned, still READY, port
     * state intact) but the app hook is cleared so cases cannot leak idle
     * observers into each other. Port app-slots are fully overwritten by
     * the next creates, so no port reset is needed. */
    mrt_priv_task_reset_table();
    s_tick = 0u;
    s_current = NULL;
    s_stop = false;
    s_cursor = 0u;
    s_idle_hook = NULL;
}

void mrt_test_set_tick(mrt_tick_t t)
{
    /* Time travel for wraparound tests: park the tick just below UINT32_MAX
     * and verify delays still wake. Never used by applications. */
    s_tick = t;
}
