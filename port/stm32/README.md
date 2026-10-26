# STM32 port (Phase 5 — planned)

Default assumption (confirm before starting Phase 5): **STM32F103C8**
("Blue Pill" board). If you own different STM32 hardware, say so and the
port targets that instead — the kernel side does not change.

## What Phase 5 implements here

- Startup code: vector table, `.data`/`.bss` init, clock configuration,
  SysTick as the RTOS tick, PendSV as the context-switch exception.
- `port.c` + assembly: PendSV handler saving/restoring the Cortex-M register
  frame, NVIC priority setup (PendSV lowest so it never preempts the tick),
  critical sections via `BASEPRI`/`PRIMASK` discipline.
- Linker script for the chosen part's flash/SRAM layout and completion of
  `cmake/toolchain-stm32.cmake`.
- UART demo output proving the same kernel tests that pass on host/AVR.

Reference input for bring-up: the ST datasheet + reference manual revisions
for the exact part chosen, pinned in this file when Phase 5 starts.
