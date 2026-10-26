# Toolchains

## Verified on this machine (Phase 0, 2026-10-26)

- Host C compiler: `gcc 16.2.1 (Red Hat 16.2.1-2)` — present.
- `cmake 4.3.0` — present.
- `python3 3.14.8` — present (used by helper scripts, not the build).
- `avr-gcc` — MISSING (needed in Phase 4).
- `arm-none-eabi-gcc` — MISSING (needed in Phase 5).

Re-check at any time with:

```sh
./tools/check_toolchains.sh
```

## Installing the missing cross-toolchains (Phase 4/5)

On Fedora the typical install is the AVR and ARM embedded GCC packages —
verify exact names with `dnf search` first, as packaging changes:

```sh
dnf search avr-gcc arm-none-eabi-gcc   # confirm names on your release
sudo dnf install avr-gcc avr-libc arm-none-eabi-gcc-cs arm-none-eabi-newlib
```

On Debian/Ubuntu the equivalents are historically `gcc-avr avr-libc` and
`gcc-arm-none-eabi`; on macOS use Homebrew (`avr-gcc`, `arm-none-eabi-gcc`).

Once installed, pin the exact versions here (output of
`avr-gcc --version` and `arm-none-eabi-gcc --version`) so firmware builds
stay reproducible, then complete `cmake/toolchain-avr.cmake` and
`cmake/toolchain-stm32.cmake`.
