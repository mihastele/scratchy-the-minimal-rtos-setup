/*
 * mini_rtos — heap.h
 *
 * WHAT:  A tiny opt-in first-fit heap over caller-owned storage — for
 *        APPLICATIONS ONLY. The kernel never touches it (enforced by the
 *        no_heap_in_kernel CTest gate), because kernel paths must never
 *        depend on fragmentation luck.
 * WHY IT EXISTS AT ALL: once in a while an application genuinely needs
 *        variable sizes (a CLI line buffer, a JSON blob). Rather than
 *        dragging in libc malloc (nondeterministic, often thread-unsafe on
 *        bare metal), here is 100 lines you can read, with magic-stamped
 *        headers that catch double releases and foreign pointers.
 * HOW (and the warnings): first-fit with splitting and neighbor coalescing,
 *        8-byte payload alignment, O(n) worst case. WARNINGS: (1) it CAN
 *        fragment — prefer pools for steady-state objects; (2) alloc time
 *        is unbounded in theory — never call it on a hard deadline; (3) it
 *        is thread-safe (critical-guarded) but ISR use is legal yet
 *        discouraged (nondeterministic latency inside an interrupt).
 */
#ifndef MINI_RTOS_HEAP_H
#define MINI_RTOS_HEAP_H

#include <stdint.h>

#include "mini_rtos/types.h"

/* Opaque header type; layout is heap.c's business. */
struct mrt_heap_blk;

typedef struct mrt_heap {
    uint8_t *mem;                 /* Aligned arena base. */
    uint32_t total;               /* Managed bytes. */
    struct mrt_heap_blk *free_head; /* Address-ordered idle list. */
} mrt_heap_t;

typedef struct mrt_heap *mrt_heap_handle_t;

/* Carve a heap out of `mem`. Too-small arenas and NULLs are INVAL. */
mrt_err_t mrt_heap_create_static(uint8_t *mem,
                                 uint32_t mem_bytes,
                                 mrt_heap_t *storage,
                                 mrt_heap_handle_t *out_handle);

/*
 * Allocate `bytes` of 8-aligned payload, or NULL when nothing fits (bytes
 * == 0 also returns NULL — a zero request is almost always a bug, and
 * failing loudly beats a mystery pointer). A NULL heap reads NULL too.
 */
void *mrt_heap_alloc(mrt_heap_handle_t h, uint32_t bytes);

/*
 * Release a block. NULL is a C-compatible no-op (OK); foreign pointers,
 * misaligned pointers, and double releases are INVAL (magic-checked).
 */
mrt_err_t mrt_heap_release(mrt_heap_handle_t h, void *ptr);

/*
 * Free payload bytes across all idle blocks — an UPPER BOUND on what one
 * alloc can take, since free space may be split. NULLs are INVAL.
 */
mrt_err_t mrt_heap_free_bytes(mrt_heap_handle_t h, uint32_t *out_bytes);

#endif /* MINI_RTOS_HEAP_H */
