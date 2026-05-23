# Open questions

**Status:** SPEC (STABLE)  
**Last updated:** 2026-05-16

Decisions deferred or unresolved as of doc-set draft 0. Each
entry: question, why it matters, options, my current lean,
who's blocked on it. Tagged so other docs can cite them.

---

## OQ-1: Library symbol prefix

**Question.** What prefix do the exported symbols of the new
subsystem use?

**Why it matters.** Per `CLAUDE.md`'s cross-project rule:
"Libraries MUST have every exported symbol start with a prefix
agreed upon w/ the user." Once chosen, it's everywhere; renaming
is annoying.

**Options.**
- `n00b_attest_*` — natural if the subsystem lives in libn00b.
- `co_attest_*` — Crashappsec branding, distinct from n00b's
  general runtime.
- `co_intoto_*` — narrower; emphasizes that the format is
  in-toto and we'd reuse the prefix for non-build attestations.

**Lean.** `n00b_attest_*` if it's a libn00b subsystem (which
00-overview proposes); `co_attest_*` if you decide it's a
separate library. The in-container C library (`04-…`) uses
`co_attest_*` regardless, because it ships outside libn00b.

**Blocked who.** Header layout, the entire public API.

---

## OQ-2: SBOM predicate exact shape

**Question.** Is the predicate shape sketched in
`03-crayon-integration.md` §5 the right one?

**Why it matters.** Downstream consumers will parse this
forever; we have to live with the field names.

**Options.**
- The sketched shape (components + per-component provenance
  subtree + attribution).
- CycloneDX-in-in-toto (loses the per-component build-provenance
  story; gains tool interop).
- Hybrid: CO-native predicate that *also* includes a CycloneDX
  blob as an embedded property for tool-interop convenience.

**Lean.** CO-native per `03-…`. The hybrid is plausible for v1.1
once we hear from customers about CycloneDX consumers in their
toolchain.

**Blocked who.** SBOM emitter, container-rollup SBOM, in-container
predicate-type matching, CLI inspect formatter.

---

## OQ-3: Builder identity URI shape

**Question.** What goes in `predicate.runDetails.builder.id` for
both per-artifact and rollup attestations?

**Why it matters.** Verifiers key trust decisions off this. It
needs to be stable across builds, distinguishable across
customers, and self-describing enough that a verifier can decide
"do I trust this builder."

**Options.**
- Pure hostname: `urn:crashoverride:builder:<hostname>` — too
  ephemeral.
- Per-install-UUID: `urn:crashoverride:crayon:<install-id>` —
  stable, opaque, hard to verify a-priori.
- Per-customer URI: `urn:crashoverride:builder:<customer-org>:
  <project>:<env>` — readable, requires customer-config-driven
  setup.
- Tuple: install-UUID + hostname + project — most informative,
  longest.

**Lean.** Per-install-UUID as the primary identity, with the
hostname embedded as an annotation/byproduct field (not as the
identity itself) for debuggability. The install UUID lives in
`/etc/n00b-attest/install-id` written by the installer.

**Blocked who.** SLSA mapper, verifier policy authors.

---

## OQ-LX-1: Linux default secret-manager backend

**Question.** When Crayon-Linux's installer auto-generates a
signing key, what backend does it write it into by default?

**Why it matters.** This is the on-ramp on Linux. Wrong default
forces a `setup` step we're trying to avoid.

**Options.**
- Per-user libsecret (GNOME Keyring / KWallet) — matches macOS
  Keychain semantics; requires desktop bus, useless on build
  servers.
- Per-user mode-0600 file under `$XDG_CONFIG_HOME/n00b-attest/
  signer.pem` — works everywhere, no daemon, no encryption at
  rest.
- Per-host system file under `/etc/n00b-attest/signer.pem`
  owned by a service account — right for shared build
  infrastructure, wrong for dev laptops.

**Lean.** Mode-0600 file as the default; `--service-account`
flag on the installer flips to system-wide. Libsecret as a
backend exists but isn't the default.

**Blocked who.** Crayon-Linux installer (not v1 of this project,
but the design lives here).

---

## OQ-PUB-1: Public-key publication default at install time

**Question.** When the installer auto-generates a keypair, where
does the public half go *by default*?

**Why it matters.** A signed attestation is useless until a
verifier has the public key. The 0-friction story breaks if
the admin has to manually move the pubkey somewhere.

