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

## Phase 2 — IPC and sync on host (done)

- Queues, counting/binary semaphores, mutexes, event flags — all with
  blocking + timeout, non-blocking, and ISR-safe variants.
- Shared waiter protocol (priority + FIFO), hint-vs-handoff wake discipline,
  enforced ISR refusal, single-level priority inheritance with a documented
  nesting caveat. 5 new tests, 11/11 green.

## Phase 3 — Memory and objects (done)

- `CreateStatic` everywhere, fixed-block pools, optional tiny heap fenced
  off from kernel paths by a CTest gate, stack high-water marks.
- Task delete with reaping (sync for others, idle-deferred for self),
  mutex-ownership delete guard, zombie-state fencing, churn testing.
  4 new test binaries + heap-freedom gate, 16/16 green.

## Phase 4 — AVR port (Uno then Mega) — DONE, green

- Uno (ATmega328P): Timer1 CTC 1 kHz tick, AVR context-switch assembly
  (`port_asm.S`: save/restore, switch, yield, naked tick ISR), `SREG`/SP
  handling, USART0 polled demo output. simavr transcripts: blinky
  `ABABAB!END`, selftest `KLHl!END` (preemption proof: a no-yield low task
  is preempted by a delayed high task — deadlocks without the tick).
- Mega (ATmega2560) delta: 3-byte program counter (`EIND` craft byte,
  `EIND`/`RAMPZ` save/restore) — green first try after Uno, same two
  transcripts. Firmware links below the 64 KB line, so the high byte is 0
  (see port/avr/README.md for why that still proves the 3-byte path).
- Bring-up bugs fixed along the way (all documented in port/avr/README.md):
  return-address byte order (PCh low, PCl high), empty-descending SP
  (craft points one below the frame), tick-in-scheduler `in_task` flag.
- Also fixed a kernel portability bug the selftest caught: heap payloads
  misaligned on AVR (header sizeof 10, not an 8-multiple) — header stride
  + host alignment regression test. Host suite still 16/16.
- Budgets: Uno blinky 4.4 KB flash / 1.3 KB SRAM; Uno selftest 11.1 KB /
  1.8 KB (88.5% SRAM); Mega selftest 11.3 KB / 3.2 KB.

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
