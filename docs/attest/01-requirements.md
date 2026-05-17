# n00b attestation subsystem — requirements

**Status:** SPEC (STABLE)  
**Last updated:** 2026-05-16

Requirements are numbered (FR-* functional, NFR-* non-functional,
CR-* Crayon-integration) so downstream design docs and PRs can cite
them. "MUST / SHOULD / MAY" used in the RFC-2119 sense.

---

## 1. Functional requirements — core n00b subsystem

### Formats and standards

- **FR-1.** The subsystem MUST produce in-toto Attestation Framework
  v1 Statements (`_type` = `https://in-toto.io/Statement/v1`).
- **FR-2.** The subsystem MUST wrap signed Statements in DSSE
  envelopes (`payloadType` = `application/vnd.in-toto+json`).
- **FR-3.** The subsystem MUST support SLSA Provenance v1
  (`predicateType` = `https://slsa.dev/provenance/v1`) as a
  first-class predicate type, with constructors that map cleanly
  from Crayon's `build.artifact` and `build.session_summary` event
  shape.
- **FR-4.** The subsystem MUST support an SBOM-shaped predicate
  type. Final shape is an open question (see
  `05-open-questions.md`, OQ-2): either an industry-standard
  predicate type (CycloneDX 1.6 in-toto wrapper, SPDX 3 in-toto
  wrapper) or a Crashappsec-specific predicate. Whichever we
  choose, the choice MUST be a runtime selection, not a build-time
  one — we will likely emit more than one shape.
- **FR-5.** The subsystem MUST accept user-defined predicate types
  (caller supplies the `predicateType` URI and the predicate JSON
  blob). This keeps us flexible for predicates we haven't
  enumerated yet (VSA, vuln-scan results, custom Crashappsec
  predicates).

### Signing — keys, not certs, by default

The primary signing model is **a long-lived signing key held in a
secret manager**. The library never asks the customer to run a CA,
issue certs, or rotate them on a calendar. See `00-overview.md`
§2.3 for the design rationale.

- **FR-6.** The subsystem MUST support **raw-key signing** as the
  default path, with at minimum Ed25519. SHOULD also support
  ECDSA P-256 and RSA-2048+ for ecosystem compatibility (some
  customer HSMs / Keychain configurations only support
  ECDSA / RSA).
- **FR-7.** The subsystem MUST support **X.509-chain signing** as
  a secondary path for customers who *do* have an internal PKI
  and want signing identity = a CA-issued cert. Caller supplies a
  leaf cert + key (potentially + intermediates); envelope carries
  the cert chain in the DSSE `signatures[].cert` field. This is
  the "use what you already have" mode; we do not force it on
  anyone who doesn't.
- **FR-8.** The subsystem MUST NOT integrate with Fulcio in v1.
  (Reconsider if a customer specifically asks for OIDC-bound
  signing identity. Locally — i.e. dev machines with already-
  authenticated work-account OIDC — there is a Phase-2 story for
  fetching a long-lived key from a hosted vault using the OIDC
  token; not v1.)
- **FR-9.** The subsystem MUST NOT include Rekor (or any
  transparency log) integration. Not as default, not as opt-in,
  not as a flag. Removing the config surface removes the
  possibility of someone turning it on by mistake.
- **FR-10.** The subsystem MUST support **detached signatures** —
  the signed DSSE envelope is the artifact, no implicit ambient
  state required to verify.
- **FR-11.** The subsystem MUST support **multi-signer envelopes**
  per the DSSE spec (`signatures[]` array). Useful for two-party
  signoffs (build host + release engineer).

### Signing — secret-manager backends and discovery

- **FR-SM-1.** The subsystem MUST address signing-key locations
  via URI scheme so callers don't need to know which backend
  resolves the URI. Required schemes in v1:
  - `keychain:<service-name>` — macOS Keychain (per-user by
    default; `keychain-system:` variant for the system keychain).
  - `file:<absolute-path>` — PEM-encoded key on disk, mode 0600
    enforced.
  - `env:<varname>` — PEM-encoded key directly in env (one-shot
    CI use); contents zeroized after read.
  - `op://<vault>/<item>/<field>` — 1Password CLI (`op` binary
    must be on `$PATH`; v1 shells out, v2 may use the 1P SDK).
  - `vault:<host>/<path>` — HashiCorp Vault (KV-v2 by default).
  - `aws-sm:<region>/<secret-name>` — AWS Secrets Manager.
  - `gcp-sm:<project>/<secret-name>` — GCP Secret Manager.
  - `az-kv:<vault-name>/<secret-name>` — Azure Key Vault.
