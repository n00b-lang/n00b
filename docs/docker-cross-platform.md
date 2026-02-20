# Cross-Platform Docker Verification

This guide is for human operators running n00b verification in Docker for:

- `linux/amd64`
- `linux/arm64`
- Windows cross build (Linux container target, MinGW toolchain):
  - default platform: `linux/amd64`

The verification container always runs this sequence:

1. `N00B_CLEAN=1 ./build.sh`
2. `meson test -C build_debug --print-errorlogs --suite unit`
3. `N00B_TEST=1 ./build.sh`

The Windows-cross verification container runs:

1. `N00B_MESON_CROSS_FILE=meson/cross/windows-x86_64.ini N00B_NCC_COMPILER=clang N00B_TOOLCHAIN_TARGET=x86_64-w64-windows-gnu N00B_CLEAN=1 ./build.sh build_cross_windows_x86_64`
2. `meson compile -C build_cross_windows_x86_64`
3. optional Wine smoke subset (`list`, `tuple`) when `wine64` is available.

## Before You Start

- Run from repository root.
- Confirm Docker and Buildx are available:
  `docker buildx version`
- Ensure your builder can execute `linux/arm64` (native or QEMU/binfmt).
- Ensure network access for pulling Ubuntu/toolchain dependencies.

## Quick Start

1. Create/select a builder:
```bash
docker buildx create --name n00b-multi --driver docker-container --use
docker buildx inspect --bootstrap
```

2. Run a fast smoke check on one platform:
```bash
docker buildx bake -f docker/buildx-bake.hcl verify-local --progress=plain
```

3. Run full multi-platform verification:
```bash
docker buildx bake -f docker/buildx-bake.hcl verify --progress=plain
```

4. Run Windows cross verification:
```bash
docker buildx bake -f docker/buildx-bake.hcl verify-windows-cross-local --progress=plain
docker buildx bake -f docker/buildx-bake.hcl verify-windows-cross --progress=plain
```

## What Success Looks Like

- Both `linux/amd64` and `linux/arm64` reach:
  `========== success ==========` and `all required build and test phases passed`
- `docker buildx bake ... verify` exits with status `0`.
- Windows-cross lane reaches:
  `========== success ==========` and `windows cross configure+compile verification passed`.

## Common Operator Commands

Run `verify-local` on arm64:

```bash
N00B_LOCAL_PLATFORM=linux/arm64 docker buildx bake -f docker/buildx-bake.hcl verify-local --progress=plain
```

Run only one platform in the full target:

```bash
N00B_PLATFORMS=linux/arm64 docker buildx bake -f docker/buildx-bake.hcl verify --progress=plain
```

Run Windows cross on a specific local platform:

```bash
N00B_WINDOWS_CROSS_LOCAL_PLATFORM=linux/amd64 docker buildx bake -f docker/buildx-bake.hcl verify-windows-cross-local --progress=plain
```

## Cache Handling

Buildx uses:

- `.buildx-cache` for import
- `.buildx-cache-new` for export

After a successful run:

```bash
rm -rf .buildx-cache
mv .buildx-cache-new .buildx-cache
```

If you see cache lock warnings at export time but both platform verify stages already
passed, treat that as a cache-export issue, not a test failure.

## Troubleshooting

- `no builder "n00b-multi"`:
  recreate/select with
  `docker buildx create --name n00b-multi --driver docker-container --use`.
- `linux/arm64` not listed in `docker buildx inspect --bootstrap`:
  enable QEMU/binfmt and re-run bootstrap.
- permission denied talking to Docker:
  fix host Docker access for your user before retrying.
- arm64 appears stuck for long periods:
  large generated Unicode files can take much longer under emulation; this is expected.
- package install or toolchain pull failures:
  retry with network access restored, optionally with no cache.
- missing MinGW tools (`x86_64-w64-mingw32-*` not found):
  ensure the `verify-windows-cross` stage completed package installation.
- Wine smoke output indicates failures:
  this lane is compile-required; smoke is currently non-blocking.
