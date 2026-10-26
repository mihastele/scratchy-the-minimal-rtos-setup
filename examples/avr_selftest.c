/*
 * mini_rtos — examples/avr_selftest.c
 *
 * WHAT:  The AVR proof suite in one firmware: API smoke plus a preemption
 *        check that deadlocks without a working tick.
 * WHY:   Blinky proves tasks run; this proves the KERNEL works under real
 *        preemption. The transcript must read exactly "KLHl!END":
 *          K — single-task API smoke passed (queue/sem/mutex/event/pool/heap)
 *          LHl — the preemption proof (see low_task: no preemption, no H)
 *          !END — the director stopped the run cleanly
 *        Anything else — a FAIL code, a missing mark, a timeout — is red.
 * HOW:   All checks are timing-INsensitive by construction (return codes and
 *        a flag-spin with unbounded margin), so the transcript is stable
 *        across compiler versions and both MCUs. Buffers are static: task
 *        stacks stay shallow enough for 192-byte Uno pools.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <avr/io.h>

#include "mini_rtos/event.h"
#include "mini_rtos/heap.h"
#include "mini_rtos/kernel.h"
#include "mini_rtos/mutex.h"
#include "mini_rtos/pool.h"
#include "mini_rtos/queue.h"
#include "mini_rtos/sem.h"
#include "mini_rtos/task.h"
#include "mini_rtos/types.h"

/* --- Polled USART0 scaffolding (shared shape with blinky_avr.c) --- */

static void uart_init(void)
{
    UBRR0 = (uint16_t)(F_CPU / 16UL / 9600UL - 1UL);
    UCSR0B = (1u << TXEN0);
    UCSR0C = (1u << UCSZ01) | (1u << UCSZ00);
}

static void uart_put(char c)
{
    while ((UCSR0A & (1u << UDRE0)) == 0u) {
    }
    UDR0 = (uint8_t)c;
}

static void uart_print(const char *s)
{
    while (*s != '\0') {
        uart_put(*s);
        s++;
    }
}

/* --- Static IPC objects and arenas (stacks stay shallow) --- */

static mrt_tcb_t s_tcb_self;
static mrt_tcb_t s_tcb_low;
static mrt_tcb_t s_tcb_high;
static mrt_tcb_t s_tcb_dir;

static mrt_queue_t s_q;
static uint8_t s_qbuf[4];
static mrt_sem_t s_sem;
static mrt_mutex_t s_mutex;
static mrt_event_t s_event;
static mrt_pool_t s_pool;
/* Pointer-aligned: the pool validates arena alignment (strict cores fault
 * on unaligned payloads), so say so instead of hoping the linker agrees. */
static _Alignas(sizeof(void *)) uint8_t s_poolmem[4u * 16u];
static mrt_heap_t s_heap;
static uint8_t s_heapmem[256];

/* Preemption flag: set by high, spun on by low. volatile so the spin
 * really re-reads RAM each lap (single core: no cache to fight). */
static volatile bool s_flag;

/* --- Phase 1: single-task API smoke. Returns 0 when all green. --- */

static int api_smoke(void)
{
    mrt_queue_handle_t hq = NULL;
    if (mrt_queue_create_static(4u, 1u, s_qbuf, sizeof(s_qbuf),
                                &s_q, &hq) != MRT_OK) {
        return 1;
    }
    char a = 'a';
    char b = '?';
    if (mrt_queue_send(hq, &a, 0u) != MRT_OK ||
        mrt_queue_receive(hq, &b, 0u) != MRT_OK || b != 'a' ||
        mrt_queue_count(hq) != 0u) {
        return 2;
    }

    mrt_sem_handle_t hs = NULL;
    if (mrt_sem_create_static(2u, 1u, &s_sem, &hs) != MRT_OK ||
        mrt_sem_take(hs, 0u) != MRT_OK ||
        mrt_sem_take(hs, 0u) != MRT_ERR_TIMEOUT ||
        mrt_sem_give(hs) != MRT_OK || mrt_sem_count(hs) != 1u) {
        return 3;
    }

    mrt_mutex_handle_t hm = NULL;
    if (mrt_mutex_create_static(&s_mutex, &hm) != MRT_OK ||
        mrt_mutex_lock(hm, 0u) != MRT_OK ||
        mrt_mutex_lock(hm, 0u) != MRT_ERR_STATE ||
        mrt_mutex_unlock(hm) != MRT_OK) {
        return 4;
    }

    mrt_event_handle_t he = NULL;
    uint32_t obs = 0u;
    if (mrt_event_create_static(&s_event, &he) != MRT_OK ||
        mrt_event_set(he, 0x03u) != MRT_OK ||
        mrt_event_wait(he, 0x03u, MRT_EVENT_WAIT_ALL, 0u, &obs) != MRT_OK ||
        obs != 0x03u) {
        return 5;
    }

    mrt_pool_handle_t hp = NULL;
    if (mrt_pool_create_static(4u, 16u, s_poolmem, sizeof(s_poolmem),
                               &s_pool, &hp) != MRT_OK) {
        return 6;
    }
    void *blk = mrt_pool_alloc(hp);
    if (blk == NULL || mrt_pool_free(hp, blk) != MRT_OK ||
        mrt_pool_free_count(hp) != 4u) {
        return 7;
    }

    mrt_heap_handle_t hh = NULL;
    if (mrt_heap_create_static(s_heapmem, sizeof(s_heapmem),
                               &s_heap, &hh) != MRT_OK) {
        return 8;
    }
    void *mem = mrt_heap_alloc(hh, 32u);
    if (mem == NULL || mrt_heap_release(hh, mem) != MRT_OK) {
        return 9;
    }

    return 0;
}

