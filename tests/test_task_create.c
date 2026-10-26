/*
 * mini_rtos — tests/test_task_create.c
 *
 * WHAT:  Focused tests for mrt_task_create_static: happy path, argument
 *        validation, name handling, boot order, and the table-full edge.
 * WHY:   Creation is the gate to everything else — a task filed wrong (bad
 *        slot, smuggled NULL func, unterminated name) corrupts the
 *        scheduler later, far from the real bug. Pin the contract here so
 *        regressions fail loudly at the source.
 * HOW:   Plain C, no framework: CHECK logs file:line and counts failures,
 *        main returns nonzero when any check failed so CTest reports red.
 */
#include <stdio.h>
#include <string.h>

#include "mini_rtos/config.h"
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

static void dummy_task(void *arg)
{
    (void)arg;
}

/* One spare TCB beyond the table so the NOMEM case has valid storage — the
 * failure must come from the full table, not from a NULL argument. */
static mrt_tcb_t s_tcb[MRT_MAX_TASKS + 1u];

int main(void)
{
    mrt_task_handle_t h = NULL;

    /* Boot order: creating before init must fail, not corrupt. */
    CHECK(mrt_task_create_static("early", dummy_task, NULL, 1u,
                                 &s_tcb[0], &h) == MRT_ERR_STATE);

    CHECK(mrt_kernel_init() == MRT_OK);
    CHECK(mrt_kernel_init() == MRT_ERR_STATE); /* init is exactly-once */

    /* Happy path: every field lands where the header promises. */
    mrt_test_reset();
    h = NULL;
    CHECK(mrt_task_create_static("worker", dummy_task, (void *)0x1234, 3u,
                                 &s_tcb[0], &h) == MRT_OK);
    CHECK(h == &s_tcb[0]);
    CHECK(strcmp(h->name, "worker") == 0);
    CHECK(h->func == dummy_task);
    CHECK(h->arg == (void *)0x1234);
    CHECK(h->prio == 3u);
    CHECK(h->state == MRT_TASK_READY);
    CHECK(h->port_state != NULL);
    CHECK(h->slot == 0u);

    /* Validation: NULLs and out-of-range priority. */
    CHECK(mrt_task_create_static("n", NULL, NULL, 1u,
                                 &s_tcb[1], &h) == MRT_ERR_INVAL);
    CHECK(mrt_task_create_static("n", dummy_task, NULL, 1u,
                                 NULL, &h) == MRT_ERR_INVAL);
    CHECK(mrt_task_create_static("n", dummy_task, NULL, 1u,
                                 &s_tcb[1], NULL) == MRT_ERR_INVAL);
    CHECK(mrt_task_create_static("n", dummy_task, NULL, MRT_MAX_PRIORITY + 1u,
                                 &s_tcb[1], &h) == MRT_ERR_INVAL);

    /* Boundary priority MRT_MAX_PRIORITY itself is legal. */
    CHECK(mrt_task_create_static("maxp", dummy_task, NULL, MRT_MAX_PRIORITY,
                                 &s_tcb[1], &h) == MRT_OK);

    /* Long names truncate but stay NUL-terminated. */
    CHECK(mrt_task_create_static("abcdefghijklmnopqrstuvwxyz", dummy_task, NULL,
                                 1u, &s_tcb[2], &h) == MRT_OK);
    CHECK(strlen(h->name) == MRT_MAX_TASK_NAME_LEN - 1u);
    CHECK(h->name[MRT_MAX_TASK_NAME_LEN - 1u] == '\0');

    /* NULL name means anonymous, not a crash. */
    CHECK(mrt_task_create_static(NULL, dummy_task, NULL, 1u,
                                 &s_tcb[3], &h) == MRT_OK);
    CHECK(h->name[0] == '\0');

    /* Table-full: exactly MAX tasks fit, the next one fails cleanly. */
    mrt_test_reset();
    for (uint8_t i = 0u; i < MRT_MAX_TASKS; i++) {
        CHECK(mrt_task_create_static("filler", dummy_task, NULL, 1u,
                                     &s_tcb[i], &h) == MRT_OK);
    }
    CHECK(mrt_task_create_static("overflow", dummy_task, NULL, 1u,
                                 &s_tcb[MRT_MAX_TASKS], &h) == MRT_ERR_NOMEM);

    if (s_failures == 0) {
        printf("PASS test_task_create\n");
    }
    return s_failures != 0;
}
