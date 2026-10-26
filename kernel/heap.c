/*
 * mini_rtos — kernel/heap.c
 *
 * WHAT:  The optional application heap (see heap.h — kernel paths must
 *        never call it; the no_heap_in_kernel gate enforces that).
 * HOW:   Textbook first-fit. Each block carries a header {size, next,
 *        magic}; the idle list stays ADDRESS-ordered so releasing coalesces
 *        with both neighbors in one pass. Split only when the remainder can
 *        hold a real block (else the sliver would be unusable waste); take
 *        the whole block instead. Magic stamps tell USED from IDLE and catch
 *        double releases and garbage pointers at the call, not weeks later.
 *
 * NOTE ON NAMING: the API says "release", not the f-word the libc heap
 * uses, quite deliberately — so grepping the kernel for libc heap calls
 * stays meaningful. Keep it that way: never write that call shape here,
 * not even in comments, or the no_heap_in_kernel gate will fail the build.
 */
#include <stddef.h>
#include <stdint.h>

#include "mini_rtos/heap.h"
#include "mini_rtos/port_api.h"
#include "mini_rtos/types.h"

#include "kernel_priv.h"

/* Payload alignment: 8 covers doubles and pointers on every target this OS
 * aims at (over-aligning on AVR wastes a few bytes per block — documented
 * in heap.h as part of the price of variable sizes). */
#define MRT_HEAP_ALIGN 8u

/* Debugger-visible stamps: read a header in RAM and know its state. */
#define MRT_HEAP_MAGIC_IDLE 0xFEEDBEEFu
#define MRT_HEAP_MAGIC_USED 0xDEADBEEFu

typedef struct mrt_heap_blk {
    uint32_t size;            /* Whole block, header included. */
    struct mrt_heap_blk *next;/* Idle-list link (valid only when idle). */
    uint32_t magic;           /* IDLE or USED stamp. */
} heap_blk_t;

/* Header STRIDE (not bare sizeof): every payload starts one stride past
 * its header, so the stride must be an alignment multiple or payloads land
 * misaligned and release's alignment check rejects them. Bare sizeof is 16
 * on 64-bit hosts (fine) but 10 on AVR and 12 on 32-bit ARM (broken — the
 * Uno selftest caught this as heap fail 9). */
#define MRT_HEAP_HDR_STRIDE \
    (uint32_t)(((uint32_t)sizeof(heap_blk_t) + MRT_HEAP_ALIGN - 1u) & \
               (uint32_t) ~(MRT_HEAP_ALIGN - 1u))

/* Smallest remainder worth splitting off: must fit its own header plus a
 * minimum payload, or the split manufactures unusable slivers. */
#define MRT_HEAP_MIN_BLOCK (uint32_t)(MRT_HEAP_HDR_STRIDE + MRT_HEAP_ALIGN)

static uint32_t align_up(uint32_t n)
{
    return (uint32_t)(((n + MRT_HEAP_ALIGN - 1u) / MRT_HEAP_ALIGN) *
                      MRT_HEAP_ALIGN);
}

mrt_err_t mrt_heap_create_static(uint8_t *mem,
                                 uint32_t mem_bytes,
                                 mrt_heap_t *storage,
                                 mrt_heap_handle_t *out_handle)
{
    if (mem == NULL || storage == NULL || out_handle == NULL) {
        return MRT_ERR_INVAL;
    }
    if (!mrt_priv_is_inited()) {
        return MRT_ERR_STATE;
    }
    /* Align the base UP (carving at most 7 bytes) and refuse arenas too
     * small to be useful — a heap that cannot serve one block is a bug. */
    uintptr_t raw = (uintptr_t)mem;
    uintptr_t base = (raw + MRT_HEAP_ALIGN - 1u) & ~(uintptr_t)7u;
    uint32_t lost = (uint32_t)(base - raw);
    if (mem_bytes < lost + MRT_HEAP_MIN_BLOCK) {
        return MRT_ERR_INVAL;
    }
    heap_blk_t *first = (heap_blk_t *)base;
    first->size = mem_bytes - lost;
    first->next = NULL;
    first->magic = MRT_HEAP_MAGIC_IDLE;

    storage->mem = (uint8_t *)base;
    storage->total = first->size;
    storage->free_head = first;
    *out_handle = storage;
    return MRT_OK;
}

