/*
 * mini_rtos — port/avr/port.c
 *
 * WHAT:  The AVR port: ATmega328P (Arduino Uno) and ATmega2560 (Arduino
 *        Mega), sharing one file via __AVR_3_BYTE_PC__ / EIND conditionals.
 * WHY ONE FILE: the two chips differ only in flash size (2- vs 3-byte
 *        return addresses) and two extra pointer registers — everything
 *        else (timers, USART, SREG discipline, sleep) is identical. One
 *        port, verified on both.
 * HOW:   - Stacks come from a static pool (MRT_AVR_TASK_STACK_SIZE each):
 *          caller code uses plain mrt_task_create_static and never thinks
 *          about stacks; the watermark (0xA5 paint) reports the truth.
 *        - Timer1 in CTC mode is the 1 kHz tick (see the OCR math below).
 *        - The asm file owns register moves; this file owns DECISIONS (the
 *          three small helpers called mid-switch) and all setup/teardown.
 *        - SRAM BUDGET (Uno: 2048 bytes!): the pool is (MAX_TASKS+1) x
 *          STACK_SIZE. Uno builds use MAX_TASKS=4 x 192 = 960 bytes; Mega
 *          builds fit the 8 x 256 defaults. See cmake board presets.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sleep.h>

#include "mini_rtos/config.h"
#include "mini_rtos/kernel.h"
#include "mini_rtos/port_api.h"
#include "mini_rtos/task.h"

#include "kernel_priv.h"

/* Stack paint: same 0xA5 contract as the host port (see its comment). */
#define MRT_STACK_PAINT 0xA5u

/*
 * One task's machine state: just a stack pointer plus the stack itself.
 * Everything else a task needs (regs, SREG) lives ON that stack, pushed by
 * SAVE_ALL — which is why switching is "save SP here, load SP there".
 */
typedef struct avr_task_state {
    uint8_t *sp;                              /* Top of the saved frame. */
    uint8_t stack[MRT_AVR_TASK_STACK_SIZE];   /* Grows down from the end. */
} avr_task_state_t;

/* Context pool: pointer-keyed like the host's (slots shift on delete, so
 * nothing here is slot-indexed). +1 for the kernel idle task. */
static struct {
    avr_task_state_t st;
    bool used;
} s_pool[MRT_MAX_TASKS + 1u];

/*
 * Switch scratch, shared with port_asm.S (non-static ON PURPOSE — the asm
 * references these symbols directly):
 * - mrt_avr_next: switch_to's TCB argument, stashed before SAVE_ALL.
 * - mrt_avr_tmp_sp: the live SP captured mid-dance; helpers rewrite it to
 *   steer the restore at a new stack. Only ever touched with I=0, so no
 *   nested ISR can clobber it (see the atomicity note in port_asm.S).
 * - mrt_avr_sched_sp: the scheduler's (main stack) resting SP.
 */
struct mrt_tcb *mrt_avr_next;
uint16_t mrt_avr_tmp_sp;
uint16_t mrt_avr_sched_sp;
/* 1 while a task owns the CPU, 0 while the scheduler (main stack) does.
 * Maintained by the asm: switch_to sets it AFTER loading the task SP,
 * yield_to_sched clears it BEFORE saving. The tick ISR must NOT decide
 * "landed in a task" from the current-task pointer alone: s_current is
 * already set while switch_to still runs on the main stack (and still
 * set while yield's restore runs there), so cur != NULL with a scheduler
 * SP would save the MAIN SP into the task and resume wild. The flag
 * closes both windows (see port_asm.S for the ordering argument). */
uint8_t mrt_avr_in_task;

/* Critical-section depth + the SREG image of the outermost entry. */
static uint8_t s_crit_nesting;
static uint8_t s_crit_sreg;

/* ISR nesting for mrt_port_in_isr (0/1 in practice — AVR never nests). */
static unsigned s_in_isr;

