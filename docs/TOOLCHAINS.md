# Toolchains

## Verified on this machine (Phase 0, 2026-10-26)

- Host C compiler: `gcc 16.2.1 (Red Hat 16.2.1-2)` — present.
- `cmake 4.3.0` — present.
- `python3 3.14.8` — present (used by helper scripts, not the build).
- `avr-gcc` — MISSING (needed in Phase 4).
- `arm-none-eabi-gcc` — MISSING (needed in Phase 5).

## Verified additions (Phase 4, 2026-10-26)

- AVR C compiler: `avr-gcc (Fedora 15.2.0-1.fc44) 15.2.0` — user-local
  extract under `.toolchains/avr` (Fedora RPMs via `dnf download` +
  `rpm2cpio`, no root needed). Put on PATH per shell:
  `export PATH=$PWD/.toolchains/avr/usr/bin:$PATH`.
- Simulator: simavr built from source under `.toolchains/src/simavr`
  (`run_avr` core only; the bundled examples don't build against this
  avr-gcc). Run with `SIMAVR_BIN=$PWD/.toolchains/src/simavr/simavr/run_avr`.
- `.toolchains/` is machine-local roughness: never commit it (git-ignored),
  just the recipes above.

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