static void self_task(void *arg)
{
    (void)arg;
    int fail = api_smoke();
    if (fail == 0) {
        uart_put('K');
    } else {
        uart_put('F');
        uart_put((char)('0' + fail)); /* single digit: codes are 1..9 */
        uart_put('\n'); /* flush for simavr's line-buffered UART */
    }
    mrt_task_delay_ticks(500u); /* park past the director */
    for (;;) {
        mrt_task_yield();
    }
}

/*
 * Phase 2: the preemption proof. low_task spins on s_flag WITHOUT yielding
 * — on a cooperative kernel this deadlocks (high never runs, no 'H', the
 * harness times out). On a preemptive kernel the tick at 5 ms runs high,
 * which sets the flag and the order reads "LHl". No calibration: the spin
 * has unbounded margin by construction.
 */
static void low_task(void *arg)
{
    (void)arg;
    uart_put('L');
    while (!s_flag) {
        /* Spin, deliberately yield-free: preemption is the only exit. */
    }
    uart_put('l');
    mrt_task_delay_ticks(500u);
    for (;;) {
        mrt_task_yield();
    }
}

static void high_task(void *arg)
{
    (void)arg;
    mrt_task_delay_ticks(5u);
    s_flag = true;
    uart_put('H');
    mrt_task_delay_ticks(500u);
    for (;;) {
        mrt_task_yield();
    }
}

static void director_task(void *arg)
{
    (void)arg;
    mrt_task_delay_ticks(300u);
    uart_put('!');
    mrt_kernel_request_stop();
    for (;;) {
        mrt_task_yield();
    }
}

int main(void)
{
    uart_init();

    if (mrt_kernel_init() != MRT_OK) {
        uart_print("INIT-FAIL\n");
        for (;;) {
        }
    }
    mrt_task_handle_t h_self = NULL;
    mrt_task_handle_t h_low = NULL;
    mrt_task_handle_t h_high = NULL;
    mrt_task_handle_t h_dir = NULL;
    /* Creation order sets slot order: self runs the smoke before low can
     * spin (same priority, slot order decides the first round). */
    if (mrt_task_create_static("self", self_task, NULL, 1u,
                               &s_tcb_self, &h_self) != MRT_OK ||
        mrt_task_create_static("low", low_task, NULL, 1u,
                               &s_tcb_low, &h_low) != MRT_OK ||
        mrt_task_create_static("high", high_task, NULL, 2u,
                               &s_tcb_high, &h_high) != MRT_OK ||
        mrt_task_create_static("dir", director_task, NULL, 1u,
                               &s_tcb_dir, &h_dir) != MRT_OK) {
        uart_print("CREATE-FAIL\n");
        for (;;) {
        }
    }

    if (mrt_kernel_run_ticks(1000u) != MRT_OK) {
        uart_print("RUN-FAIL\n");
        for (;;) {
        }
    }
    /* Watermark tripwire: every task must finish with 32+ bytes of stack
     * to spare (a 'W' verdict means size up, not "it worked anyway"). */
    mrt_task_handle_t all[4] = { h_self, h_low, h_high, h_dir };
    for (int i = 0; i < 4; i++) {
        uint32_t u = 0u;
        if (mrt_task_stack_unused(all[i], &u) != MRT_OK || u < 32u) {
            uart_put('W');
            uart_put('\n');
            for (;;) {
            }
        }
    }
    uart_print("END\n"); /* trailing newline: simavr line-buffers UART */
    for (;;) {
    }
}