static avr_task_state_t *avr_state(struct mrt_tcb *tcb)
{
    return (avr_task_state_t *)tcb->port_state;
}

/*
 * First-run frame crafter. Lays out, top-down from the stack end, exactly
 * what RESTORE_ALL + ret consumes: return address (2 bytes, 3 on mega),
 * r0, SREG (I=1: tasks start enabled), r1, r2-r31 with the TCB pointer in
 * the r24:r25 argument slot, and EIND/RAMPZ zeroes on big parts. The
 * matching pop order lives in port_asm.S — change one, change both.
 */
static void craft_first_frame(avr_task_state_t *st, struct mrt_tcb *tcb)
{
    /* Entry point: the function's 16-bit C address splits directly onto
     * the stack (this is the standard avr-libc convention — see also
     * FreeRTOS' AVR port). The kernel always lives below the 64 KB line
     * (it is a few KB), so the mega's third byte is 0. */
    extern void avr_trampoline(struct mrt_tcb *tcb);
    uint16_t entry = (uint16_t)(uintptr_t)avr_trampoline;
    uint16_t tcb_addr = (uint16_t)(uintptr_t)tcb;

    uint8_t *sp = &st->stack[MRT_AVR_TASK_STACK_SIZE];
    /* Return-address byte order: `call` pushes PCl FIRST (it lands at the
     * higher address) and PCh second (lower, popped first by `ret`); on
     * 3-byte-PC parts the EIND byte goes last and sits lowest of the three.
     * avr-libc's setjmp pops in exactly this order (EIND, PCh, PCl), and
     * simavr's pop_addr treats the lowest byte as the high bits — match it.
     * Get this backwards and the first `ret` lands byte-swapped in the
     * weeds and the MCU looks like it reboots in a tight loop (the exact
     * symptom this ordering bug produced under simavr). */
    *--sp = (uint8_t)(entry & 0xFFu); /* PCl: highest of the group */
    *--sp = (uint8_t)(entry >> 8);    /* PCh: below PCl */
#ifdef __AVR_3_BYTE_PC__
    *--sp = 0u; /* EIND image: lowest (near address, low flash) */
#endif
    *--sp = 0u;     /* r0 */
    *--sp = 0x80u;  /* SREG image: I-bit set, flags clear */
    *--sp = 0u;     /* r1 (ABI zero) */
    /* Registers r2..r31 in ASCENDING fill order: descending writes land
     * r31 lowest, so RESTORE_ALL's r31-first pops line up. (Writing this
     * loop 31-down-to-2 mirrors the block and delivers the TCB to the
     * wrong registers — the first task then jumps wild. Yes, this exact
     * bug happened once; the direction is load-bearing.) */
    for (int r = 2; r <= 31; r++) {
        uint8_t v = 0u;
        if (r == 24) {
            v = (uint8_t)(tcb_addr & 0xFFu);
        } else if (r == 25) {
            v = (uint8_t)(tcb_addr >> 8);
        }
        *--sp = v;
    }
#ifdef EIND
    *--sp = 0u; /* EIND */
#endif
#ifdef RAMPZ
    *--sp = 0u; /* RAMPZ */
#endif
    /* AVR stacks are EMPTY-descending (Instruction Set Manual: PUSH
     * post-decrements AFTER storing, POP pre-increments BEFORE loading),
     * so SP always names the FREE slot and the top byte sits at SP+1 —
     * NOT at SP as on ARM (full-descending). Point one below the frame.
     * (This off-by-one crashed first boot: pops landed shifted, `ret`
     * fetched garbage, and the MCU reboot-looped with zero output.) */
    st->sp = sp - 1;
}

/* Every task context starts here (r24:r25 = its TCB, per the craft above).
 * Returning from the task body suspends it — the mirror of the host port. */