- **FR-SM-2.** The subsystem MUST support a **discovery chain** at
  sign-time so callers usually don't have to specify any URI.
  Default chain order:
  1. Env var `N00B_ATTEST_SIGNER_KEY` (PEM bytes inline).
  2. Env var `N00B_ATTEST_SIGNER_REF` (URI).
  3. Per-project config `./.n00b-attest.json` `.signer.ref`.
  4. Per-user config
     `$XDG_CONFIG_HOME/n00b-attest/signer.json` `.ref`.
  5. Per-host config `/etc/n00b-attest/signer.json` `.ref` (the
     MDM-managed slot).
  6. OS-default keychain item under service name
     `com.crashoverride.n00b-attest.signer` for the running user.
  7. Fail with a structured error that names every link in the
     chain it tried.
  Order of the chain MUST be overridable per call.
- **FR-SM-3.** Backends MUST implement a small uniform
  interface — "load private key bytes," "describe origin for
  audit logging," "zeroize on release." Backends that can sign
  without releasing key bytes (Keychain `SecKey` references,
  HSM PKCS#11 handles) MUST be able to take that path so private
  key bytes never leave the secure boundary. New backends MUST
  be addable without recompiling the library (out-of-tree plugin
  shape, see `02-architecture.md`).
- **FR-SM-4.** The **default credential-provisioning path is the
  install, not a user command.** Crayon's installer (and the
  forthcoming Crayon-Linux installer) MUST:
  1. Check whether an MDM / config-management layer has already
     pre-provisioned a signing key at the expected URI. If yes,
     adopt it; record the URI in `/etc/n00b-attest/signer.json`
     (or the per-user equivalent); done.
  2. Otherwise, generate a keypair (default Ed25519), write the
     private half to the local default backend (macOS Keychain;
     for Linux, per the Linux-Crayon backend choice — see
     OQ-LX-1 in `05-open-questions.md`), and record the URI.
  3. Write the public half to a known on-host location
     (`/etc/n00b-attest/local-pubkey.pem`) and emit it in the
     installer's post-install report so the admin can publish
     it to verifiers.
  4. If the optional CO-hosted public-key directory is enabled
     in the install configuration, upload the pubkey to it.
  After install, signing "just works" — no command for the user
  to run. The end-state is symmetric: a host provisioned via
  MDM, via a fresh install, or by an admin who ran `setup`
  manually all converge on the same local config shape.
- **FR-SM-4a.** `n00b-attest setup` MUST exist as a secondary
  surface for the cases the installer genuinely can't auto-
  handle: provisioning a key into an external secret manager
  (`--backend aws-sm:...`, `--backend vault:...`, etc.),
  re-provisioning after a wipe, or onboarding a customer who
  intentionally opted out of install-time generation. It is a
  fallback, not the on-ramp.
- **FR-SM-5.** A symmetric **rotation** primitive MUST exist:
  `n00b-attest rotate` generates a new keypair, writes it under a
  new URI / Keychain item, and emits a *signing manifest* that
  lists active + retired keys with their validity windows.
  Verifiers consult the manifest to decide which key was active at
  attestation time.
- **FR-SM-6.** Public-key distribution MUST work in two modes:
  - **Offline / bundled.** Public keys (and any signing-manifest
    history) ship as a file the verifier consults locally. The
    canonical shape is a JSON keyring; the verifier accepts a
    directory of these and treats them as union. This is the
    air-gap-friendly mode.
  - **Hosted directory (optional).** An optional CO-hosted
    service maps `signer_id` (or pubkey fingerprint) to pubkey +
    rotation history. Verifiers MAY use this for convenience;
    it is never a required dependency for either signing or
    verification. The hosted directory is a roadmap item, not a
    v1 blocker; v1 ships with offline mode only.

### Verification

- **FR-12.** The subsystem MUST verify DSSE envelopes against:
  raw public keys, X.509 trust roots (with optional CRL / OCSP
  hooks deferred — see open questions), and Fulcio CT-issued
  certs with caller-supplied trust roots.
- **FR-13.** The subsystem MUST verify subject digest match
  before treating an attestation as applicable to a candidate
  artifact.
- **FR-14.** The subsystem MUST expose verification as a pure
  library function with no network dependency for the default
  path (raw-key and X.509). Fulcio-cert verification MAY require
  network only for chain-resolution if intermediates are not
  bundled.

### Storage and transport — OCI registries

