# Docker Operator Runbook

Use this runbook when you need to validate n00b in Docker across:

- `linux/amd64`
- `linux/arm64`

## What This Validates

Each verify run executes the repository-native build/test sequence in-container:

1. `N00B_CLEAN=1 ./build.sh`
2. `meson test -C build_debug --print-errorlogs --suite unit`
3. `N00B_TEST=1 ./build.sh`

If any step fails on either architecture, the overall verify run fails.

## Preflight Checklist

- Current directory is repository root.
- `docker buildx version` succeeds.
- Builder supports `linux/amd64` and `linux/arm64`.
- Host can run arm64 containers (native arm64 or QEMU/binfmt configured).
- Network access is available for package installs.

## Initial Builder Setup (Once Per Host)

```bash
docker buildx create --name n00b-multi --driver docker-container --use
docker buildx inspect --bootstrap
```

Check that `linux/amd64` and `linux/arm64` appear in the platform list.

## Standard Operating Commands

Fast single-platform smoke check:

```bash
docker buildx bake -f docker/buildx-bake.hcl verify-local --progress=plain
```

Full multi-platform verification:

```bash
docker buildx bake -f docker/buildx-bake.hcl verify --progress=plain
```

Force `verify-local` to arm64:

```bash
N00B_LOCAL_PLATFORM=linux/arm64 docker buildx bake -f docker/buildx-bake.hcl verify-local --progress=plain
```

Run full verify for only one platform:

```bash
N00B_PLATFORMS=linux/arm64 docker buildx bake -f docker/buildx-bake.hcl verify --progress=plain
```

## Success Criteria

- Logs contain `========== success ==========` for each platform.
- Logs contain `all required build and test phases passed` for each platform.
- `docker buildx bake ... verify` exits with code `0`.

## Cache Operations

This bake configuration imports from `.buildx-cache` and exports to
`.buildx-cache-new`.

After successful verify:

```bash
rm -rf .buildx-cache
mv .buildx-cache-new .buildx-cache
```

If cache export prints transient lock warnings after both platforms already
passed verification, classify that as a cache/export issue.

## Incident Response / Recovery

Builder missing:

```bash
docker buildx create --name n00b-multi --driver docker-container --use
```

Builder lacks arm64:

```bash
docker buildx inspect --bootstrap
```

If arm64 is still missing, enable QEMU/binfmt on the host and bootstrap again.

Stale local state:

```bash
rm -rf .buildx-cache .buildx-cache-new
docker buildx bake -f docker/buildx-bake.hcl verify-local --progress=plain
```

Unexpectedly long arm64 compile times:

- This is normal for large generated Unicode files under emulation.
- If CPU is active, wait; this stage can be significantly slower than amd64.
