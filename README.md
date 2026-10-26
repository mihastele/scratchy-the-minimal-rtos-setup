# mini_rtos

A free (MIT licensed), production-minded real-time OS written for learning.
Every file explains what it does and why, so you can read the source top to
bottom like a textbook — and still ship it on a real board.

## Status: Phase 4 (preemptive AVR port proven under simavr)

- Host simulator builds and runs demo tasks: yes.
- Kernel core + IPC + memory: everything in Phase 2, plus task delete with
  reaping, fixed-block pools, fenced-off app heap, stack watermarks, and a
  build gate proving the kernel never touches libc heap — 16/16 checks green.
- Arduino Uno (ATmega328P) / Mega (ATmega2560): done — real context switch,
  1 kHz preemptive tick, simavr transcripts `ABABAB!END` + `KLHl!END` on
  both boards. See [port/avr/README.md](port/avr/README.md).
- One STM32 (default assumption STM32F103C8, to confirm): planned, Phase 5.

See [docs/PLAN.md](docs/PLAN.md) for the phased divide-and-conquer plan and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the pieces fit together.

## Quick start (host simulator)

Prerequisites: `gcc`, `cmake` (>= 3.16), `make`. No cross-toolchains needed
for the host build.

```sh
cmake -S . -B build-host
cmake --build build-host
ctest --test-dir build-host -V
./build-host/examples/blinky_host
```

Expected: the test suite passes and `blinky_host` prints interleaved `A`/`B`
task lines for a bounded number of ticks.

## Layout

- `include/mini_rtos/` — public kernel API (what applications include).
- `kernel/` — portable kernel: tasks, scheduler, tick math. Never touches
  hardware registers; talks to the MCU only through `port_api.h`.
- `port/host/` — simulator port (POSIX `ucontext` context switch, virtual
  tick). Host-only; real ports use assembly + timers.
- `port/avr/` — Arduino Uno/Mega port (done; proven under simavr).
- `port/stm32/` — STM32 port (Phase 5; README + checklist for now).
- `drivers/` — UART/GPIO/timer glue (Phase 6; README for now).
- `examples/` — small demos, one per concept.
- `tests/` — focused CTest suite, no external framework.
- `cmake/` — AVR toolchain file (done) + STM32 stub (Phase 5).
- `docs/` — plan, architecture, toolchain notes.
- `tools/` — helper scripts (toolchain check, size report).

## Design rules

- Static allocation first: every kernel object has a `CreateStatic` form.
  The kernel never calls `malloc` behind your back.
- Portable core, thin ports: adding a chip means implementing the small
  `port_api.h` contract, not forking the kernel.
- Heavily commented: if a block is non-obvious, it carries a "why" comment.
- `-Wall -Wextra -Werror`, C11, tested on host before touching hardware.

## License

MIT — see [LICENSE](LICENSE).