- **FR-15.** The subsystem MUST be able to **push** an
  attestation envelope to an OCI v2 registry as an OCI 1.1
  artifact manifest, with the `subject` field pointing at the
  attested image's manifest digest. This is the "referrers"
  mechanism.
- **FR-16.** The subsystem MUST be able to **discover** existing
  attestations for a subject via the registry's
  `/v2/<name>/referrers/<digest>` endpoint, including pagination
  and `artifactType` filtering.
- **FR-17.** The subsystem MUST be able to **pull** an
  attestation envelope by digest.
- **FR-18.** The subsystem MUST support these registry auth
  shapes: anonymous, HTTP Basic with username/password, bearer
  token (token-exchange flow per the OCI distribution spec),
  Docker-credential-helper invocation. SHOULD support
  Apple-Keychain-backed credentials when running on macOS.
- **FR-19.** Push and pull MUST work against at minimum these
  reference implementations: ghcr.io, Docker Hub,
  GitLab Registry, AWS ECR, GCP Artifact Registry, Azure ACR,
  Harbor, zot. Compatibility is tracked per-registry in a
  matrix in the test suite.
- **FR-20.** The subsystem MUST support pushing attestations as
  *standalone* artifacts even when the registry does not
  implement OCI 1.1 referrers (fall back to the cosign-tag
  convention `<digest:.->-.sig` / `.att`). When we fall back,
  we MUST log a warning visible to the build operator.

### Allocator and lifecycle

- **FR-21.** Every `n00b_attest` API that allocates MUST accept an
  optional `.allocator` keyword argument per the n00b convention
  (default `nullptr` → runtime allocator). No internal allocator
  bypass — including for "tiny" allocations like signature
  scratch buffers. This is the recurring failure mode the team
  has called out and must be guarded both in code review and in
  a dedicated test pass.
- **FR-22.** The subsystem MUST be **arena-friendly**: a caller
  can pass a single arena allocator into `n00b_attest_sign(...)`
  and free the whole arena once the envelope bytes are extracted.
  This implies no hidden global caches, no `[[gnu::constructor]]`
  state that holds caller allocations.
- **FR-23.** The C-ABI surface (for non-ncc callers like Crayon)
  MUST flatten `_kargs` blocks into explicit parameter
  structures. Callers fill in a struct and pass it by pointer.
  No `_kargs` syntax in headers Crayon includes.

### CLI / dev tooling

- **FR-24.** A `n00b-attest` CLI MUST exist for: inspect (decode
  envelope + dump payload pretty-printed), verify (envelope +
  trust root → pass/fail), sign (predicate JSON file + signer
  config → envelope on stdout), discover (registry + subject
  digest → referrer list), push, pull. This is the human
  surface; Crayon uses the library directly.

---

## 2. Crayon-integration requirements

### Producer side — per-artifact

- **CR-1.** Crayon MUST emit one in-toto Statement per
  `build.artifact` event whose `artifact_kind` is one of
  `executable`, `static_lib`, `shared_lib`, `package`.
  `object`-kind artifacts are aggregated into their consumer's
  attestation by default but MAY be emitted independently if a
  policy flag asks for it (see CR-9).
- **CR-2.** The Statement's `subject` field MUST be
  `{name: <artifact_path basename>, digest: {sha256: <hex>}}`,
  with the hash taken from `CRAYON_BUILD_KEY_FILE_HASHES` for
  that path. If the file hash is absent (file too large, not
  regular, unreadable), the Statement MUST be suppressed and a
  warning logged — we do not emit attestations we can't make
  verifiable claims about.
- **CR-3.** The predicate MUST be SLSA Provenance v1, with the
  following mapping from Crayon's `build.artifact` payload to
  SLSA fields:
  - `predicate.buildDefinition.buildType`: a Crashappsec-owned
    URI naming the toolchain mode, e.g.
    `https://schemas.crashoverride.com/crayon/build-type/cc-link/v1`.
    Distinct URIs per artifact kind / op chain.
  - `predicate.buildDefinition.externalParameters`: the root
    `build_tool_argv` + `build_tool_cwd` + `stdin_kind`.
  - `predicate.buildDefinition.internalParameters`: the producing
    op chain (`compile_operations` for direct producers).
  - `predicate.buildDefinition.resolvedDependencies`: every entry
    from `source_files`, `header_files`, `object_inputs`,
    `library_inputs`, `other_inputs`, with their SHA-256 from
    `file_hashes`. Each entry carries a `name` (the path) and a
    `digest`.
  - `predicate.runDetails.builder.id`: the host identity URI
    (TBD shape — see OQ-3). At minimum includes the Crayon
    install's unique installation ID + the build host hostname.
  - `predicate.runDetails.metadata.invocationId`: the Crayon
    `op_id` (`<root_pid>:<root_start_time_ns>`).
  - `predicate.runDetails.metadata.startedOn` /
    `finishedOn`: from `started_ns` / `completed_ns` (converted
    to RFC3339).
  - `predicate.runDetails.byproducts`: `package_contributions` +
    `network_requests` + (if non-empty) `sub_processes`.
