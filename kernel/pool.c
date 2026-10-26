/*
 * mini_rtos — kernel/pool.c
 *
 * WHAT:  Fixed-block pools (see pool.h for the contract).
 * HOW:   LIFO free stack threaded intrusively through the idle blocks:
 *        alloc pops the head, return pushes it back. Everything happens
 *        under the port critical section, so alloc/return are safe from any
 *        context. Validation is deliberately paranoid — range, alignment,
 *        double-return scan — because heap corruption that surfaces ten
 *        minutes later is the worst kind of bug; failing AT the bad call is
 *        the kindest thing an allocator can do.
 */
#include <stddef.h>
#include <stdint.h>

#include "mini_rtos/pool.h"
#include "mini_rtos/port_api.h"
#include "mini_rtos/types.h"

#include "kernel_priv.h"

mrt_err_t mrt_pool_create_static(uint16_t num_blocks,
                                 uint16_t block_size,
                                 uint8_t *mem,
                                 uint32_t mem_bytes,
                                 mrt_pool_t *storage,
                                 mrt_pool_handle_t *out_handle)
{
    if (num_blocks == 0u || block_size < sizeof(void *) ||
        mem == NULL || storage == NULL || out_handle == NULL) {
        return MRT_ERR_INVAL;
    }
    if (!mrt_priv_is_inited()) {
        return MRT_ERR_STATE;
    }
    /* Alignment law (see pool.h): both the arena and the stride must be
     * pointer-aligned, or payloads inherit misalignment and strict cores
     * fault on ordinary word loads. */
    if (((uintptr_t)mem % sizeof(void *)) != 0u ||
        (block_size % sizeof(void *)) != 0u) {
        return MRT_ERR_INVAL;
    }
    if (mem_bytes < (uint32_t)num_blocks * (uint32_t)block_size) {
        return MRT_ERR_INVAL;
    }

    /* Thread the idle list: block i points at block i+1, last points NULL.
     * Block 0 is the head, so the first allocations come out in order. */
    for (uint16_t i = 0u; i < num_blocks; i++) {
        uint8_t *blk = &mem[(size_t)i * block_size];
        void *next = (i + 1u < num_blocks)
            ? (void *)&mem[(size_t)(i + 1u) * block_size]
            : NULL;
        *(void **)blk = next;
    }
    storage->mem = mem;
    storage->free_head = mem;
    storage->block_size = block_size;
    storage->num_blocks = num_blocks;
    storage->free_count = num_blocks;
    *out_handle = storage;
    return MRT_OK;
}

void *mrt_pool_alloc(mrt_pool_handle_t p)
{
    /* No assert by design: NULL already IS the failure signal, so debug
     * and release agree and the path stays testable (see test_pool). */
    if (p == NULL) {
        return NULL;
    }
    mrt_port_enter_critical();
    void *blk = p->free_head;
    if (blk != NULL) {
        p->free_head = *(void **)blk;
        p->free_count--;
    }
    mrt_port_exit_critical();
    return blk; /* NULL when empty — the documented OOM signal. */
}

mrt_err_t mrt_pool_free(mrt_pool_handle_t p, void *block)
{
    if (p == NULL || block == NULL) {
        return MRT_ERR_INVAL;
    }
    uintptr_t base = (uintptr_t)p->mem;
    uintptr_t addr = (uintptr_t)block;
    uintptr_t span = (uintptr_t)p->num_blocks * (uintptr_t)p->block_size;
    /* Range AND stride-alignment: a mid-block pointer is as foreign as an
     * off-arena one — both would corrupt the idle list. */
    if (addr < base || addr - base >= span ||
        ((addr - base) % p->block_size) != 0u) {
        return MRT_ERR_INVAL;
    }

    mrt_port_enter_critical();
    /* Double-return scan: bounded by num_blocks+1 steps, so even a
     * corrupted list (a cycle) terminates with INVAL instead of hanging. */
    void *cur = p->free_head;
    for (uint16_t i = 0u; i <= p->num_blocks; i++) {
        if (cur == NULL) {
            break;
        }
        if (cur == block) {
            mrt_port_exit_critical();
            return MRT_ERR_INVAL;
        }
        cur = *(void **)cur;
    }
    *(void **)block = p->free_head;
    p->free_head = block;
    p->free_count++;
    mrt_port_exit_critical();
    return MRT_OK;
}

uint16_t mrt_pool_free_count(mrt_pool_handle_t p)
{
    /* NULL reads 0 in all builds (same rule as alloc above). */
    if (p == NULL) {
        return 0u;
    }
    mrt_port_enter_critical();
    uint16_t n = p->free_count;
    mrt_port_exit_critical();
    return n;
}
