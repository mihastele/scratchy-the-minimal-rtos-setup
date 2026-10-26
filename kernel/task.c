/*
 * mini_rtos — kernel/task.c
 *
 * WHAT:  Task creation and the task-side blocking API (yield, delay,
 *        delay_until, suspend/resume, priorities).
 * WHY:   Creating a task is bookkeeping plus one port call: the kernel
 *        validates arguments, fills the TCB, and files it in a fixed table;
 *        the port builds the machine state (stack + first register frame).
 *        Keeping the table fixed-size is what lets this kernel run without
 *        a heap — creation can fail with NOMEM, never with fragmentation.
 *
 * SLOT MODEL: slot N always means table index N. There is no task-delete
 * API yet (deferred reaping arrives with the memory work in Phase 3), so
 * slots are never recycled and a slot's TCB pointer never changes. The
 * host port leans on this to find per-task machine state. The idle task
 * lives outside the table at MRT_IDLE_SLOT and is invisible to all of this
 * file's scans — it can neither be suspended nor have its priority read.
 */
#include <stdbool.h>
#include <string.h>

#include "mini_rtos/config.h"
#include "mini_rtos/port_api.h"
#include "mini_rtos/task.h"
#include "mini_rtos/tick.h"

#include "kernel_priv.h"

/* Slots ride in a uint8_t (TCB.slot, table index math), so a giant
 * MRT_MAX_TASKS would silently truncate. Catch a bad config at compile
 * time instead of debugging phantom tasks at runtime. */
_Static_assert(MRT_MAX_TASKS <= 255u, "MRT_MAX_TASKS must fit in uint8_t");

/* The whole task set: fixed pointers, filled left to right. s_count is both
 * the task count and the next free index. Touched under critical section on
 * creation so a tick ISR on real hardware can never see a half-filed task. */
static struct mrt_tcb *s_table[MRT_MAX_TASKS];
static uint8_t s_count;

/* True while any entry is MRT_TASK_DELETED: lets the idle reaper skip its
 * scan on the (usual) nothing-died passes. Set under critical, cleared by
 * the reaper itself. */
static bool s_zombies;

/*
 * True when the handle names a live table entry. O(n) with n <= 8: far
 * cheaper than debugging a wild-pointer TCB write. The idle task is NOT in
 * the table, so this also rejects any attempt to control it — by design.
 */
static bool handle_valid(mrt_task_handle_t task)
{
    uint8_t n = mrt_priv_task_count();
    for (uint8_t i = 0u; i < n; i++) {
        if (mrt_priv_task_at(i) == task) {
            return true;
        }
    }
    return false;
}

/*
 * Block the current task until absolute tick `wake`. Both delay flavors
 * funnel through here so there is exactly one place that writes
 * wake_tick/state — one place to get the atomicity right.
 */
static void delay_until_abs(struct mrt_tcb *self, mrt_tick_t wake)
{
    mrt_port_enter_critical();
    self->wake_tick = wake;
    self->state = MRT_TASK_DELAYED;
    mrt_port_exit_critical();
    /* NOTE for the preemptive future (Phase 4): on hardware, the tick ISR
     * could fire between exit_critical and yield_to_sched. That is benign
     * here — the task is already DELAYED, so the worst case is waking one
     * tick "late", i.e. exactly on time. IPC paths in Phase 2 will need the
     * stricter lock-the-scheduler discipline, covered there. */
    mrt_port_yield_to_sched();
}