- **CR-4.** The Crayon-side adapter MUST NOT lose information.
  Anything Crayon observed about the build that doesn't fit a
  standard SLSA field is placed under a Crashappsec-owned
  extension key (TBD, e.g.
  `predicate.runDetails.byproducts[].crashoverride:crayon_raw`).
  Downstream consumers that don't know about the extension see
  it as opaque JSON; we never silently drop data.

### Producer side — session-level / container

- **CR-5.** When a `build.session_summary` event corresponds to
  a session whose produced artifacts include one or more
  `artifact_kind == container_image`, Crayon MUST emit one
  **container-rollup attestation** per image: an in-toto
  Statement whose `subject` is the image manifest digest,
  whose predicate is SLSA Provenance v1, and whose
  `resolvedDependencies` cite the per-binary chalk marks
  harvested from the image's layers.
- **CR-6.** "Per-binary chalk marks" are harvested by walking
  the image's pushed layers post-build: for each file in each
  layer, run `n00b_chalk_extract_buffer`; files with a mark
  contribute one entry citing the path-in-layer, the file's
  unchalked SHA-256 (from the mark's `HASH` field), and the
  attestation envelope digest (from the mark's `ATTESTATION
  .envelope_digest`). Files without a mark are surfaced in an
  `unchalked_contents` byproduct of the rollup Statement.
- **CR-7.** Per-artifact attestation envelopes MUST be pushed
  to the same registry as the image, before the rollup itself.
  Each per-artifact envelope is its own OCI 1.1 referrer
  manifest against the artifact's unchalked-SHA-256 subject
  (not against the image — they're attached to the artifact's
  identity, which is independent of which images it ends up in).
- **CR-8.** Container rollup attestations MUST NOT modify the
  built image. No synthetic layers, no augmentation, no
  alternate-digest publication. The rollup is a separate
  referrer manifest in the registry against the image's actual
  pushed digest.
- **CR-9.** The Crayon side MUST support a policy flag to opt
  into object-file granularity attestations. Default is off
  (the noise-to-signal ratio is poor for `.o` files in most
  projects) but the data is on the wire and the attestation
  builder can produce them.

### Signing and policy

- **CR-10.** The Crayon adapter MUST NOT have its own opinion on
  signing keys. It calls `n00b_attest_sign` with a
  `n00b_attest_signer_t *` opaque handle, resolved by an
  MDM-aware loader (`n00b_attest_signer_resolve_default(...)`).
  This keeps the policy in one place — see `02-architecture.md`
  §6.
- **CR-11.** The Crayon adapter MUST support **deferred signing**:
  if the signer is unavailable (MDM hasn't delivered creds yet,
  key card not present, etc.), the attestation MUST be persisted
  unsigned to a known on-disk location for a separate signing
  pass. We do not drop attestations because signing isn't ready.
- **CR-12.** The Crayon adapter MUST NOT block the build-
  lifecycle event loop on registry pushes. Pushes happen on a
  dedicated dispatch queue and are bounded by a backpressure
  policy that aligns with the existing warehouse outbound
  backpressure shape (see auto-memory `project_warehouse_memory
  _leak.md` — we do not invent a second leaky spool).

### SBOM emission

- **CR-13.** For each artifact attestation, Crayon MUST also emit
  a paired **SBOM attestation** when the artifact's input closure
  includes at least one entry from `packages_installed`. Subject
  is the same as the build attestation; predicate type is the
  selected SBOM predicate (per FR-4).
- **CR-14.** The SBOM predicate MUST list only components that
  were *actually consumed by the producer chain*, not the full
  session-wide `session_packages_installed`. This is the "no
  typical SBOM noise" requirement: a CycloneDX from syft against
  a built binary lists everything on the build host filesystem;
  Crayon knows the actually-read set and we use it.
- **CR-15.** Container-rollup attestations MUST be paired with a
  **container SBOM attestation** whose components are the union
  of components in the constituent per-artifact SBOMs, deduped
  by `(ecosystem, package, version)`. The container SBOM also
  records, per-component, which contained artifact(s) drew it in
  (back-reference list).

---

## 3. In-container identity requirements

The identity surface is **per-binary self-introspection** — a
process reads its own image's chalk mark. The previous
draft's per-image identity file (`/.crashoverride/attestation
.json`) is gone.

- **IC-1.** A running process MUST be able to learn the
  attestation envelope (or its digest + registry hint) that
  describes its own binary, using only the bytes of its own
  loaded image — no required network call, no filesystem
  read of any other path.
- **IC-2.** A library MUST be available (link-once, static,
  with **no libn00b runtime dependency** on the in-container
  side — must work in any customer container) that performs
  the self-introspection and returns a structured value.
- **IC-3.** The mechanism MUST identify the running binary's
  own image via platform-appropriate runtime calls: Mach-O
  `_dyld_get_image_header(0)` on macOS, `dl_iterate_phdr` on
  Linux, `GetModuleHandleW(NULL)` on Windows. No
  `/proc/self/exe` race, no path-based heuristic, no
  filesystem read.
- **IC-4.** Tampering MUST be detectable by an out-of-process
  verifier presented with the binary's bytes: verifier
  re-computes the unchalked SHA-256 and checks it against the
  envelope's `subject.digest.sha256`. The chalk mark's own
  HASH field provides a redundant integrity check.
- **IC-5.** Sentinel returns MUST distinguish: no chalk mark
  present (binary was never attested), chalk mark present
  with no ATTESTATION field, malformed ATTESTATION, and
  platform-lookup failure. Callers can act differently on each.

---

## 4. Non-functional requirements

### Security

- **NFR-1.** Private signing keys MUST NEVER appear in heap
  dumps, log output, or error messages. Use locked memory pages
  for in-memory key material and zeroize on free.
- **NFR-2.** When MDM delivers a key as a file, the loader MUST
  refuse to use files with mode broader than 0600 on Unix-like
  systems, and MUST warn (but proceed) when ownership doesn't
  match the running user.
- **NFR-3.** The default `n00b_attest_sign` path MUST NOT contact
  the network. This is a defense-in-depth complement to FR-9 —
  even if a misconfiguration sneaks a Rekor URL in, the default
  call shape uses raw-key signing offline.
- **NFR-4.** TLS to the registry MUST verify the certificate
  chain by default. A trust-override is provided
  (`.tls_verify = false` kwarg) for testing against local zot
  instances; the C-ABI flattened form makes the override
  explicit, not a default.
- **NFR-5.** The OCI registry client MUST be hardened against
  malicious manifests: size caps on referrer-listing pagination,
  depth caps on JSON parse, refusal to follow registry-supplied
  redirects to non-HTTPS targets.

### Performance

- **NFR-6.** Signing an in-toto envelope with a ~50 KB
  predicate MUST take < 50 ms wall-clock on an M-series macOS
  build host, including key load from Keychain.
- **NFR-7.** Pushing one attestation to a typical registry MUST
  complete in < 500 ms p50 from a build host with normal LAN
  reachability to the registry. Pushes are pipelined; the
  adapter MUST NOT serialize per-attestation pushes within a
  single session.
- **NFR-8.** The Crayon adapter's added per-event overhead MUST
  NOT regress build-lifecycle event throughput below the levels
  recorded by the existing benchmark. Concretely: the
  Statement-construction step happens off the event-handling
  serial queue.

### Compatibility

- **NFR-9.** A DSSE envelope produced by `n00b_attest` MUST be
  verifiable by an unmodified `cosign verify-attestation
  --insecure-ignore-tlog` invocation, given the matching public
  key. This is the cross-tool interop contract.
- **NFR-10.** SLSA Provenance v1 payloads MUST validate against
  the slsa.dev JSON schema.
- **NFR-11.** OCI manifests MUST validate against the OCI
  distribution spec v1.1 schema.

### Operability

- **NFR-12.** Every attestation envelope, when persisted to disk
  unsigned (per CR-11) or signed-but-undelivered, MUST be placed
  under a single root path with predictable naming so an operator
  can find / drain / replay the queue without parsing logs.
- **NFR-13.** All signer / registry-client failures MUST be
  observable on the existing Crayon warehouse bus (new event
  kinds, not log lines) so the same alerting plumbing can surface
  them.

### Testing

- **NFR-14.** End-to-end tests MUST run against a local zot
  registry brought up by the test harness. We do not test against
  the public Docker Hub / GHCR in CI.
- **NFR-15.** A `cosign verify-attestation` smoke test MUST run
  in CI against every signing backend, asserting NFR-9.
