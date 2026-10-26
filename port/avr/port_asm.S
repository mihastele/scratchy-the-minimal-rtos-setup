/*
 * mini_rtos — port/avr/port_asm.S
 *
 * WHAT:  The AVR context switch in assembly: save-all/restore-all, the
 *        scheduler<->task swap pair, and the preemptive tick ISR.
 * WHY ASSEMBLY: switching means moving the stack pointer and every CPU
 *        register — operations C cannot name. Everything else (whom to run,
 *        when time passes) stays in C; this file only moves machine state.
 * HOW:   Three entry points share one skeleton —
 *          1. SAVE_ALL: push r0, the SREG image, r1 (then clear it), r2-r31,
 *             and on big parts EIND/RAMPZ. Interrupts die inside SAVE_ALL
 *             (after the SREG image is captured, so the true I-bit is kept).
 *          2. Stash the live SP, call a tiny C helper (port.c) that decides
 *             the new SP using kernel state the asm cannot read.
 *          3. Load the new SP, RESTORE_ALL (exact reverse), ret/reti.
 *        The C call's own stack frame is simply abandoned when SP reloads —
 *        that is why the helper must not return anything on the stack (it
 *        communicates through the shared SP globals only).
 *
 * ATOMICITY (the subtle part — read twice):
 * - From `cli` to the SREG restore, interrupts are dead, so the dance is
 *   atomic: no nested ISR can clobber the shared SP scratch or observe a
 *   half-saved frame.
 * - Before `cli` (push r0, capture SREG) and after the SREG restore (pop
 *   r0, ret), interrupts may fire — and that is SAFE by stack discipline:
 *   a nested ISR pushes/pops its own frame LIFO and returns exactly where
 *   it interrupted, even if it switches tasks in between (the interrupted
 *   dance simply resumes when its task is picked again).
 *
 * MEGA (3-byte PC) NOTES: on parts with >64 KB flash the CPU pushes/pops a
 * 3-byte program counter on call/ret/reti automatically — the switch code
 * needs no special casing. Only the FIRST-run frame crafted in C (port.c)
 * carries the extra zero byte, and EIND/RAMPZ join the save set. All of it
 * keys off __AVR_3_BYTE_PC__ / EIND / RAMPZ, so one file serves both Uno
 * (ATmega328P) and Mega (ATmega2560).
 */
#include <avr/io.h>

/* Shared scratch with port.c (defined there, with C types). */
.extern mrt_avr_next
.extern mrt_avr_tmp_sp
.extern mrt_avr_sched_sp
.extern mrt_avr_in_task
.extern mrt_avr_switch_helper
.extern mrt_avr_yield_helper
.extern mrt_avr_tick_helper

/* Push the full CPU state. Entry: interrupts may be on; exit: cli'd, with
 * the TRUE incoming SREG image on the stack (captured before cli). */
.macro MRT_SAVE_ALL
    push r0
    in r0, _SFR_IO_ADDR(SREG)
    cli
    push r0                     /* SREG image (true I-bit) */
    push r1
    clr r1                      /* ABI pins r1 to zero */
    .irp reg,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14,r15,r16,r17,r18,r19,r20,r21,r22,r23,r24,r25,r26,r27,r28,r29,r30,r31
    push \reg
    .endr
#ifdef EIND
    lds r0, EIND
    push r0
#endif
#ifdef RAMPZ
    lds r0, RAMPZ
    push r0
#endif
.endm

/* Pop the full CPU state. Exact reverse of SAVE_ALL; the SREG restore is
 * what re-enables interrupts for tasks that ran enabled. */
.macro MRT_RESTORE_ALL
#ifdef RAMPZ
    pop r0
    sts RAMPZ, r0
#endif
#ifdef EIND
    pop r0
    sts EIND, r0
