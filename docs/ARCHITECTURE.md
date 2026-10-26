# mini_rtos architecture

## The one big idea

The kernel is split into two halves with a tiny contract between them:

- `kernel/` + `include/mini_rtos/` — portable logic: which task runs next,
  who waits for what, when timeouts expire. It never touches a hardware
  register. You could compile it for any CPU without changes.
- `port/<target>/` — the only code allowed to touch hardware: saving and
  restoring CPU registers (context switch), the tick timer, and
  enable/disable-interrupt (critical sections).

The contract is `include/mini_rtos/port_api.h`. Porting mini_rtos to a new
chip means implementing those few functions, not forking the kernel. This is
the same shape FreeRTOS and Zephyr use, because it is the shape that keeps
ports honest.

## Tasks and scheduling (Phase 1 core)

- A task is a C function plus a Task Control Block (TCB): name, priority,
  state (ready / delayed / suspended), and a wake-up tick.
- The scheduler always runs the highest-priority READY task. Tasks at equal
  priority take turns (round-robin), so no task starves its peers.
- `mrt_task_delay_ticks(n)` marks the caller DELAYED with
  `wake_tick = now + n` and yields. The tick ISR wakes it once the tick
  counter reaches `wake_tick`. Tick math is unsigned 32-bit, so it survives
  the ~49-day wraparound; see `tick.h` for why `(int32_t)(now - when) >= 0`
  is the correct comparison.
- `mrt_task_delay_until(&prev, period)` is the phase-locked variant for
  periodic loops: the deadline derives from the previous deadline, so loop
  jitter never accumulates drift. Overruns skip the block but keep phase.
- `mrt_task_suspend()` / `mrt_task_resume()` park and unpark tasks;
  suspending abandons any in-progress delay. Priorities can be read and
  changed at runtime; changes take effect at the next pick.
- A task that returns from its function is suspended (it is finished, not
  runnable). Task delete with slot reaping arrives in Phase 3.
- The idle task is kernel-owned, lives outside the pick table at
  `MRT_IDLE_SLOT`, and runs only when nothing else is READY. Each idle pass
  runs the application idle hook (if set) and then the port's sleep action.

## IPC: queues, semaphores, mutexes, events (Phase 2)

All blocking shares one waiter protocol (`kernel/wait.c`): a blocked task
is DELAYED (timed) or BLOCKED (FOREVER) with `wait_obj` pointing at the
object. Exactly three things end a wait — the object wakes it (OK), the
deadline passes (TIMEOUT), or a suspend abandons it (STATE on resume) — and
all three detach the wait, so a stale wake can never corrupt a later one.
Wake order is highest effective priority first, FIFO within a level.

Two wake flavors, and the difference matters:

- Hint wakes (queues): a freed slot or new item may be gone by the time the
  woken task runs, so send/receive loop and re-check under critical. The
  deadline is fixed outside the loop, so re-waiting never extends the total.
- Handoff wakes (semaphores, mutexes, events): the token, ownership, or bit
  snapshot is reserved for the woken task at wake time — no loop needed.

Priority inversion story: mutexes implement single-level priority
inheritance (a waiter lends the owner its effective priority until unlock),
which bounds inversion to one critical section. Nested contended mutexes
are not transitively tracked — the design rule is to never hold more than
one contended mutex (global lock order, short holds, reverse-order unlock).
Full transitive PI is a documented v1 non-goal; the inversion test pins the
single-level guarantee observably.

ISR rule, enforced not just documented: any call that can block or that
reports a wake refuses with MRT_ERR_ISR in interrupt context — the `_isr`
twins exist for exactly that context and report `woke` so the port can
reschedule. Calls that neither block nor wake (`event_clear/get`,
`queue/sem_count`) are legal everywhere and have no twins.

## Time on the host vs on hardware

- All time flows through `mrt_kernel_on_tick_isr()`: +1 tick, wake whoever
  is due, report whether anyone woke. The host calls it once per scheduler
  pass (at the pass boundary); on hardware the timer ISR calls the identical
  function. Virtual time is deterministic: the same binary always produces
  the same interleaving, which is what makes the tests reproducible.
- The one honest difference: on hardware the ISR can preempt a task
  mid-function, while the host runs it between passes. The kernel code does
  not change between the two; only the port's tick source, context switch,
  and reschedule trigger do (Phase 4/5).

## The host port trick

Real context switches are assembly. On the host we fake them with POSIX
`ucontext` (`getcontext`/`makecontext`/`swapcontext`): each task gets its
own stack buffer and context, and yielding swaps back to the scheduler
context. This teaches the real shape — save current state, restore next
state — while staying in plain C. `ucontext` is host-only and never ships
on a microcontroller.

## The AVR port (Phase 4)

Same contract, no fakery: Timer1 in CTC mode is the 1 kHz tick, and
`port_asm.S` + `port.c` implement the real thing on ATmega328P (Uno) and
ATmega2560 (Mega) from one source pair:

- Context switch = save all registers + SREG to the live stack, stash SP,
  load the next SP, restore, `ret`. First-run tasks get a crafted stack
  whose `ret` lands in a trampoline that calls the task body.
- The tick ISR is a naked handler: save, stash SP, run one C helper (tick
  + wake + maybe switch), reload SP, restore, `reti`. The helper only
  switches stacks when the tick landed mid-task — an `in_task` flag the
  asm maintains tells scheduler-stack from task-stack apart, because the
  current-task pointer alone is set in both cases.
- Mega delta: 3-byte return addresses (pushed EIND byte) plus `EIND`/`RAMPZ`
  in the save/restore set. Firmware links below 64 KB, so the high byte is
  0 — see `port/avr/README.md` for the full story and the three bring-up
  bugs (return-address byte order, empty-descending SP, the `in_task` flag).
- Proven with no hardware: `tools/avr_sim_check.sh uno|mega` rebuilds both
  example firmwares and demands exact UART transcripts under simavr
  (`ABABAB!END`, `KLHl!END` — the latter deadlocks without preemption).

## Memory discipline (Phase 3)

- All kernel objects are statically allocated by the caller (`..._create_static`).
  The kernel owns no heap and calls no libc allocator — enforced by the
  `no_heap_in_kernel` CTest gate (source grep + undefined-symbol check). On
  a 2 KB-SRAM Uno there is nowhere for a heap to hide, so the design never
  assumes one.
- Each port owns the machine state: on host, the `ucontext_t` + stack lives
  in a port-side pool found by pointer (never by table slot, which shifts
  on delete); the kernel only holds the opaque `port_state`.
- Task delete: deleting another task reaps synchronously (detach, free port
  state, compact the table); self-delete marks DELETED and the idle task
  reaps. A per-TCB owned-mutex count refuses to orphan locks (delete while
  holding is STATE). Suspended reads of zombies are fenced (suspend no-ops,
  resume/priority refuse) so the dead can never resurrect.
- Fixed-block pools (intrusive free list, LIFO, ISR-safe) are the default
  allocator for same-size objects; pair with a semaphore to block for one.
  The tiny first-fit heap exists for applications only, with documented
  fragmentation warnings — kernel code calling it fails the build.
- Stack watermarks: ports paint stacks at creation and count survivors, so
  every stack size ships with measured evidence. Value-returning queries
  (counts, allocators) treat NULL as the failure value in all builds —
  testable everywhere, no debug-only traps.
