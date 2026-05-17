# n00b attestation subsystem — overview

**Status:** SPEC (STABLE)  
**Audience:** n00b + Crayon engineers, CO supply-chain stakeholders  
**Last updated:** 2026-05-16

---

## 1. The problem

We need to produce, sign, store, and verify **build attestations** for
the artifacts Crayon already observes — both individual build outputs
(executables, libraries, packages) and the OCI container images those
artifacts are eventually layered into. We need to push those
attestations to OCI registries and we need software running inside
those containers to be able to ask "what attestation describes
*me*?"

**The overriding design constraint is zero-friction setup.** Most of
our customers do not run their own PKI and have no appetite to start.
The ideal user experience is **"install Crayon, signing works"** —
the installer (or an MDM-pushed config) provisions the signing key,
nothing more is required of the user. A `setup` command exists as a
secondary surface for the cases the installer can't auto-handle
(provisioning into external secret managers, rotation, opt-out
scenarios), but it is a fallback, not the on-ramp. Every design
decision below is filtered through that lens; if a feature makes
the default path harder or requires running an additional service,
we default to rejecting it.

Crayon already produces the *data* — see
`include/private/build_lifecycle/protocol.h`
(`CRAYON_BUILD_EVENT_ARTIFACT`, `CRAYON_BUILD_KEY_FILE_HASHES`,
`CRAYON_BUILD_KEY_PACKAGES_INSTALLED`, `CRAYON_BUILD_KEY_PRODUCING_CHAIN`,
etc.). What we lack is the **signed, distributable attestation
envelope** that downstream consumers (registries, policy engines,
verifying clients) expect.

Today the industry-standard glue for that is the sigstore/cosign
stack. We can't use it as-is.

## 2. What we keep and what we drop from the Sigstore stack

The Sigstore ecosystem is not one thing — it's a stack of formats
plus a set of services plus a CLI. We adopt the open formats and
the OCI storage shape; we drop the services and the CLI. "Sigstore
compat" for this project means *a Sigstore-aware verifier that is
configured for keyed verification can fetch and validate our
attestations*. It does **not** mean "drop in for cosign."