mrt_err_t mrt_task_create_static(const char *name,
                                 mrt_task_func_t func,
                                 void *arg,
                                 mrt_prio_t prio,
                                 mrt_tcb_t *tcb_storage,
                                 mrt_task_handle_t *out_handle)
{
    /* Validate everything BEFORE the critical section: fail fast, and keep
     * the atomic region (interrupts masked on hardware!) as short as it can
     * possibly be. */
    if (func == NULL || tcb_storage == NULL || out_handle == NULL) {
        return MRT_ERR_INVAL;
    }
    if (prio > MRT_MAX_PRIORITY) {
        return MRT_ERR_INVAL;
    }
    /* Boot order is law: the port layer (stacks, timers) does not exist
     * until init ran, so creating earlier would build on nothing. */
    if (!mrt_priv_is_inited()) {
        return MRT_ERR_STATE;
    }

    mrt_port_enter_critical();

    /* Creation is allowed even after the scheduler runs: the new task is
     * simply picked up on the next pass. The critical section is what makes
     * that safe — the scheduler never observes a partially built TCB. */
    if (s_count >= MRT_MAX_TASKS) {
        mrt_port_exit_critical();
        return MRT_ERR_NOMEM;
    }

    /* Start from zeroes so every field is defined even if we forget one
     * below; in particular port_state starts NULL until the port sets it. */
    memset(tcb_storage, 0, sizeof(*tcb_storage));

    if (name != NULL) {
        /* Truncate, always NUL-terminate: strncpy's non-terminating edge is
         * why the explicit last-byte write exists. */
        strncpy(tcb_storage->name, name, sizeof(tcb_storage->name) - 1u);
        tcb_storage->name[sizeof(tcb_storage->name) - 1u] = '\0';
    }
    /* else: leave name as "" — anonymous tasks are legal, just harder to
     * find in logs. */

    tcb_storage->func = func;
    tcb_storage->arg = arg;
    tcb_storage->prio = prio;
    tcb_storage->base_prio = prio; /* boost starts 0 (memset above). */
    tcb_storage->state = MRT_TASK_READY;
    tcb_storage->slot = s_count;

    /* The port builds machine state now, while the task is still invisible
     * to the scheduler (not yet in s_table). On failure the TCB is simply
     * abandoned — nothing was published, so nothing needs unpublishing. */
    mrt_err_t err = mrt_port_task_init(tcb_storage);
    if (err != MRT_OK) {
        mrt_port_exit_critical();
        return err;
    }

    /* Publish: exactly one write each, so any observer sees either the old
     * count or a fully built task — never a torn entry. */
    s_table[s_count] = tcb_storage;
    *out_handle = tcb_storage;
    s_count++;

    mrt_port_exit_critical();
    return MRT_OK;
}

void mrt_task_yield(void)
{
    /* Yielding outside a task (from main, or from the scheduler's own
     * context) has no "current task" to save — a certain crash later, so
     * fail loudly here in debug builds. */
    MRT_ASSERT(mrt_priv_current() != NULL);
    mrt_port_yield_to_sched();
}

void mrt_task_delay_ticks(mrt_tick_t ticks)
{
    struct mrt_tcb *self = mrt_priv_current();
    MRT_ASSERT(self != NULL);

    if (ticks == 0u) {
        /* Defined convention (see task.h): delay(0) is a yield. */
        mrt_port_yield_to_sched();
        return;
    }

    /* Unsigned addition wraps by definition — a delay across the 49-day
     * tick wrap still lands on the right wake tick, and the tick ISR's
     * mrt_time_after_eq() comparison reads it correctly. */
    delay_until_abs(self, mrt_priv_tick_now() + ticks);
}

void mrt_task_delay_until(mrt_tick_t *prev_wake, mrt_tick_t period)
{
    struct mrt_tcb *self = mrt_priv_current();
    MRT_ASSERT(self != NULL && prev_wake != NULL);

    if (period == 0u) {
        /* Defined convention (see task.h): period 0 is a yield. */
        mrt_port_yield_to_sched();
        return;
    }

    /* Deadline from the last deadline, not from now: loop-body jitter does
     * not shift the phase. Unsigned wrap is correct by construction. */
    mrt_tick_t wake = *prev_wake + period;
    *prev_wake = wake;
    if (mrt_time_before(mrt_priv_tick_now(), wake)) {
        delay_until_abs(self, wake);
    }
    /* else: overrun — return immediately with phase preserved (see task.h). */
}

mrt_tick_t mrt_task_get_tick(void)
{
    /* Reading one 32-bit tick is atomic on 32-bit CPUs. On 8-bit AVR it is
     * NOT (four separate byte loads a tick ISR could split), so Phase 4's
     * port wraps this read in a critical section. The host port needs none. */
    return mrt_priv_tick_now();
}