void *mrt_heap_alloc(mrt_heap_handle_t h, uint32_t bytes)
{
    /* No assert: NULL already IS the failure signal (see mrt_pool_alloc). */
    if (h == NULL || bytes == 0u) {
        return NULL;
    }
    uint32_t need = align_up(bytes) + MRT_HEAP_HDR_STRIDE;

    mrt_port_enter_critical();
    heap_blk_t *prev = NULL;
    heap_blk_t *cur = h->free_head;
    while (cur != NULL && cur->size < need) {
        prev = cur;
        cur = cur->next;
    }
    if (cur == NULL) {
        mrt_port_exit_critical();
        return NULL; /* Nothing fits — the documented OOM signal. */
    }
    /* Unlink, splitting only when the tail stays usable. */
    if (cur->size - need >= MRT_HEAP_MIN_BLOCK) {
        heap_blk_t *tail =
            (heap_blk_t *)((uint8_t *)cur + need);
        tail->size = cur->size - need;
        tail->magic = MRT_HEAP_MAGIC_IDLE;
        tail->next = cur->next;
        cur->size = need;
        cur->next = NULL;
        if (prev != NULL) {
            prev->next = tail;
        } else {
            h->free_head = tail;
        }
    } else {
        if (prev != NULL) {
            prev->next = cur->next;
        } else {
            h->free_head = cur->next;
        }
    }
    cur->magic = MRT_HEAP_MAGIC_USED;
    mrt_port_exit_critical();
    return (void *)((uint8_t *)cur + MRT_HEAP_HDR_STRIDE);
}

mrt_err_t mrt_heap_release(mrt_heap_handle_t h, void *ptr)
{
    if (h == NULL) {
        return MRT_ERR_INVAL;
    }
    if (ptr == NULL) {
        return MRT_OK; /* C-compatible no-op. */
    }
    uintptr_t base = (uintptr_t)h->mem;
    uintptr_t addr = (uintptr_t)ptr;
    /* Payload must sit strictly inside the arena (past the first header)
     * and stay aligned — else it never came from here. */
    if (addr < base + MRT_HEAP_HDR_STRIDE || addr >= base + h->total ||
        ((addr - base) % MRT_HEAP_ALIGN) != 0u) {
        return MRT_ERR_INVAL;
    }
    heap_blk_t *blk =
        (heap_blk_t *)((uint8_t *)ptr - MRT_HEAP_HDR_STRIDE);
    if (blk->magic != MRT_HEAP_MAGIC_USED) {
        /* Double release (IDLE stamp) or a forged header — either way, no. */
        return MRT_ERR_INVAL;
    }

    mrt_port_enter_critical();
    blk->magic = MRT_HEAP_MAGIC_IDLE;
    /* Insert address-ordered, then fuse with any touching neighbors. The
     * list stays ordered forever, so one pass suffices on every release. */
    heap_blk_t *prev = NULL;
    heap_blk_t *cur = h->free_head;
    while (cur != NULL && cur < blk) {
        prev = cur;
        cur = cur->next;
    }
    blk->next = cur;
    if (prev != NULL) {
        prev->next = blk;
    } else {
        h->free_head = blk;
    }
    if (cur != NULL && (uint8_t *)blk + blk->size == (uint8_t *)cur) {
        blk->size += cur->size;
        blk->next = cur->next;
    }
    if (prev != NULL && (uint8_t *)prev + prev->size == (uint8_t *)blk) {
        prev->size += blk->size;
        prev->next = blk->next;
    }
    mrt_port_exit_critical();
    return MRT_OK;
}

mrt_err_t mrt_heap_free_bytes(mrt_heap_handle_t h, uint32_t *out_bytes)
{
    if (h == NULL || out_bytes == NULL) {
        return MRT_ERR_INVAL;
    }
    mrt_port_enter_critical();
    /* Payload bytes only (headers excluded): what alloc() calls can claim
     * in total — still an upper bound per call when split across blocks. */
    uint32_t sum = 0u;
    for (heap_blk_t *cur = h->free_head; cur != NULL; cur = cur->next) {
        sum += cur->size - MRT_HEAP_HDR_STRIDE;
    }
    mrt_port_exit_critical();
    *out_bytes = sum;
    return MRT_OK;
}
