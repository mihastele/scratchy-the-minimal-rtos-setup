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

## Memory discipline

- All kernel objects are statically allocated by the caller (`..._create_static`).
  The kernel owns no heap and calls no `malloc`. On a 2 KB-SRAM Uno there is
  nowhere for a heap to hide, so the design never assumes one.
- Each port owns the machine state: on host, the `ucontext_t` + stack lives
  in the port layer and the kernel only holds an opaque pointer to it.
