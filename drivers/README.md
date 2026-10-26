# Drivers (Phase 6 — planned)

Minimal blocking-first drivers behind one small interface per peripheral
(UART tx/rx, GPIO, timer delay). One implementation per board family:

- `drivers/avr_usart.c` — Uno/Mega shared USART0 (polled first, then
  interrupt-driven rx ring buffer).
- `drivers/stm32_usart.c` — STM32 USART for the Phase 5 part.
- `drivers/host_stdio.c` — simulator UART mapped to stdin/stdout so
  examples run unmodified on the host.

Rule: drivers may use kernel IPC (Phase 2) but never reach into scheduler
internals; ISRs use only the `...FromISR` API variants.