**Options.**
- Write to `/etc/n00b-attest/local-pubkey.pem` only. Admin
  manually publishes. Low magic, manual step required.
- Upload to a CO-hosted public-key directory if install-time
  config has credentials for it. Magical, depends on hosted
  service.
- Print to stdout, expect MDM / Ansible / etc. to capture
  install logs. Brittle.
- Combine: write to file always; upload if configured; print
  fingerprint always for posterity.

**Lean.** Combine. File always; upload-if-configured;
fingerprint-print always. v1 ships file-only because we don't
run the hosted directory yet; the upload hook is wired for v1.1.

**Blocked who.** Installer, hosted-directory roadmap.

---

## OQ-MDM-1: MDM-pre-provisioned key handover URI

**Question.** What URI shape does an MDM payload write into
`/etc/n00b-attest/signer.json` to tell the installer "use this
key"?

**Why it matters.** MDM platforms (Jamf, Workspace ONE, Intune,
etc.) have different capabilities for pushing payloads. We need
a URI scheme that works with the most-constrained common
denominator.

**Options.**
- `file:/etc/n00b-attest/keys/<keyname>.pem` — works with any
  MDM that can push a file. Plain mode-0600 PEM.
- `keychain:com.crashoverride.n00b-attest.signer` — MDM writes
  to macOS Keychain via configuration profile. Stronger
  security, narrower MDM support.
- Both, with installer preferring whichever exists.

**Lean.** Both. File mode is the universal floor; Keychain is
the macOS-preferred form. Installer probes Keychain first, falls
back to file.

**Blocked who.** Installer, MDM admin docs.

---

## OQ-CACHE-1: Signer-key cache lifetime

(Mostly answered in `02-architecture.md` §6.2 — per-session, no
process-wide cache.)

**Residual question.** For non-Crayon callers using
`n00b_attest_signer_resolve`, is "per-resolve" the right model
or should we offer an explicit caching primitive?

**Lean.** No caching primitive in v1. Callers handle handle
lifetime themselves; that's the simplest, hardest to misuse.

---

## OQ-OBJGRAN-1: Object-file granularity attestations

**Question.** Does v1 enable per-`.o` attestations by default?

**Why it matters.** A make build with 500 `.o` files emits 500
attestations. Each is small but the cumulative push volume is
real, and the noise-to-signal ratio for downstream consumers is
poor — they want "the binary" not "every translation unit."

**Lean.** Default off. The data is on the wire; opt-in via
`attest.granularity.include_objects = true`. Matches what
`01-requirements.md` CR-9 says.

---

## OQ-PROC-1: Publisher process shape

**Question.** Does the Crayon-side publisher run in-process
inside the warehouse daemon, or as a standalone launchd-managed
service?

**Why it matters.** Crash isolation, entitlement granularity,
restart cadence, signing-time process identity.

**Options.**
- **In-warehouse.** Lower setup cost; tight coupling.
- **Standalone** (`com.crashoverride.crayon.svc.attest-
  publisher`). Crash-isolated; can be entitled separately;
  separate signing identity if desired.

**Lean.** In-warehouse for v1, extract if any of (separate
entitlements, crash isolation, distinct update cadence) becomes
load-bearing. Code is structured (`src/attest/`) so extraction
is a launchd-plist change, not a refactor.

**Blocked who.** Launchd plists, install scripts, the
auto-memory note about `scripts/release.sh`.

---

## OQ-INTERP-1: Signed attestations for interpreter invocations

**Question.** Do we emit signed in-toto Statements for
`interp.end` events (one per `python foo.py` / `node main.js`
invocation), or only for build-artifact / container events?

**Why it matters.** Interpreter invocations dwarf build events
in volume. Signing them all is expensive; not signing them
leaves the existing `crayon attest` human-render path as their
only surface.

**Options.**
- Sign every `interp.end`. High volume; consumers can filter.
- Sign only "ai-spawned" interpreter invocations (already
  classified by Crayon). Smaller volume, sharper signal.
- Don't sign any. Interpreter events stay human-render-only.

**Lean.** Don't sign in v1. Revisit once we hear what consumers
actually want from interpreter attestation.

---

## OQ-LAYER-1: Layer-walk attribution in v1

(Discussed in `03-crayon-integration.md` §4.3.)

**Lean.** Ship layer-walk attribution under size caps (< 64
layers, < 1 GiB uncompressed). Above caps, emit coarse rollup
and signal the omission via warehouse event. Phase 2 brings
buildkit-frontend integration for cap-free attribution.

