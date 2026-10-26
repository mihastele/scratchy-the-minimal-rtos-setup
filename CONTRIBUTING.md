# Contributing to mini_rtos

## Commenting standard

This project is a textbook that happens to run. Every new file starts with a
header comment answering: what lives here, why it exists, and how it fits the
whole. Every non-obvious block gets a short "why" comment. If you had to think
for more than a minute, the next reader deserves a comment.

## Code rules

- C11, `-Wall -Wextra -Werror`, no warnings accepted.
- Kernel code (`kernel/`, `include/mini_rtos/`) must stay portable: no
  register access, no libc calls except via the port layer, no `malloc`.
- Every new kernel object needs a `CreateStatic` form and a bounded test in
  `tests/` that runs under CTest on the host.
- Public API changes update `docs/ARCHITECTURE.md` in the same change.

## Workflow

- Out-of-source builds only: `cmake -S . -B build-host`, never configure
  in the source tree.
- Run the focused test for your change first, then the full host suite:
  `ctest --test-dir build-host`.
- AVR/STM32 ports get their own directories under `port/` and must implement
  the full `port_api.h` contract; stub functions that silently do nothing are
  not acceptable — fail loudly or not at all.