void avr_trampoline(struct mrt_tcb *tcb)
{
    tcb->func(tcb->arg);
    tcb->state = MRT_TASK_SUSPENDED;
    mrt_port_yield_to_sched();
    for (;;) {
        /* Unreachable: SUSPENDED tasks are never picked again. */
    }
}

/*
 * Mid-switch C helpers, called from port_asm.S with interrupts dead and
 * the live SP stashed in mrt_avr_tmp_sp. Their own call frames sit BELOW
 * the stashed SP and are abandoned when the asm reloads SP — so they must
 * communicate ONLY through the shared globals, never by returning stack
 * addresses or taking stack pointers.
 */
void mrt_avr_switch_helper(void)
{
    mrt_avr_sched_sp = mrt_avr_tmp_sp;
    mrt_avr_tmp_sp = (uint16_t)(uintptr_t)avr_state(mrt_avr_next)->sp;
}

void mrt_avr_yield_helper(void)
{
    avr_state(mrt_priv_current())->sp = (uint8_t *)(uintptr_t)mrt_avr_tmp_sp;
}

void mrt_avr_tick_helper(void)
{
    s_in_isr++;
    struct mrt_tcb *cur = mrt_priv_current();
    /* Time passes and sleepers wake even when the tick lands in scheduler
     * context (cur == NULL between switches) — only the task swap is
     * skipped there, since there is no task frame to save. */
    (void)mrt_kernel_on_tick_isr();
    /* Switch only mid-task: cur != NULL is ALSO true while the scheduler
     * runs between pick and switch (and during yield's way home), when
     * the live SP is the MAIN stack — saving that as a task SP is the
     * wild-resume bug the in_task flag exists to prevent. */
    if (cur != NULL && mrt_avr_in_task != 0u) {
        avr_state(cur)->sp = (uint8_t *)(uintptr_t)mrt_avr_tmp_sp;
        mrt_priv_pick_and_set_current();
        mrt_avr_tmp_sp =
            (uint16_t)(uintptr_t)avr_state(mrt_priv_current())->sp;
    }
    s_in_isr--;
}

mrt_err_t mrt_port_init(void)
{
    for (unsigned i = 0u; i < MRT_MAX_TASKS + 1u; i++) {
        s_pool[i].used = false;
        s_pool[i].st.sp = NULL;
    }
    s_crit_nesting = 0u;
    s_crit_sreg = 0u;
    s_in_isr = 0u;
    mrt_avr_next = NULL;
    mrt_avr_tmp_sp = 0u;
    mrt_avr_sched_sp = 0u;
    mrt_avr_in_task = 0u;

    /*
     * Timer1: CTC mode, prescaler 64, compare-A match at 1 kHz.
     *   OCR1A = F_CPU / 64 / TICK_HZ - 1 = 16000000/64/1000 - 1 = 249.
     * Timer1 (16-bit) is chosen over Timer0 (8-bit) so the full range fits
     * with room to spare, and because Arduino cores (which we do not use,
     * but coexist with conceptually) own Timer0. The ISR vector itself is
     * the naked handler in port_asm.S.
     */
    _Static_assert(MRT_TICK_HZ == 1000u, "tick math below assumes 1 kHz");
    const uint32_t ocr =
        (uint32_t)F_CPU / 64u / (uint32_t)MRT_TICK_HZ - 1u;
    TCCR1A = 0u;
    TCCR1B = (1u << WGM12); /* CTC: TOP = OCR1A */
    OCR1A = (uint16_t)ocr;
    TCNT1 = 0u;
    TIFR1 = (1u << OCF1A);  /* clear any stale match flag */
    TIMSK1 = (1u << OCIE1A);
    TCCR1B |= (1u << CS11) | (1u << CS10); /* prescaler 64: GO */

    /* The scheduler runs with interrupts on from here; tasks inherit it. */
    sei();
    return MRT_OK;
}

