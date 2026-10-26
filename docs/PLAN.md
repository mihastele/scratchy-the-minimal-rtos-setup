# mini_rtos plan (divide and conquer)

Goal: a free MIT-licensed, production-minded, heavily-commented RTOS for
learning and real use. Portable C11 kernel + thin per-chip ports.

## Phase 0 — Scaffold and toolchain (done)

- Repo layout, MIT LICENSE, README, CMake host build, CTest suite.
- Minimal commented kernel + host simulator running two tasks.
- AVR/STM32 toolchain stubs; record host tool versions.
- Done when: `ctest --test-dir build-host` is green and `blinky_host`
  prints interleaved tasks.

## Phase 1 — Kernel core on host (done)

- TCB/task lifecycle (suspend/resume; delete deferred to Phase 3 reaping),
  fixed-priority + round-robin scheduler, tick ISR + timeout wakes,
  critical-section discipline, idle task with app hook + port sleep hook.
- Timing model proven deterministically on the host port: one tick ISR per
  pass, same function hardware ISRs will call. Task delete arrives with the
  memory work in Phase 3.

## Phase 2 — IPC and sync on host

- Queues, binary/counting semaphores, mutexes, event flags.
- Blocking with timeout, non-blocking, and ISR-safe variants; documented
  priority-inversion story.

## Phase 3 — Memory and objects

- `CreateStatic` everywhere, fixed-block pools, optional tiny heap kept out
  of kernel paths, stack high-water marks, OOM/error-path tests.

## Phase 4 — AVR port (Uno then Mega)

- Uno (ATmega328P) first: timer tick, AVR context-switch assembly,
  `SREG`/SP handling, USART0 demo output.
- Mega (ATmega2560) delta: 3-byte program counter, `EIND` handling, large
  flash linker script. Highest-risk step: prove a task above the 64 KB
  boundary returns correctly.

## Phase 5 — STM32 port (one variant)

- Default assumption: STM32F103C8 ("Blue Pill") — confirm before starting.
- Vector table, SysTick tick, PendSV context switch, NVIC priorities,
  startup/linker script, UART demo. Same kernel tests as AVR.

## Phase 6 — Driver glue and examples

- Minimal UART/GPIO/timer drivers behind one interface per board family.
- Examples: `blinky`, `producer_consumer`, `cli_over_uart`, each with wiring
  and expected serial output.

## Phase 7 — Hardening and learning docs

- Stress/soak on host, stack-overflow detection, fault injection, per-target
  size reports, porting/memory/testing guides, v1.0 API freeze.
