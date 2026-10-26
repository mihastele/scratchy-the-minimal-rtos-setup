/*
 * mini_rtos — examples/blinky_avr.c
 *
 * WHAT:  The AVR bring-up proof: two RTOS tasks printing A/B under a REAL
 *        preemptive 1 kHz tick, plus a director task that ends the run.
 * WHY:   The first firmware answers three questions: does the context
 *        switcher preserve tasks (both marks appear)? Does the tick
 *        preempt (marks interleave while printing)? Do delays track wall
 *        time (alternation holds)? Under simavr the UART transcript answers
 *        all three deterministically: it must read exactly "ABABAB!END".
 * HOW:   A/B print 3 times each at 50-tick spacing (same priority, so they
 *        strictly alternate — A keeps a 1-tick phase lead every round, see
 *        the design note in the port review). The director wakes at tick
 *        400, prints '!', and stops the scheduler; main() prints END.
 *        UART here is 20 lines of polled bring-up scaffolding — the real
 *        driver layer arrives in Phase 6.
 */
#include <stddef.h>

#include <avr/io.h>

#include "mini_rtos/kernel.h"
#include "mini_rtos/task.h"
#include "mini_rtos/types.h"

/* --- Polled USART0 scaffolding (bring-up only, Phase 6 replaces it) --- */

static void uart_init(void)
{
    /* 9600 baud from F_CPU: UBRR = F_CPU/16/baud - 1 = 103 @16 MHz. The
     * polled TX loop below is preemption-safe (a tick mid-wait just borrows
     * the CPU; the loop resumes and the byte still goes out whole). */
    UBRR0 = (uint16_t)(F_CPU / 16UL / 9600UL - 1UL);
    UCSR0B = (1u << TXEN0);
    UCSR0C = (1u << UCSZ01) | (1u << UCSZ00); /* 8N1 */
}

static void uart_put(char c)
{
    while ((UCSR0A & (1u << UDRE0)) == 0u) {
        /* Busy-wait for the TX register: the tick ISR preempts this freely
         * on hardware (that IS the preemption proof); on paper it is one
         * idle loop. */
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

/* --- Tasks --- */

static mrt_tcb_t s_tcb_a;
static mrt_tcb_t s_tcb_b;
static mrt_tcb_t s_tcb_director;

static void mark_task(void *arg)
{
    char mark = *(const char *)arg;
    for (int i = 0; i < 3; i++) {
        uart_put(mark);
        mrt_task_delay_ticks(50u);
    }
    /* Returning suspends the task (finished) — see the kernel contract. */
}

static void director_task(void *arg)
{
    (void)arg;
    /* A/B finish around tick ~155 (3 prints x ~51 ticks); 400 leaves a
     * margin no reasonable jitter can eat. */
    mrt_task_delay_ticks(400u);
    uart_put('!');
    mrt_kernel_request_stop();
    for (;;) {
        /* Parked: start() returns after this pass; never picked again. */
        mrt_task_yield();
    }
}

static char s_mark_a = 'A';
static char s_mark_b = 'B';

int main(void)
{
    uart_init();

    if (mrt_kernel_init() != MRT_OK) {
        uart_print("INIT-FAIL\n");
        for (;;) {
        }
    }
    mrt_task_handle_t h = NULL;
    (void)h;
    if (mrt_task_create_static("a", mark_task, &s_mark_a, 1u,
                               &s_tcb_a, &h) != MRT_OK ||
        mrt_task_create_static("b", mark_task, &s_mark_b, 1u,
                               &s_tcb_b, &h) != MRT_OK ||
        mrt_task_create_static("dir", director_task, NULL, 1u,
                               &s_tcb_director, &h) != MRT_OK) {
        uart_print("CREATE-FAIL\n");
        for (;;) {
        }
    }

    if (mrt_kernel_run_ticks(1000u) != MRT_OK) {
        uart_print("RUN-FAIL\n");
        for (;;) {
        }
    }
    /* run_ticks returns after the director's stop request (~tick 400). */
    uart_print("END\n"); /* trailing newline: simavr line-buffers UART */
    for (;;) {
        /* Firmware ends here; simavr's timeout collects the transcript. */
    }
}