mrt_err_t mrt_port_task_init(struct mrt_tcb *tcb)
{
    if (tcb == NULL || tcb->slot > MRT_IDLE_SLOT) {
        return MRT_ERR_INVAL;
    }
    unsigned pidx = MRT_MAX_TASKS + 1u;
    for (unsigned i = 0u; i < MRT_MAX_TASKS + 1u; i++) {
        if (!s_pool[i].used) {
            pidx = i;
            break;
        }
    }
    if (pidx > MRT_MAX_TASKS) {
        return MRT_ERR_NOMEM;
    }

    /* Full rebuild on every claim (delete/reap cycles reuse entries), paint
     * before crafting so the watermark starts at 100% unused. */
    avr_task_state_t *st = &s_pool[pidx].st;
    for (uint32_t i = 0u; i < (uint32_t)sizeof(st->stack); i++) {
        st->stack[i] = MRT_STACK_PAINT;
    }
    craft_first_frame(st, tcb);
    s_pool[pidx].used = true;
    tcb->port_state = st;
    return MRT_OK;
}

void mrt_port_task_delete(struct mrt_tcb *tcb)
{
    MRT_ASSERT(tcb != NULL);
    if (tcb == NULL || tcb->port_state == NULL) {
        return;
    }
    for (unsigned i = 0u; i < MRT_MAX_TASKS + 1u; i++) {
        if (s_pool[i].used && &s_pool[i].st == tcb->port_state) {
            s_pool[i].used = false;
            tcb->port_state = NULL;
            return;
        }
    }
    MRT_ASSERT(false); /* Unreachable: every live task owns an entry. */
}

uint32_t mrt_port_stack_unused(const struct mrt_tcb *tcb)
{
    MRT_ASSERT(tcb != NULL);
    if (tcb == NULL || tcb->port_state == NULL) {
        return 0u;
    }
    /* AVR stacks grow down like everyone else's: count surviving paint
     * upward from the base. */
    const avr_task_state_t *st = (const avr_task_state_t *)tcb->port_state;
    uint32_t n = 0u;
    while (n < (uint32_t)sizeof(st->stack) &&
           st->stack[n] == MRT_STACK_PAINT) {
        n++;
    }
    return n;
}

void mrt_port_enter_critical(void)
{
    /* Classic SREG discipline: capture-then-cli is safe (an interrupt
     * landing between the two simply runs and returns; the captured image
     * stays correct), and only the outermost exit restores — nested
     * sections cannot re-enable early. */
    uint8_t sreg = SREG;
    cli();
    if (s_crit_nesting == 0u) {
        s_crit_sreg = sreg;
    }
    s_crit_nesting++;
}

void mrt_port_exit_critical(void)
{
    MRT_ASSERT(s_crit_nesting > 0u);
    if (s_crit_nesting == 0u) {
        return;
    }
    s_crit_nesting--;
    if (s_crit_nesting == 0u) {
        SREG = s_crit_sreg;
    }
}

bool mrt_port_in_isr(void)
{
    return s_in_isr > 0u;
}

void mrt_port_idle(void)
{
    /* Sleep until the next interrupt (the 1 ms tick, normally). The
     * enable/sei/cpu/disable order is avr-libc's canonical race-free nap:
     * even if the tick fires between enable and cpu, time was already
     * counted by the ISR — the sleep just lasts one tick. */
    set_sleep_mode(SLEEP_MODE_IDLE);
    sleep_enable();
    sei();
    sleep_cpu();
    sleep_disable();
}

void mrt_port_end_of_pass(void)
{
    /* Hardware ticks: the timer ISR drives time, so the pass boundary does
     * nothing. (The host port advances its virtual timer here instead.) */
}

void mrt_port_panic(const char *file, int line)
{
    /* No stdout on bare metal: freeze with interrupts dead. A product
     * would blink file:line in morse on an LED or reset into a bootloader
     * fault log; the simavr harness reports the freeze as a timeout. */
    (void)file;
    (void)line;
    cli();
    for (;;) {
    }
}