---

## OQ-MULTIARCH-1: Multi-arch image attestation strategy

**Question.** Does v1 attest multi-arch images at the
image-index level, the per-manifest level, both, or neither?

**Options.**
- Per-manifest only — verifier picks the right arch.
- Index-level rollup only — easier verifier UX, loses per-arch
  granularity.
- Both — most informative, most pushes.

**Lean.** Per-manifest only in v1. Index-level rollup is a Phase
2 feature once we see how customers consume multi-arch
verifications.

---

## OQ-INTRA-SESSION-PUSH-1: Push timing

**Question.** Do we push per-artifact attestations *during* a
build session (as each artifact finalizes), or batch until
`build.session_summary`?

**Why it matters.** Pushing during-session means a long build
(hours) has its early artifacts pushed early; verifiers waiting
for early outputs don't block on the whole session. Pushing
during-session also means the rollup's `resolvedDependencies`
can confidently reference already-pushed artifacts.

**Tension.** A build that fails mid-way under "push during"
leaves the registry holding per-artifact attestations for an
incomplete build with no rollup binding them. Verifiers might
trust them anyway — they're individually valid — but the
context is incomplete.

**Lean.** Push during-session, emit `attest.session_aborted` on
warehouse if the session fails mid-way so consumers can correlate.

---

## OQ-PATH-1 / OQ-AUGMENT-1: (RETIRED)

The in-container file path bikeshed and the
"don't-augment-image" opt-out were both load-bearing in the
Draft-0 design that injected a synthetic image layer. The
Draft-1 design carries identity in per-binary chalk marks; no
image augmentation, no path to bikeshed. Retired.

---

## OQ-INTERCEPT-1: ld AUTH_CLOSE deadline budget

**Question.** Can the mark + sign cycle reliably fit inside the
ES AUTH_CLOSE response deadline?

**Why it matters.** If we miss the deadline, the kernel
proceeds without our response and the linker's output closes
unmarked; in the worst case the ES extension is treated as
unresponsive and gets disabled. The deadline is configurable
via `es_subscribe_deadline_set` but has a kernel-enforced
floor; sign-against-Vault under exponential backoff can blow
any reasonable setting.

**Mitigation candidates.**

1. **Pre-resolve the signer at session start** so the
   per-AUTH_CLOSE call is just sign-bytes (typically tens of ms
   for Keychain / file backends).
2. **Allow the close**, mark the binary in-place asynchronously
   using a sidecar mark or a deferred re-mark — accept that the
   binary briefly exists unmarked. Forfeits the "downstream
   codesign sees marked bytes" property; only acceptable for
   builds where codesign isn't the next step.
3. **Spool the marked-binary work** and rely on the customer
   not codesigning until the spool drains. Hard to enforce.

**Lean.** Option 1 as the primary mitigation; option 2 as the
fallback when signer backend is unreachable. The fallback
generates an `attest.deferred_mark` warehouse event so the
operator knows mark-then-sign was decoupled for this build.

---

## OQ-INTERCEPT-2: Detecting the "final" ld output

**Question.** Linkers produce intermediate files, .tmp targets,
rename-on-completion patterns, incremental-link interim states.
How do we identify "this AUTH_CLOSE is the final output that
will become the customer's binary"?

**Options.**

- Path heuristics (skip `.tmp`, `.partial`, etc.).
- Wait for `AUTH_RENAME` and use that as the "final" signal
  instead.
- Use the link-driver process tree: identify the cc-driver's
  `-o <path>` argv at exec time, only mark the close on that
  specific path.

**Lean.** The argv-derived approach. It's deterministic and
doesn't depend on linker-implementation-specific filename
conventions. The classifier already gives us actor argv.

---

## OQ-INTERCEPT-3: ld intercept vs codesign wrap, primary?

**Question.** Of the two intercept points
(`03-crayon-integration.md` §1.1 vs §1.2), which is the
default?

**Lean per `03-…` §1.3.** ld intercept primary on both macOS
and Linux; codesign wrap as opt-in on macOS for customers who
prefer the "seal moment" semantics.

---

## OQ-MARK-RECONCILE-1: Sidecar vs binary re-mutation at quiesce

**Question.** The placeholder-digest pattern at AUTH_CLOSE time
needs a reconcile step when the real envelope is signed later.
Sidecar (`<artifact>.chalk`) or in-binary re-mark?

