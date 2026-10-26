# AVR port (Phase 4 — planned)

Targets, per the official Arduino docs:

- Arduino Uno R3 — board based on the **ATmega328P**, 16 MHz.
  Specs: <https://docs.arduino.cc/hardware/uno-rev3/>
- Arduino Mega 2560 Rev3 — board based on the **ATmega2560**, 16 MHz.
  Specs: <https://docs.arduino.cc/hardware/mega-2560/>

Memory budgets to confirm against the vendor datasheets during bring-up
(typical values: 328P ~32 KB flash / 2 KB SRAM; 2560 ~256 KB flash /
8 KB SRAM). The 2 KB Uno SRAM budget is why the kernel is static-only.

## What Phase 4 implements here

- `port.c` + `port_asm.S`: full register save/restore, `SREG` and stack
  pointer handling, Timer tick ISR with preemptive switch.
- Mega delta: 3-byte program counter support (`EIND`, `EICALL`/`EIJMP`
  awareness) so tasks living above the 64 KB flash boundary return
  correctly. This is the highest-risk item in the whole project.
- Linker scripts and `cmake/toolchain-avr.cmake` completion.
- USART0 polled-UART demo output shared by both boards.

Order: Uno first (simpler baseline), then the Mega delta on top.
