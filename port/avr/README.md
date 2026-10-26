# AVR port (Phase 4 — done, green on Uno + Mega)

Targets, per the official Arduino docs:

- Arduino Uno R3 — **ATmega328P**, 16 MHz, 32 KB flash / 2 KB SRAM.
  Specs: <https://docs.arduino.cc/hardware/uno-rev3/>
- Arduino Mega 2560 Rev3 — **ATmega2560**, 16 MHz, 256 KB flash / 8 KB SRAM.
  Specs: <https://docs.arduino.cc/hardware/mega-2560/>

One port file pair serves both chips; the differences (2- vs 3-byte return
addresses, `EIND`/`RAMPZ`) are `#ifdef` branches, not a fork.

## How it works

- `port_asm.S` owns register moves: `MRT_SAVE_ALL` / `MRT_RESTORE_ALL`
  (r0, SREG with its true I-bit, r1, r2–r31, plus `EIND`/`RAMPZ` on the
  Mega), `mrt_port_switch_to` (scheduler → task), `mrt_port_yield_to_sched`
  (task → scheduler), and the naked Timer1 compare-A tick ISR.
- `port.c` owns decisions: Timer1 in CTC mode at 1 kHz
  (`OCR1A = F_CPU/64/1000 − 1 = 249`), the static task-stack pool, the
  first-run frame crafter, and the three mid-switch C helpers the asm calls.
- First run: each task starts with a crafted stack that `ret` turns into a
  jump to `avr_trampoline`, which calls the body with its argument.
- Sleep: the scheduler idles with `sleep_cpu()`; the tick wakes it.

## Three things that bit during bring-up (read before porting!)

1. **Return-address byte order.** AVR `call` pushes PCl first (lands high),
   PCh second (lands low, popped first by `ret`); on 3-byte-PC parts the
   EIND byte lands lowest of the three. avr-libc's `setjmp` pops in exactly
   this order. Backwards = the first `ret` lands byte-swapped in the weeds.
2. **AVR stacks are EMPTY-descending.** PUSH post-decrements after storing,
   POP pre-increments before loading — SP names the FREE slot and the top
   byte sits at SP+1 (the opposite of ARM). A crafted frame's SP must point
   one BELOW its lowest byte. Off-by-one = every pop shifts, `ret` fetches
   garbage, and the MCU reboot-loops with zero output.
3. **The tick can't trust "a task is current".** `s_current` is already set
   while `switch_to` still runs on the main stack (and still set during
   yield's way home). Saving the live SP as a task SP there stores the MAIN
   SP and resumes wild. So the asm maintains `mrt_avr_in_task` (set after
   the SP load, cleared before the yield save) and the tick helper only
   switches mid-task. Either ordering reversed reopens the hole.

## The 64 KB question (Mega)

The kernel + tasks link below the 64 KB line (the whole selftest is ~11 KB
of flash), so the crafted EIND byte is always 0 and the pushed return
address always names the (low) trampoline. Code linked above 64 KB still
works: the linker routes long calls through `__trampolines` stubs below the
line, and those are what task function pointers name. The 3-byte path IS
exercised on the Mega build — every save/restore pushes and pops the third
PC byte plus `EIND`/`RAMPZ` — just with a zero high byte, exactly like
real sub-64 KB firmware on that chip.

## Memory budgets (measured, avr-size)

| Firmware | Flash | SRAM |
|---|---|---|
| Uno blinky | 4,416 B (13.5%) | 1,305 B (63.7%) |
| Uno selftest | 11,106 B (33.9%) | 1,812 B (88.5%) |
| Mega selftest | 11,310 B (4.3%) | 3,176 B (38.8%) |

Uno task stacks are 192 B × 5 (4 tasks + idle); Mega uses 256 B × 9. The
selftest enforces a 32-byte minimum watermark per task (`W` verdict).

## Build and prove (no hardware needed)

```sh
export PATH=/home/miha/dev/mini_rtos/.toolchains/avr/usr/bin:$PATH
SIMAVR_BIN=/home/miha/dev/mini_rtos/.toolchains/src/simavr/simavr/run_avr \
  ./tools/avr_sim_check.sh uno    # expect ABABAB!END + KLHl!END
SIMAVR_BIN=/home/miha/dev/mini_rtos/.toolchains/src/simavr/simavr/run_avr \
  ./tools/avr_sim_check.sh mega   # same transcripts, 3-byte PC path
```

See [docs/TOOLCHAINS.md](../../docs/TOOLCHAINS.md) for the user-local
toolchain setup (avr-gcc 15.2.0, simavr from source).
