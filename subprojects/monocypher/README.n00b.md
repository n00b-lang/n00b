# Monocypher (vendored)

Upstream: https://github.com/LoupVaillant/Monocypher
Version: v4.0.2
License: CC0-1.0 OR BSD-2-Clause (see [LICENCE.md](LICENCE.md))

## What we ship

This subproject is an in-tree copy of upstream Monocypher v4.0.2,
restricted to the four files n00b needs for Ed25519 signing:

- `src/monocypher.c` + `include/monocypher.h` — core (field arithmetic,
  Curve25519 ops, BLAKE2b-based EdDSA).
- `src/monocypher-ed25519.c` + `include/monocypher-ed25519.h` —
  RFC 8032 SHA-512 Ed25519 (`crypto_ed25519_*` + `crypto_sha512_*`).

We use the **SHA-512 Ed25519** variant (`crypto_ed25519_sign` /
`crypto_ed25519_check`) for DSSE signing because that's the RFC 8032
form interoperable with all standards-compliant verifiers. The
`crypto_eddsa_*` family in core Monocypher is BLAKE2b-based and
**not** interchangeable with RFC 8032 Ed25519.

## Why in-tree (not meson-wrap)

Per D-034 the integration shape was delegated to the Phase 1 sub-agent.
We chose in-tree (matching the `subprojects/picotls/` precedent) for:

1. **Build determinism.** No network fetch at configure time;
   reproducible across air-gapped CI.
2. **Surgical surface.** Upstream Monocypher ships test/doc/changelog
   artifacts we don't need; the in-tree copy is the four sources +
   the licence + this README.
3. **Meson driver.** Upstream uses a Makefile (no native meson
   support), so a wrap file would require a separate
   `packagefiles/monocypher/` overlay anyway. The in-tree shim
   collapses that into one file (`meson.build` alongside the
   sources).

## Upgrade procedure

1. Fetch the new upstream release tag.
2. Replace `src/monocypher.c`, `src/monocypher-ed25519.c`,
   `include/monocypher.h`, `include/monocypher-ed25519.h`,
   `LICENCE.md`, `AUTHORS.md`, `CHANGELOG.md` with the new versions.
3. Bump the `version:` field in `meson.build`.
4. Run the smoke regression
   (`meson test -C build_debug --print-errorlogs attest_monocypher_smoke`)
   to confirm the RFC 8032 test vector still verifies cleanly. Any
   silent ABI / behavior change in upstream will fail this gate.