**Why it matters.** Sidecar produces a sibling file the
customer didn't ask for. In-binary re-mark mutates the binary
a second time — and if the customer's codesign happened in
between, we've invalidated their signature.

**Lean.** Sidecar. Documented as "Crayon may write a `.chalk`
sibling next to your binary post-build." The in-binary mark
carries a fixed-format placeholder digest the verifier knows
to treat as "look in the sidecar." Both libchalk's `_certs`
and `_sidecar` codecs already do this for codec-immutable
artifact kinds; this extends the pattern to mutable kinds when
post-codesign mutation must be avoided.

---

## OQ-VERIFIER-DIST: Verifier distribution

**Question.** Do we ship our own verifier binary (`co attest
verify`), rely on `sigstore-python` in keyed mode, or both?

**Why it matters.** Customers running policy at admission time
need a verifier somewhere. The further upstream we can stay
in their tool choice, the better.

**Lean.** Ship our own `n00b-attest verify` (covered in
`02-architecture.md` §10) because we have to anyway for the
CLI. Document `sigstore-python` keyed-mode as a known-working
alternative. Do not maintain compatibility patches for
`sigstore-python` — let upstream choose its own path.

---

## OQ-KEY-ALG: Algorithm support in v1

**Question.** Which signature algorithms ship in v1?

**Options.**
- Ed25519 only — simplest, smallest implementation, modern
  default.
- Ed25519 + ECDSA P-256 — ECDSA needed for some hardware /
  Keychain configurations.
- All of Ed25519, ECDSA P-256, RSA-2048+ — maximally
  compatible.

**Lean.** Ed25519 + ECDSA P-256 in v1. RSA in v1.1 — we can
implement quickly if a customer asks but it's not worth blocking
v1 on.

---

## OQ-2P-SIGN: Two-party signing in v1

**Question.** Do we wire up the two-party-signoff flow (build
host signs, release engineer counter-signs) in v1, or document
it as a future use of the multi-signer envelope?

**Lean.** Document, don't wire in v1. The envelope format
supports it; the CLI and adapter don't need to. v1.1 adds a
`crayon attest cosign <envelope>` (note: lowercase, not the
binary) verb.

---

## OQ-RECOVERY: Key loss / compromise recovery

**Question.** What's the customer's story when a signing key is
lost or compromised?

**Lean.** Tied to FR-SM-5 (rotation):
- Key lost: rotate to a new key; old attestations remain valid
  forever (their verifiability is independent of current
  signer state); new attestations sign under the new key;
  signing manifest records both.
- Key compromised: rotate, then *publish a revocation* — a
  signed statement under the *new* key declaring the *old* key
  no longer trusted as of a given timestamp. Verifiers consult
  the signing manifest; attestations signed before the
  compromise timestamp remain valid, after that are rejected.
- Revocation distribution: same channels as pubkey distribution
  (offline keyring update, optional CO directory).

v1 ships rotation; revocation is v1.1.

---

## OQ-BUILD-EXTRACTION: Build/distribution shape of the n00b .o for Crayon

**Question.** How does Crayon's meson build pick up the
n00b-attest `.o`?

**Options.**
- Meson subproject (`subprojects/n00b.wrap` per `NCC.md`),
  exposing a `libn00b_attest_dep`.
- Pre-built static library shipped as part of libn00b releases,
  consumed via pkg-config.
- Pre-built object file in libn00b release artifacts,
  Crayon links directly.

**Lean.** Meson subproject — matches what `NCC.md` says is the
canonical way. Crayon's `meson.build` pulls in libn00b as a
subproject (it doesn't today; it has its own build); the
subproject exposes `libn00b_attest_dep` and Crayon links it.

**Blocked who.** Crayon's `meson.build`, libn00b's release shape.

---

## OQ-SESSION-ID-COLLISION: Op-ID stability across rotations

**Question.** Crayon's `op_id = "<root_pid>:<root_start_time_ns>"`
is the invocation identifier we surface as `invocationId` in
the SLSA predicate. Is this stable enough?

**Concern.** Two builds on two hosts can collide on `(pid,
start_time_ns)` (low probability but non-zero). For SLSA's
purposes a UUID would be cleaner.

**Lean.** Augment, don't replace. SLSA `invocationId` becomes
`<install-uuid>:<op_id>` — guaranteed unique because the
install UUID is per-host. The internal Crayon `op_id` stays
unchanged.

---

If you want any of these decided before implementation kicks off
in the next session, mark them in this file and we'll resolve
each before writing code.
