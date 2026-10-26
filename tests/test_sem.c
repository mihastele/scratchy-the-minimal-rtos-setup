/*
 * mini_rtos — tests/test_sem.c
 *
 * WHAT:  Semaphore behavior: counting, the binary pattern, give overflow,
 *        blocking-take handoff, timeouts.
 * WHY:   The handoff proof is the point: give() to a waiter must NOT bump
 *        the count (the token belongs to the woken task already). The count
 *        assertions below would catch any detour through the counter.
 * HOW:   Direct _isr calls from main for mechanics, one scripted pair for
 *        the handoff, one task for the timeout.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mini_rtos/kernel.h"
#include "mini_rtos/sem.h"
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

static mrt_tcb_t s_tcb_a, s_tcb_b;
static mrt_sem_t s_sem;

static char s_log[16];
static size_t s_len;

static void log_reset(void)
{
    s_len = 0u;
    s_log[0] = '\0';
}

static void check_log(const char *want)
{
    if (strcmp(s_log, want) != 0) {
        printf("FAIL %s:%d: log \"%s\", want \"%s\"\n",
               __FILE__, __LINE__, s_log, want);
        s_failures++;
    }
}

static void take_emit_task(void *arg)
{
    mrt_sem_handle_t s = (mrt_sem_handle_t)arg;
    if (mrt_sem_take(s, MRT_WAIT_FOREVER) == MRT_OK) {
        if (s_len + 1u < sizeof(s_log)) {
            s_log[s_len++] = 'T';
            s_log[s_len] = '\0';
        }
    }
    mrt_task_delay_ticks(100u);
}

static void delayed_give_task(void *arg)
{
    mrt_sem_handle_t s = (mrt_sem_handle_t)arg;
    mrt_task_delay_ticks(2u);
    mrt_sem_give(s);
    mrt_task_delay_ticks(100u);
}

static mrt_err_t s_recorded;

static void timeout_take_task(void *arg)
{
    mrt_sem_handle_t s = (mrt_sem_handle_t)arg;
    s_recorded = mrt_sem_take(s, 2u);
    mrt_task_delay_ticks(100u);
}

int main(void)
{
    mrt_sem_handle_t h = NULL;

    CHECK(mrt_sem_create_static(2u, 0u, &s_sem, &h) == MRT_ERR_STATE);
    CHECK(mrt_kernel_init() == MRT_OK);

    /* Validation. */
    CHECK(mrt_sem_create_static(0u, 0u, &s_sem, &h) == MRT_ERR_INVAL);
    CHECK(mrt_sem_create_static(2u, 3u, &s_sem, &h) == MRT_ERR_INVAL);
    CHECK(mrt_sem_create_static(2u, 0u, NULL, &h) == MRT_ERR_INVAL);
    CHECK(mrt_sem_create_static(2u, 0u, &s_sem, NULL) == MRT_ERR_INVAL);

    /* Counting mechanics: max 3, init 2 — drain, refuse, refill, refuse. */
    CHECK(mrt_sem_create_static(3u, 2u, &s_sem, &h) == MRT_OK);
    CHECK(mrt_sem_count(h) == 2u);
    CHECK(mrt_sem_take_isr(h) == MRT_OK);
    CHECK(mrt_sem_take_isr(h) == MRT_OK);
    CHECK(mrt_sem_count(h) == 0u);
    CHECK(mrt_sem_take_isr(h) == MRT_ERR_TIMEOUT);
    CHECK(mrt_sem_give_isr(h, NULL) == MRT_OK);
    CHECK(mrt_sem_count(h) == 1u);
    CHECK(mrt_sem_give_isr(h, NULL) == MRT_OK);
    CHECK(mrt_sem_give_isr(h, NULL) == MRT_OK);
    CHECK(mrt_sem_count(h) == 3u);
    CHECK(mrt_sem_give_isr(h, NULL) == MRT_ERR_STATE); /* past max */
    CHECK(mrt_sem_count(h) == 3u);

    /* Binary pattern: max 1, init 0 — signal, consume, empty. */
    CHECK(mrt_sem_create_static(1u, 0u, &s_sem, &h) == MRT_OK);
    CHECK(mrt_sem_take_isr(h) == MRT_ERR_TIMEOUT);
    CHECK(mrt_sem_give_isr(h, NULL) == MRT_OK);
    CHECK(mrt_sem_take_isr(h) == MRT_OK);
    CHECK(mrt_sem_take_isr(h) == MRT_ERR_TIMEOUT);

    /* Blocking handoff: T waits, G gives at tick 3 — T runs at 4 with the
     * token, and the count NEVER rises (direct handoff, not bank+take). */
    mrt_test_reset();
    log_reset();
    CHECK(mrt_sem_create_static(2u, 0u, &s_sem, &h) == MRT_OK);
    mrt_task_handle_t ht = NULL;
    mrt_task_handle_t hg = NULL;
    CHECK(mrt_task_create_static("t", take_emit_task, h, 1u,
                                 &s_tcb_a, &ht) == MRT_OK);
    CHECK(mrt_task_create_static("g", delayed_give_task, h, 1u,
                                 &s_tcb_b, &hg) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(5u) == MRT_OK);
    check_log("T");
    CHECK(mrt_sem_count(h) == 0u);

    /* Timeout on an empty semaphore reports exactly. */
    mrt_test_reset();
    s_recorded = MRT_OK;
    CHECK(mrt_sem_create_static(1u, 0u, &s_sem, &h) == MRT_OK);
    CHECK(mrt_task_create_static("t", timeout_take_task, h, 1u,
                                 &s_tcb_a, &ht) == MRT_OK);
    CHECK(mrt_kernel_run_ticks(4u) == MRT_OK);
    CHECK(s_recorded == MRT_ERR_TIMEOUT);

    if (s_failures == 0) {
        printf("PASS test_sem\n");
    }
    return s_failures != 0;
}
