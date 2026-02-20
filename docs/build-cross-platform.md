# Meson Cross-Platform Builds

This repository uses `./build.sh` as the only supported top-level build entrypoint.
These commands cover Linux/macOS native validation plus Linux arm64 and Windows x86_64
cross-compilation.

## Prerequisites

- Native host lanes (Linux/macOS):
  - `python3`, `meson >= 1.6.0`, `ninja`
  - Clang toolchain capable of C23
- Linux arm64 cross lane:
  - `gcc-aarch64-linux-gnu`
  - `binutils-aarch64-linux-gnu`
  - `meson`, `ninja`, `python3`
- Windows x86_64 cross lane:
  - `gcc-mingw-w64-x86-64`
  - `binutils-mingw-w64-x86-64`
  - `meson`, `ninja`, `python3`
  - optional smoke runner: `wine64`

## Native Validation (Linux or macOS)

From repository root:

```bash
N00B_CLEAN=1 ./build.sh
meson test -C build_debug --print-errorlogs --suite unit
N00B_TEST=1 ./build.sh
```

Expected outcome: clean configure/build passes, unit suite passes, and the full regression run passes.

## Linux arm64 Cross-Compilation

Use the checked-in cross file:

```bash
N00B_MESON_CROSS_FILE=meson/cross/linux-arm64.ini \
N00B_NCC_COMPILER=clang \
N00B_TOOLCHAIN_TARGET=aarch64-linux-gnu \
N00B_CLEAN=1 ./build.sh build_cross_linux_arm64
meson compile -C build_cross_linux_arm64
```

Expected outcome: Meson configures as a cross build and all targets compile for `aarch64`.

Optional test execution requires an `exe_wrapper` in the cross file (for example `qemu-aarch64`).
Without a wrapper, cross-built tests are compile-only.

## Windows x86_64 Cross-Compilation

Use the checked-in cross file:

```bash
N00B_MESON_CROSS_FILE=meson/cross/windows-x86_64.ini \
N00B_NCC_COMPILER=clang \
N00B_TOOLCHAIN_TARGET=x86_64-w64-windows-gnu \
N00B_CLEAN=1 ./build.sh build_cross_windows_x86_64
meson compile -C build_cross_windows_x86_64
```

Expected outcome: Meson configures as a cross build and all targets compile for
`x86_64-w64-windows-gnu`.

Optional smoke execution under Wine:

```bash
meson test -C build_cross_windows_x86_64 --print-errorlogs --wrapper wine64 list tuple
```

If `wine64` is not available, keep the lane compile-only.

## Optional Meson File Plumbing

`build.sh` supports optional Meson file arguments:

- `N00B_MESON_NATIVE_FILE=<path>`: pass `--native-file` to bootstrap and main project setup.
- `N00B_MESON_CROSS_FILE=<path>`: pass `--cross-file` to the main project setup.
- `N00B_NCC_COMPILER=<compiler>`: set backend compiler used by `ncc-bootstrap`.
- `N00B_TOOLCHAIN_TARGET=<triple>`: set Meson `toolchain_target` (clang `--target=` triple).
- `N00B_BUILD_DIR=<path>`: choose build directory when no positional build dir argument is provided.

Example:

```bash
N00B_MESON_NATIVE_FILE=meson/native/linux.ini N00B_BUILD_DIR=build_linux_native ./build.sh
```