| Component | Decision |
|---|---|
| **in-toto Attestation Framework** v1 (Statement schema: subject + predicate) | **Adopt.** Open spec, not Sigstore-specific. |
| **DSSE** (Dead Simple Signing Envelope) | **Adopt.** Open spec, not Sigstore-specific. |
| **SLSA Provenance** v1 predicate | **Adopt.** Open spec. |
| **Crashappsec SBOM predicate** (CO-native, optionally embedding full per-component build provenance) | **Adopt — we own this.** See `03-crayon-integration.md`. |
| **OCI 1.1 Referrers API** (registry-layer storage and discovery) | **Adopt.** Open spec; modern registries support it. |
| **Fulcio** (OIDC → short-lived X.509 cert) | **Drop in v1.** Revisit if a customer asks for OIDC-bound signing identity. |
| **CTLog** (Certificate Transparency log Fulcio writes into) | **Drop.** Falls out of the Fulcio decision. |
| **Rekor** (signature transparency log) | **Drop entirely.** Not even an opt-in flag. |
| **TUF root bootstrap** (Sigstore's trust-root distribution) | **Drop.** Trust comes from secret-manager / MDM-delivered public keys, not a parallel PKI. |
| **`.sigstore` bundle format** (DSSE + cert + Rekor proof, in one file) | **Drop.** Our on-wire shape is DSSE envelope inside an OCI 1.1 referrer manifest. We can produce a bundle wrapper later if a specific verifier insists, but it is not the default. |
| **`.sig` / `.att` tag fallback** (pre-OCI-1.1 cosign convention) | **Drop.** Targets we care about (ECR, ACR, Artifact Registry, GHCR, Harbor, zot) all support referrers. |
| **`cosign` the binary** | **Drop.** We ship our own. cosign-compat as an interop bench is not a goal. |

## 2.1 Why these specific deviations

**Rekor.** The piece our customers most loudly object to. cosign's
default UX is built around the assumption that every signature
lands in a public transparency log. That is hostile to our use case
for three reasons:

1. **Confidentiality.** Every signed artifact's digest, signer
   identity, predicate type, and timestamp becomes a public,
   append-only record. Even with redaction, the metadata leaks.
2. **Availability.** Mandatory-tlog-inclusion makes Rekor a single
   point of failure for any sign-or-fail build pipeline. The
   public instance is operationally fragile; running a private
   Rekor is heavy and nobody we sell to wants to.
3. **Threat-model mismatch.** Rekor exists to make signer
   equivocation publicly detectable across mutually-distrusting
   parties. Our customers have a single signer identity per
   project and a known verifier set; the log adds operational
   pain without security uplift for them.

**Fulcio + CTLog.** Fulcio issues 10-minute X.509 certs whose SAN
embeds an OIDC identity. The short cert lifetime presumes a
transparency log records the signing moment — without Rekor, the
cert is just expired by the time anyone verifies, so the workflow
falls apart. The remaining value of Fulcio without Rekor would be
"no long-lived secret on the build runner" — but secret-manager-
fetched-at-runtime gives us the same property with far less
machinery and no OIDC dependency. CTLog falls out of dropping
Fulcio.

**TUF.** Sigstore's trust root is itself published via a TUF repo,
which then tells you which Fulcio / Rekor / CTLog instances to
trust. Customers who don't want a parallel PKI also don't want a
parallel trust-root rotation calendar. Trust for us comes from
public keys delivered by the same mechanism that already delivers
their other secrets (OS keychain, 1Password, Vault, MDM profile,
etc.).

**`.sigstore` bundle.** The bundle's verification-material section
assumes Rekor inclusion proofs are present and most off-the-shelf
verifiers complain or reject without them. It's a wrapper around a
wrapper around a wrapper, and the inner DSSE envelope is the only
part anyone actually needs. We emit the inner thing.

**`.sig` / `.att` tag fallback.** Pre-OCI-1.1 way of attaching
signatures to images — write a sibling tag named `sha256-<digest>
.sig`. Pollutes the registry namespace and creates tag-cleanup
problems. OCI 1.1 referrers replace this and the modern registry
ecosystem has caught up. We require referrers and tell anyone
running a non-conforming registry to update.

**cosign the binary.** Go, opinionated, defaults bake in
Rekor + Fulcio + TUF. Producing envelopes a cosign verifier in
keyed-no-tlog mode (`--insecure-ignore-tlog --key publickey.pem`)
will validate is *easy* and not really cosign-specific — anything
that speaks DSSE + in-toto will validate the same envelope. We do
not adopt cosign-as-canonical-verifier and we will not work hard
to keep its specific CLI quirks happy.

## 2.2 What "Sigstore-compatible" means in scope

The position is **DSSE + in-toto + OCI 1.1 at the format layer**.
A Sigstore-aware verifier (sigstore-python in keyed mode, the
Sigstore policy controller configured for keyed verification, any
in-toto-attestation-validation library, etc.) can:

- Fetch our envelope from the registry via the standard
  `/v2/<name>/referrers/<digest>` endpoint.
- Parse it as DSSE → in-toto Statement v1.
- Validate the signature against a public key it already trusts.

What it cannot do, by design:

- Validate a Rekor inclusion proof (there isn't one).
- Validate a Fulcio-issued cert (we don't issue from Fulcio).
- Fetch a TUF root from us (we don't run one).

If a Sigstore-aware consumer rejects the envelope for missing
those pieces, they're enforcing a policy we explicitly opted out
of. The fix is on their side: configure for keyed-no-tlog
verification.

## 2.3 Credential delivery: secret management, not PKI

The model is **long-lived signing key held in a secret manager,
provisioned at install time, fetched at sign time.** Concretely:

- **Install is the on-ramp.** Crayon's installer auto-generates a
  keypair (or adopts an MDM-pre-provisioned one), writes the
  private half to the local default backend, emits the public
  half. After install, signing works with no user action.
- **Default backend on macOS**: per-user Keychain item under
  service name `com.crashoverride.n00b-attest.signer`. (We
  default to per-user because Crayon today runs per-user on
  macOS; service-account on shared build hosts is an install-
  time override.)
- **Default backend on Linux** (forthcoming, with Crayon-Linux):
  TBD between per-user libsecret, per-user mode-0600 file, and
  per-host system keystore. See `05-open-questions.md` OQ-LX-1.
- **Other supported backends** (used when explicit URI in config
  or env): 1Password (huge enterprise penetration), HashiCorp
  Vault, AWS / GCP / Azure secret managers, env-var-direct
  (`N00B_ATTEST_SIGNER_KEY=...` for one-shot CI jobs), generic
  file path. Backends are addressed by URI scheme so callers
  don't have to know which library is doing the resolution.
- **Discovery chain**: at sign time the library walks
  `env-var-direct → env-var-URI → project config → user config →
  host config (MDM slot) → OS keychain → fail with structured
  error`. The chain is configurable; defaults are picked so an
  installer-provisioned host just works without any further
  config.

We do **not** ask customers to:

- Run a CA, issue certs, or rotate certs on a calendar.
- Stand up a Fulcio, a Rekor, a Vault, or any other service we
  don't already integrate with as a client.
- Maintain a TUF root.
- Distribute trust roots through a parallel channel from how they
  already manage secrets.

Public-key distribution to verifiers (separate problem from
private-key custody) is covered in `02-architecture.md`. The
short version: image annotation carries a key fingerprint;
verifiers resolve the fingerprint via either an MDM-managed local
keyring or an optional CO-hosted public-key directory; offline /
air-gapped verification ships the keyring bundled.

## 3. Where this lives

This is a **module of libn00b**, per D-017. It lives as a `jj`
workspace at `~/n00b-attest/` of the `~/n00b/` repo, following
the same convention as `libn00b_chalk` (whose workspace is at
`~/n00b-chalk/`). The module's source code lives inside n00b's
tree at `include/attest/...` and `src/attest/...`. The build is
n00b's existing tooling (meson + `scripts/build.sh`).

Three modules, one repo:

- **`n00b_attest`** (this module) — in-toto Statement
  construction, DSSE envelope sign/verify, signer abstraction
  with secret-manager backends, OCI registry client, pubkey
  resolver, verifier surface, CLI.
- **`libn00b_chalk`** (existing module) — codec-specific
  in-band marking (ELF / Mach-O / PE / wrappers / scripts /
  sidecars), mark construction, unchalked-hash computation,
  re-sign helpers. The `ATTESTATION` field in libchalk's mark
  format is the slot where our signed DSSE envelope (or its
  digest + registry hint) lives.
- **`libn00b`** (existing) — runtime, crypto primitives,
  allocator machinery, JSON / dict / result types.

No `subprojects/n00b.wrap` or `subprojects/n00b_chalk.wrap` —
all three are in the same repo. Symbols are prefixed
`n00b_attest_*` per D-015.

**Separation of concerns:** marking is libchalk's job;
attestation envelope construction, signing, and distribution
is n00b_attest's job. libchalk does not learn about DSSE or
OCI; n00b_attest does not learn about ELF or Mach-O. The
unchalked SHA-256 that libchalk computes for a binary is
exactly the value n00b_attest uses as the in-toto Statement's
`subject.digest.sha256`, which is the join point.

The Crayon side is a **thin adapter** (in the Crayon tree)
that subscribes to `build.artifact` / `build.session_summary`,
hooks ES `AUTH_CLOSE` on linker output for the in-band
mark-and-sign moment, builds the Statement, signs the DSSE
envelope, calls libchalk's insert to embed it in the binary,
and pushes the envelope to a registry. Crayon does not own any
of the cryptography, the envelope format, the registry
transport, or the byte-level marking.

n00b_attest is built with `ncc`; Crayon is built with stock
Apple clang. The integration surface is therefore a **plain-C
ABI** header set (`n00b_attest_cabi.h`), no `_kargs` /
`+`-varargs / `r"..."` rich-string usage in the exported
symbols. ncc-compiled translation units link cleanly into
clang-compiled callers as long as the headers they cross stay
within plain C23. We are strict about this boundary.

## 4. What this project ships, in priority order

1. **`n00b_attest` library**, a standalone n00b project providing
   in-toto + DSSE + signing + registry surface, plus the chalk
   integration wrapper that builds an ATTESTATION-shaped tree and
   delegates byte-level marking to libchalk.
2. **Crayon adapter**, wiring ES `AUTH_CLOSE` on `ld` output
   (and, optionally, `AUTH_EXEC` on `codesign`) into mark + sign,
   plus warehouse subscription on `build.artifact` and
   `build.session_summary` for the reconcile + push half.
3. **Container rollup synthesis** (Crayon-side): walk the pushed
   image's layers post-build, harvest chalk marks from every
   file, build one container-level in-toto Statement whose
   subject is the image manifest digest and whose
   resolvedDependencies cite the per-binary marks. No image
   augmentation; the rollup is a separate registry artifact
   referencing the image.
4. **In-container identity** surface: a tiny self-introspection
   library customers link into their applications. Reads the
   running binary's own chalk mark via Mach-O `LC_NOTE` / ELF
   `.note` / PE resource walking; returns the embedded
   attestation envelope (or its digest + registry hint).
   Distributed as either a single C source file with no libn00b
   dependency, or pre-built `.a`s for the common platform / libc
   combinations.
5. **Install-time credential provisioning** (default path):
   Crayon's installer generates the signing key and writes it to
   the local default backend (macOS Keychain; Linux backend TBD
   with the forthcoming Crayon-Linux work). MDM may pre-provision
   a key, in which case the installer adopts it instead.
   `n00b-attest setup` exists for the secondary cases —
   provisioning into a remote secret manager (AWS Secrets Manager,
   Vault, 1Password, etc.), key rotation, or onboarding a
   customer who opted out of install-time generation. We do
   **not** require customers to run a CA or any other service.

## 5. Non-goals (for this project)

- **Verification at admission time.** This project ships the
  ability to *verify* attestations as a library primitive, but
  building the policy engine that decides "is this image allowed
  to run?" is out of scope. That's downstream consumer work.
- **Replacing SBOM tooling.** We emit SBOM-shaped predicates
  inside in-toto Statements based on the per-artifact data Crayon
  already collects. We do not aim to compete with syft / trivy /
  grype as a generic SBOM tool.
- **Cross-tenant key management.** The default model is one
  signer identity per OS user. Anything multi-tenant on a single
  user account is out of scope.
- **Running any new service ourselves as a hard dependency.** An
  optional CO-hosted public-key directory is in scope (verifier
  convenience, not a requirement). A required hosted service is
  out of scope.
- **Linux parity for the Crayon side.** Crayon today runs on
  macOS via ES. The `n00b_attest` library is portable and is part
  of libn00b's general portability story; the *Crayon adapter*
  ships only where Crayon ships.
- **Re-implementing Fulcio or Rekor.** We can *talk to* a Fulcio
  for the optional OIDC-issued-cert flow, but we don't run one and
  we definitely don't run a transparency log.

## 6. Reading order

1. This file — what and why.
2. `01-requirements.md` — functional + non-functional requirements,
   numbered so other docs can cite them.
3. `02-architecture.md` — the n00b_attest library itself: module
   layout, API surface, signing backends, registry client, chalk
   integration entry points.
4. `03-crayon-integration.md` — how Crayon calls in: the ld /
   codesign intercept, the SLSA Provenance mapping, the
   container rollup synthesis from harvested marks, SBOM
   emission.
5. `04-in-container-identity.md` — the in-process self-
   introspection surface (binary reads its own chalk mark).
6. `05-open-questions.md` — things we're explicitly deferring or
   that need a decision before implementation starts.

## 7. Terminology

- **Attestation** — a signed claim about an artifact. In this
  project, always wrapped in a DSSE envelope carrying an in-toto
  Statement v1 payload.
- **Subject** — the artifact(s) the attestation is *about*.
  Identified by name + content digest.
- **Predicate** — the actual claim. In this project the common
  predicates are SLSA Provenance v1 (for build attestations) and
  a Crayon-specific SBOM predicate (TBD whether we adopt CycloneDX,
  SPDX, or a Crayon-native shape; see open questions).
- **Statement** — the in-toto v1 JSON object combining subject(s) +
  predicate. Goes inside the DSSE envelope.
- **Envelope** — the DSSE wrapper carrying the signature(s) and
  base64-encoded payload.
- **Referrers** — the OCI 1.1 mechanism by which an artifact
  manifest (an attestation, in our case) declares which other
  manifest (the image, in our case) it refers to. The registry
  exposes a `/v2/<name>/referrers/<digest>` endpoint that lists
  every manifest pointing at a given subject digest. This is how
  attestations are discovered without baking digests into the image.
- **Container rollup** — the synthesis step where Crayon
  recognizes that an active build session produced an OCI image,
  gathers the per-artifact attestations whose subjects are now
  layered into that image, and emits one image-scoped attestation
  pointing at all of them.
