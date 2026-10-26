/*
 * mini_rtos — tests/test_event.c
 *
 * WHAT:  Event flags: ANY/ALL waits, CLEAR_ON_EXIT, broadcast wake with
 *        snapshots, timeouts with untouched out-bits, validation, and the
 *        set_isr woke flag.
 * WHY:   Events are the broadcast primitive — one set must wake EVERY
 *        matching waiter with the bits that were true at wake time, and
 *        clearers must apply in deterministic pick order. The snapshot test
 *        (observed == 0x03 while the final read is 0x00) is the proof that
 *        observation and clearing are correctly separated.
 * HOW:   Scripted setter/waiter pairs with exact logs, plus direct main()
 *        calls for validation and ISR-flag behavior.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mini_rtos/event.h"
#include "mini_rtos/kernel.h"
#include "mini_rtos/task.h"
#include "mini_rtos/types.h"
#include "test_hooks.h"

static int s_failures;

#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                         \
        }                                                         \
    } while (0)

static char s_log[64];
static size_t s_len;

static void log_reset(void)
{
    s_len = 0u;
    s_log[0] = '\0';
}

static void emit(char c)
{
    if (s_len + 1u < sizeof(s_log)) {
        s_log[s_len++] = c;
        s_log[s_len] = '\0';
    }
}

static void check_log(const char *want)
{
    if (strcmp(s_log, want) != 0) {
        printf("FAIL %s:%d: log \"%s\", want \"%s\"\n",
               __FILE__, __LINE__, s_log, want);
        s_failures++;
    }
}

static mrt_tcb_t s_tcb_a, s_tcb_b, s_tcb_c;
static mrt_event_t s_event;
static uint32_t s_observed;

typedef struct {
    mrt_event_handle_t e;
    char mark;
} waiter_cfg_t;

/* Waits ANY of 0x03 FOREVER, snapshots, marks, parks. */
static void any_waiter(void *arg)
{
    waiter_cfg_t *cfg = (waiter_cfg_t *)arg;
    uint32_t obs = 0u;
    if (mrt_event_wait(cfg->e, 0x03u, MRT_EVENT_WAIT_ANY,
                       MRT_WAIT_FOREVER, &obs) == MRT_OK) {
        s_observed = obs;
        emit(cfg->mark);
    }
    mrt_task_delay_ticks(100u);
}

/* Waits ALL of 0x03 with CLEAR_ON_EXIT, snapshots, marks, parks. */
static void all_clear_waiter(void *arg)
{
    waiter_cfg_t *cfg = (waiter_cfg_t *)arg;
    uint32_t obs = 0u;
    if (mrt_event_wait(cfg->e, 0x03u,
                       MRT_EVENT_WAIT_ALL | MRT_EVENT_CLEAR_ON_EXIT,
                       MRT_WAIT_FOREVER, &obs) == MRT_OK) {
        s_observed = obs;
        emit(cfg->mark);
    }
    mrt_task_delay_ticks(100u);
}

/* ANY of 0x01 (broadcast member). */
static void bit0_waiter(void *arg)
{
    waiter_cfg_t *cfg = (waiter_cfg_t *)arg;
    mrt_event_wait(cfg->e, 0x01u, MRT_EVENT_WAIT_ANY, MRT_WAIT_FOREVER, NULL);
    emit(cfg->mark);
    mrt_task_delay_ticks(100u);
}

/* Delays 2, sets 0x01, parks. */
static void set_one_task(void *arg)
{
    mrt_event_handle_t e = (mrt_event_handle_t)arg;
    mrt_task_delay_ticks(2u);
    mrt_event_set(e, 0x01u);
    mrt_task_delay_ticks(100u);
}

/* Sets 0x01 and 0x02 one tick apart, then parks. */
static void set_split_task(void *arg)
{
    mrt_event_handle_t e = (mrt_event_handle_t)arg;
    mrt_task_delay_ticks(1u);
    mrt_event_set(e, 0x01u);
    mrt_task_delay_ticks(1u);
    mrt_event_set(e, 0x02u);
    mrt_task_delay_ticks(100u);
}

static mrt_err_t s_recorded;
static uint32_t s_timeout_out;

/* Waits on bits that never come, recording verdict and out-bits. */
static void timeout_waiter(void *arg)
{
    mrt_event_handle_t e = (mrt_event_handle_t)arg;
    uint32_t out = 0xAAu; /* sentinel: must survive untouched */
    s_recorded = mrt_event_wait(e, 0xFFu, MRT_EVENT_WAIT_ANY, 2u, &out);
    s_timeout_out = out;
    emit(s_recorded == MRT_ERR_TIMEOUT ? 'T' : '?');
    mrt_task_delay_ticks(100u);
}

static waiter_cfg_t s_cfg_a, s_cfg_b;