#endif
    .irp reg,r31,r30,r29,r28,r27,r26,r25,r24,r23,r22,r21,r20,r19,r18,r17,r16,r15,r14,r13,r12,r11,r10,r9,r8,r7,r6,r5,r4,r3,r2
    pop \reg
    .endr
    pop r1
    pop r0
    out _SFR_IO_ADDR(SREG), r0
    pop r0
.endm

/* Scheduler -> task. Called from C as mrt_port_switch_to(next): the TCB
 * pointer arrives in r25:r24 and is stashed before SAVE_ALL (which would
 * bury it on the stack at a brittle offset). Returns to the C caller when
 * the task later yields; first-runs land in the trampoline instead. */
.global mrt_port_switch_to
.type mrt_port_switch_to, @function
mrt_port_switch_to:
    sts mrt_avr_next, r24
    sts mrt_avr_next+1, r25
    MRT_SAVE_ALL
    in r0, _SFR_IO_ADDR(SPL)
    sts mrt_avr_tmp_sp, r0
    in r0, _SFR_IO_ADDR(SPH)
    sts mrt_avr_tmp_sp+1, r0
    call mrt_avr_switch_helper  /* sched_sp = tmp_sp; tmp_sp = next.sp */
    lds r0, mrt_avr_tmp_sp+1
    out _SFR_IO_ADDR(SPH), r0             /* SPH first, then SPL (cli'd: order safe) */
    lds r0, mrt_avr_tmp_sp
    out _SFR_IO_ADDR(SPL), r0
    /* Flag AFTER the SP load: a tick between load and flag takes the
     * no-switch path and resumes the task (a missed preemption, harmless);
     * flagging first would let a tick save the MAIN SP as the task's. */
    ldi r16, 1
    sts mrt_avr_in_task, r16
    MRT_RESTORE_ALL
    ret

/* Task -> scheduler. Symmetric: the task frame is saved, its SP recorded
 * by the helper, and the scheduler frame (main stack) restored. */
.global mrt_port_yield_to_sched
.type mrt_port_yield_to_sched, @function
mrt_port_yield_to_sched:
    /* Clear FIRST (r1 is the ABI zero): a tick anywhere below sees
     * scheduler context and skips the switch — safe at every SP, since
     * the yield's own save/restore brackets the whole sequence. */
    sts mrt_avr_in_task, r1
    MRT_SAVE_ALL
    in r0, _SFR_IO_ADDR(SPL)
    sts mrt_avr_tmp_sp, r0
    in r0, _SFR_IO_ADDR(SPH)
    sts mrt_avr_tmp_sp+1, r0
    call mrt_avr_yield_helper   /* current.sp = tmp_sp */
    lds r0, mrt_avr_sched_sp+1
    out _SFR_IO_ADDR(SPH), r0
    lds r0, mrt_avr_sched_sp
    out _SFR_IO_ADDR(SPL), r0
    MRT_RESTORE_ALL
    ret

/* Preemptive tick, Timer1 compare-A. Hardware pushed the PC and cleared I;
 * SAVE_ALL stacks the rest on the LIVE stack (a task's, or the scheduler's
 * main stack if the tick landed between switches — the C helper tells the
 * cases apart via the current-task pointer and only switches mid-task). */
.global TIMER1_COMPA_vect
.type TIMER1_COMPA_vect, @function
TIMER1_COMPA_vect:
    MRT_SAVE_ALL
    in r0, _SFR_IO_ADDR(SPL)
    sts mrt_avr_tmp_sp, r0
    in r0, _SFR_IO_ADDR(SPH)
    sts mrt_avr_tmp_sp+1, r0
    call mrt_avr_tick_helper    /* tick+wake; maybe switch (rewrites tmp_sp) */
    lds r0, mrt_avr_tmp_sp+1
    out _SFR_IO_ADDR(SPH), r0
    lds r0, mrt_avr_tmp_sp
    out _SFR_IO_ADDR(SPL), r0
    MRT_RESTORE_ALL
    reti