mrt_err_t mrt_task_suspend(mrt_task_handle_t task)
{
    bool self = (task == NULL);
    struct mrt_tcb *t = self ? mrt_priv_current() : task;
    /* No assert here by design: this function HAS an error channel, so the
     * STATE return below is the contract — debug and release agree, and the
     * path stays testable. (Asserts are for APIs without one: yield, delay,
     * get_priority.) */
    if (t == NULL) {
        return MRT_ERR_STATE; /* NULL with no current task. */
    }
    if (!handle_valid(t)) {
        /* Rogue explicit handle — or NULL-from-idle: the idle task is
         * kernel-owned and suspending it would leave nothing to run. */
        return self ? MRT_ERR_STATE : MRT_ERR_INVAL;
    }
    if (t->state == MRT_TASK_DELETED) {
        /* Already dying: idempotent OK. Never "resurrect" into SUSPENDED —
         * the idle reaper only collects DELETED, and anything else leaks. */
        return MRT_OK;
    }

    mrt_port_enter_critical();
    /* Unconditional write: READY, DELAYED, and BLOCKED all become SUSPENDED,
     * so a delay or IPC wait in progress is abandoned (see task.h). A live
     * wait is detached with report STATE, so the resumed task's blocking
     * call returns STATE instead of looping back into the wait. */
    t->state = MRT_TASK_SUSPENDED;
    if (t->wait_obj != NULL) {
        t->wait_obj = NULL;
        t->wait_kind = MRT_WAIT_NONE;
        t->wait_result = MRT_ERR_STATE;
    }
    mrt_port_exit_critical();

    if (t == mrt_priv_current()) {
        /* Suspending ourselves: get off the CPU now. Any other task is
         * already off it (only one task runs), so the state change alone
         * takes effect at the next pick. */
        mrt_port_yield_to_sched();
    }
    return MRT_OK;
}

mrt_err_t mrt_task_resume(mrt_task_handle_t task)
{
    if (task == NULL) {
        /* "Resume myself" is meaningless — a running task is never the
         * suspended one — so NULL is rejected rather than guessed at. */
        return MRT_ERR_INVAL;
    }
    if (!handle_valid(task)) {
        return MRT_ERR_INVAL;
    }

    mrt_port_enter_critical();
    mrt_err_t err = MRT_OK;
    switch (task->state) {
    case MRT_TASK_SUSPENDED:
        task->state = MRT_TASK_READY;
        break;
    case MRT_TASK_READY:
        break; /* Already eligible; idempotent OK. */
    default:
        /* DELAYED, BLOCKED: waits run their course. DELETED: the dead
         * stay dead — resume must never resurrect a task pending reap. */
        err = MRT_ERR_STATE;
        break;
    }
    mrt_port_exit_critical();
    return err;
}

mrt_err_t mrt_task_set_priority(mrt_task_handle_t task, mrt_prio_t prio)
{
    if (prio > MRT_MAX_PRIORITY) {
        return MRT_ERR_INVAL;
    }
    bool self = (task == NULL);
    struct mrt_tcb *t = self ? mrt_priv_current() : task;
    /* No assert: the STATE return is the contract (see mrt_task_suspend). */
    if (t == NULL) {
        return MRT_ERR_STATE;
    }
    if (!handle_valid(t)) {
        return self ? MRT_ERR_STATE : MRT_ERR_INVAL;
    }
    if (t->state == MRT_TASK_DELETED) {
        return MRT_ERR_STATE; /* Gone is gone: no tuning the dead. */
    }

    /* Write base, recompute effective: a boosted owner keeps its boost
     * (the mutex still needs it); setting below the boost takes full effect
     * when the mutex is released. No immediate reschedule on the host
     * (cooperative — see task.h); preemptive ports yield here when the new
     * priority warrants it. */
    mrt_port_enter_critical();
    t->base_prio = prio;
    t->prio = (prio > t->boost_prio) ? prio : t->boost_prio;
    mrt_port_exit_critical();
    return MRT_OK;
}

mrt_prio_t mrt_task_get_priority(mrt_task_handle_t task)
{
    struct mrt_tcb *t = (task != NULL) ? task : mrt_priv_current();
    /* No error channel (see task.h): rogue use asserts in debug builds.
     * Release builds trust the caller — validate handles beforehand. */
    MRT_ASSERT(t != NULL && handle_valid(t));
    return t->prio;
}

/*
 * Cut `t` out of the table: free its port resources, then compact by moving
 * the last entry into the hole (updating its slot — the slot==index
 * invariant is what keeps the port and the cursor honest). Caller holds
 * critical. The victim must NOT be running: only the reap paths call this.
 */
