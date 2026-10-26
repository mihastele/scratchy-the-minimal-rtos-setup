/*
 * mini_rtos — queue.h
 *
 * WHAT:  Bounded FIFO message queues: the workhorse of task communication.
 * WHY:   Tasks must exchange data without racing. A queue serializes that:
 *        senders and receivers never touch the buffer concurrently, and the
 *        queue — not the application — decides who sleeps and who wakes.
 * HOW:   Fixed ring buffer in caller-owned storage. Items are COPIED in and
 *        out (no ownership transfer, no lifetime puzzles); sending a pointer
 *        just copies the pointer — the pointee's lifetime stays yours.
 *        Timeout rules shared by all IPC: 0 = try once, N = wait up to N
 *        ticks, MRT_WAIT_FOREVER = wait without a deadline. A wait that
 *        finds no room/item in time returns MRT_ERR_TIMEOUT.
 */
#ifndef MINI_RTOS_QUEUE_H
#define MINI_RTOS_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "mini_rtos/types.h"

/* Transparent like the TCB: every field inspectable in a debugger. Written
 * only by queue.c, always under the port critical section. */
typedef struct mrt_queue {
    uint8_t *buf;      /* Item storage: capacity * item_size bytes. */
    uint16_t item_size;/* Bytes per item (> 0). */
    uint16_t capacity; /* Max items (> 0). */
    uint16_t count;    /* Items currently held. */
    uint16_t head;     /* Dequeue index. */
    uint16_t tail;     /* Enqueue index. */
} mrt_queue_t;

typedef struct mrt_queue *mrt_queue_handle_t;

/*
 * Wire a queue to caller-owned storage. `buf` must hold at least
 * capacity * item_size bytes (checked — undersized is INVAL, not a quiet
 * overflow). Both `buf` and `queue_storage` must outlive the queue.
 *
 * Returns OK, INVAL (bad sizes, short buffer, NULLs), or STATE (not inited).
 */
mrt_err_t mrt_queue_create_static(uint16_t capacity,
                                  uint16_t item_size,
                                  uint8_t *buf,
                                  uint32_t buf_bytes,
                                  mrt_queue_t *queue_storage,
                                  mrt_queue_handle_t *out_handle);

/*
 * Copy one item in, waiting up to `timeout` ticks for room. Wakes the
 * best-priority blocked receiver, if any. Never call from an ISR (use
 * send_isr) — returns MRT_ERR_ISR there. Calling with no current task
 * (from main) is MRT_ERR_STATE.
 */
mrt_err_t mrt_queue_send(mrt_queue_handle_t q,
                         const void *item,
                         mrt_tick_t timeout);

/*
 * Copy one item out, waiting up to `timeout` ticks for data. Wakes the
 * best-priority blocked sender, if any. Same ISR/task rules as send.
 */
mrt_err_t mrt_queue_receive(mrt_queue_handle_t q,
                            void *item_out,
                            mrt_tick_t timeout);

/*
 * ISR twins: never block, never sleep; NULL `woke` is tolerated (pass NULL
 * when the caller cannot reschedule anyway). On success *woke reports
 * whether a task was unblocked — the ISR's port uses it to request a
 * context switch before returning to the preempted task. Full (send) or
 * empty (receive) returns MRT_ERR_TIMEOUT. Safe to call from tasks too.
 */
mrt_err_t mrt_queue_send_isr(mrt_queue_handle_t q,
                             const void *item,
                             bool *woke);
mrt_err_t mrt_queue_receive_isr(mrt_queue_handle_t q,
                                void *item_out,
                                bool *woke);

/* Items currently held (NULL handle reads 0). Safe from tasks and ISRs
 * (single read under critical). */
uint16_t mrt_queue_count(mrt_queue_handle_t q);

#endif /* MINI_RTOS_QUEUE_H */
