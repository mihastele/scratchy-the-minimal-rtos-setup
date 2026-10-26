/*
 * mini_rtos — pool.h
 *
 * WHAT:  Fixed-block memory pools: O(1) deterministic allocation, zero
 *        fragmentation, by construction.
 * WHY:   A general heap is the enemy of real-time (unbounded search,
 *        fragmentation over months of uptime). When every object has one
 *        size — message structs, descriptors, nodes — a pool gives it back
 *        in constant time and can NEVER fragment: any free block fits any
 *        request. Need to BLOCK for a block? Pair the pool with a counting
 *        semaphore seeded to the block count (take-then-alloc, free-then-
 *        give) — the standard pattern, composed from parts instead of
 *        baking waits into the allocator.
 * HOW:   The free list threads THROUGH the free blocks themselves (each one
 *        starts with a next-pointer), so the pool needs no side storage —
 *        but block_size must hold a pointer (checked). Callers align `mem`
 *        to max_align_t (e.g. `_Alignas(max_align_t) static uint8_t mem[]`)
 *        and keep block_size a multiple of pointer size; both are validated
 *        because unaligned payloads fault on strict cores (Cortex-M0).
 *        alloc/free never block and never wake, so they are ISR-safe.
 */
#ifndef MINI_RTOS_POOL_H
#define MINI_RTOS_POOL_H

#include <stdint.h>

#include "mini_rtos/types.h"

typedef struct mrt_pool {
    uint8_t *mem;       /* Block storage: num_blocks * block_size bytes. */
    void *free_head;    /* Intrusive free-list head (NULL when empty). */
    uint16_t block_size;/* Bytes per block (>= pointer size, ptr-aligned). */
    uint16_t num_blocks;/* Total blocks (> 0). */
    uint16_t free_count;/* Blocks currently free. */
} mrt_pool_t;

typedef struct mrt_pool *mrt_pool_handle_t;

/*
 * Wire a pool to caller-owned storage. Returns OK, INVAL (bad sizes,
 * under-aligned mem or block_size, short buffer, NULLs), or STATE (boot
 * order — init the kernel first, like every other create).
 */
mrt_err_t mrt_pool_create_static(uint16_t num_blocks,
                                 uint16_t block_size,
                                 uint8_t *mem,
                                 uint32_t mem_bytes,
                                 mrt_pool_t *storage,
                                 mrt_pool_handle_t *out_handle);

/*
 * Take one block (LIFO — hot blocks stay hot in cache), or NULL when the
 * pool is empty. A NULL handle also reads NULL in all builds (no trap —
 * the failure value is testable). ISR-safe.
 */
void *mrt_pool_alloc(mrt_pool_handle_t p);

/*
 * Return a block. INVAL on a foreign pointer (outside the arena or
 * misaligned), a double return, or NULLs — all caught, never silent.
 * ISR-safe.
 */
mrt_err_t mrt_pool_free(mrt_pool_handle_t p, void *block);

/* Blocks currently free (NULL handle reads 0). ISR-safe. */
uint16_t mrt_pool_free_count(mrt_pool_handle_t p);

#endif /* MINI_RTOS_POOL_H */