static void reap_locked(struct mrt_tcb *t)
{
    MRT_ASSERT(t->slot < s_count && s_table[t->slot] == t);
    mrt_port_task_delete(t);
    uint8_t last = (uint8_t)(s_count - 1u);
    if (t->slot != last) {
        s_table[t->slot] = s_table[last];
        s_table[t->slot]->slot = t->slot;
    }
    s_table[last] = NULL; /* Hygiene: no dangling table pointers. */
    s_count = last;
}

mrt_err_t mrt_task_delete(mrt_task_handle_t task)
{
    bool self = (task == NULL);
    struct mrt_tcb *t = self ? mrt_priv_current() : task;
    /* No assert: the STATE return is the contract (see mrt_task_suspend). */
    if (t == NULL) {
        return MRT_ERR_STATE;
    }
    if (!handle_valid(t)) {
        return self ? MRT_ERR_STATE : MRT_ERR_INVAL;
    }
    if (t->state == MRT_TASK_DELETED) {
        return MRT_OK; /* Already dying; the reaper has it. */
    }
    /* Never orphan a mutex: an owner pointer into a reused slot would hand
     * ownership to whatever task comes next. Unlock first, then die. */
    if (t->owned_mutexes > 0u) {
        return MRT_ERR_STATE;
    }

    mrt_port_enter_critical();
    /* Detach any live wait (a blocked task dies unblocked): waiter scans
     * skip detached entries, so boosts and broadcasts self-heal. */
    if (t->wait_obj != NULL) {
        t->wait_obj = NULL;
        t->wait_kind = MRT_WAIT_NONE;
    }
    if (!self) {
        /* Someone else's task is never running: reap synchronously. */
        reap_locked(t);
        mrt_port_exit_critical();
        return MRT_OK;
    }
    /* Ourselves: mark and yield — the idle task reaps on its next pass,
     * once we are safely off our own stack. */
    t->state = MRT_TASK_DELETED;
    s_zombies = true;
    mrt_port_exit_critical();
    mrt_task_yield();
    /* Unreachable: DELETED tasks are never picked again. If a kernel bug
     * ever resumes us, park instead of running dead code. */
    for (;;) {
        mrt_port_yield_to_sched();
    }
    return MRT_OK; /* Placates return-type analysis; never runs. */
}

mrt_err_t mrt_task_stack_unused(mrt_task_handle_t task, uint32_t *out_bytes)
{
    if (out_bytes == NULL) {
        return MRT_ERR_INVAL;
    }
    bool self = (task == NULL);
    struct mrt_tcb *t = self ? mrt_priv_current() : task;
    if (t == NULL) {
        return MRT_ERR_STATE;
    }
    if (!handle_valid(t)) {
        return self ? MRT_ERR_STATE : MRT_ERR_INVAL;
    }
    *out_bytes = mrt_port_stack_unused(t);
    return MRT_OK;
}

/* --- Private accessors for sched.c (see kernel_priv.h) --- */

uint8_t mrt_priv_task_count(void)
{
    return s_count;
}

struct mrt_tcb *mrt_priv_task_at(uint8_t slot)
{
    MRT_ASSERT(slot < s_count);
    return s_table[slot];
}

void mrt_priv_task_reset_table(void)
{
    /* Test hook: drops every task. Port states ARE freed (else the host's
     * context pool would leak across cases); the TCB structs belong to the
     * caller, so forgetting the pointers is their cleanup. */
    for (uint8_t i = 0u; i < s_count; i++) {
        mrt_port_task_delete(s_table[i]);
        s_table[i] = NULL;
    }
    s_count = 0u;
    s_zombies = false;
}

void mrt_priv_reap_deleted(void)
{
    if (!s_zombies) {
        return; /* The common case: nothing died, no scan. */
    }
    mrt_port_enter_critical();
    /* Rescan-friendly loop: each reap compacts a new entry into index i,
     * so i only advances past survivors. */
    for (uint8_t i = 0u; i < s_count;) {
        if (s_table[i]->state == MRT_TASK_DELETED) {
            reap_locked(s_table[i]);
        } else {
            i++;
        }
    }
    s_zombies = false;
    mrt_port_exit_critical();
}