int main(void)
{
    mrt_event_handle_t h = NULL;

    CHECK(mrt_event_create_static(&s_event, &h) == MRT_ERR_STATE);
    CHECK(mrt_kernel_init() == MRT_OK);

    /* Validation — all answerable from main(), no task needed. */
    CHECK(mrt_event_create_static(NULL, &h) == MRT_ERR_INVAL);
    CHECK(mrt_event_create_static(&s_event, NULL) == MRT_ERR_INVAL);
    CHECK(mrt_event_create_static(&s_event, &h) == MRT_OK);
    CHECK(mrt_event_wait(NULL, 0x01u, MRT_EVENT_WAIT_ANY, 0u, NULL)
          == MRT_ERR_INVAL);
    CHECK(mrt_event_wait(h, 0u, MRT_EVENT_WAIT_ANY, 0u, NULL)
          == MRT_ERR_INVAL);
    CHECK(mrt_event_wait(h, 0x01u, 0u, 0u, NULL) == MRT_ERR_INVAL);
    CHECK(mrt_event_wait(h, 0x01u,
                         MRT_EVENT_WAIT_ANY | MRT_EVENT_WAIT_ALL, 0u, NULL)
          == MRT_ERR_INVAL);
    CHECK(mrt_event_wait(h, 0x01u, 0x08u, 0u, NULL) == MRT_ERR_INVAL);
    CHECK(mrt_event_set(NULL, 0x01u) == MRT_ERR_INVAL);
    CHECK(mrt_event_clear(NULL, 0x01u) == MRT_ERR_INVAL);
    CHECK(mrt_event_get(NULL, &s_observed) == MRT_ERR_INVAL);
    CHECK(mrt_event_get(h, NULL) == MRT_ERR_INVAL);

    /* ANY wait: set 0x01 satisfies ANY-of-0x03; snapshot reads 0x01. */
    mrt_test_reset();
    log_reset();
    s_observed = 0u;
    CHECK(mrt_event_create_static(&s_event, &h) == MRT_OK);
    mrt_task_handle_t hw = NULL;
    mrt_task_handle_t hs = NULL;
    s_cfg_a.e = h;
    s_cfg_a.mark = 'W';
    CHECK(mrt_task_create_static("w", any_waiter, &s_cfg_a, 1u,
                                 &s_tcb_a, &hw) == MRT_OK);
    CHECK(mrt_task_create_static("s", set_one_task, h, 1u,
                                 &s_tcb_b, &hs) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(5u) == MRT_OK);
    check_log("W");
    CHECK(s_observed == 0x01u);

    /* ALL + CLEAR: 0x01 alone does not wake; 0x02 completes 0x03, the
     * waiter snapshots 0x03, and its exit-clear drains the group to 0. */
    mrt_test_reset();
    log_reset();
    s_observed = 0u;
    CHECK(mrt_event_create_static(&s_event, &h) == MRT_OK);
    s_cfg_a.e = h;
    s_cfg_a.mark = 'W';
    CHECK(mrt_task_create_static("w", all_clear_waiter, &s_cfg_a, 1u,
                                 &s_tcb_a, &hw) == MRT_OK);
    CHECK(mrt_task_create_static("s", set_split_task, h, 1u,
                                 &s_tcb_b, &hs) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(6u) == MRT_OK);
    check_log("W");
    CHECK(s_observed == 0x03u);
    {
        uint32_t now = 0xFFu;
        CHECK(mrt_event_get(h, &now) == MRT_OK);
        CHECK(now == 0u);
    }

    /* Broadcast: one set wakes both ANY waiters, in rotation order. */
    mrt_test_reset();
    log_reset();
    CHECK(mrt_event_create_static(&s_event, &h) == MRT_OK);
    mrt_task_handle_t h1 = NULL;
    mrt_task_handle_t h2 = NULL;
    s_cfg_a.e = h;
    s_cfg_a.mark = '1';
    s_cfg_b.e = h;
    s_cfg_b.mark = '2';
    CHECK(mrt_task_create_static("w1", bit0_waiter, &s_cfg_a, 1u,
                                 &s_tcb_a, &h1) == MRT_OK);
    CHECK(mrt_task_create_static("w2", bit0_waiter, &s_cfg_b, 1u,
                                 &s_tcb_b, &h2) == MRT_OK);
    CHECK(mrt_task_create_static("s", set_one_task, h, 1u,
                                 &s_tcb_c, &hs) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(7u) == MRT_OK);
    check_log("12");

    /* Timeout: verdict recorded, out-bits sentinel untouched. */
    mrt_test_reset();
    log_reset();
    s_recorded = MRT_OK;
    s_timeout_out = 0u;
    CHECK(mrt_event_create_static(&s_event, &h) == MRT_OK);
    CHECK(mrt_task_create_static("t", timeout_waiter, h, 1u,
                                 &s_tcb_a, &hw) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(4u) == MRT_OK);
    check_log("T");
    CHECK(s_recorded == MRT_ERR_TIMEOUT);
    CHECK(s_timeout_out == 0xAAu);

    /* set_isr woke flag: true when a waiter catches it, false when idle. */
    mrt_test_reset();
    log_reset();
    CHECK(mrt_event_create_static(&s_event, &h) == MRT_OK);
    s_cfg_a.e = h;
    s_cfg_a.mark = 'W';
    CHECK(mrt_task_create_static("w", any_waiter, &s_cfg_a, 1u,
                                 &s_tcb_a, &hw) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK); /* W blocks on ANY 0x03. */
    {
        bool woke = false;
        CHECK(mrt_event_set_isr(h, 0x01u, &woke) == MRT_OK);
        CHECK(woke == true);
    }
    CHECK(mrt_kernel_run_ticks(1u) == MRT_OK); /* W emits, parks. */
    {
        bool woke = true;
        CHECK(mrt_event_set_isr(h, 0x02u, &woke) == MRT_OK);
        CHECK(woke == false);
        uint32_t now = 0u;
        CHECK(mrt_event_get(h, &now) == MRT_OK);
        CHECK(now == 0x03u);
    }
    check_log("W");

    if (s_failures == 0) {
        printf("PASS test_event\n");
    }
    return s_failures != 0;
}
