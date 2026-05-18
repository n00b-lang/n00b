# n00b_attest library — architecture

**Status:** SPEC (STABLE)  
**Last updated:** 2026-05-17

This doc covers the **library itself**: how the n00b_attest
package is structured, what the public surfaces look like, how
it gets built, and how Crayon links it. Crayon-specific wiring
(event-to-Statement mapping, rollup synthesis, spooling, ld
intercept) lives in `03-crayon-integration.md`.

n00b_attest is a **separate library** from libn00b and from
libn00b_chalk. It depends on both:

- **libn00b** for the runtime, crypto primitives, allocator
  machinery, JSON / dict / result types.
- **libn00b_chalk** for codec-specific in-band marking (ELF /
  Mach-O / PE / wrappers / scripts / sidecars), mark
  construction, and the unchalked SHA-256 used as the
  in-toto Statement's subject digest.

n00b_attest itself owns: in-toto Statement construction, DSSE
envelope sign/verify, signer backends and the secret-manager
discovery chain, the OCI registry client, the pubkey resolver,
the verifier surface, and the CLI. It does **not** own marking
code — that lives in libchalk by deliberate separation of
concerns.

---

## 1. Project layout

A module of libn00b. Workspace at `~/n00b-attest/` (a `jj`
workspace of `~/n00b/`, per D-017). Source lives inside the
n00b repo at `include/attest/...` and `src/attest/...`,
following the libn00b_chalk module-layout precedent:

```
<n00b repo>/
├── include/
│   ├── attest/
│   │   ├── n00b_attest.h              ← public API (ncc-flavor; _kargs OK)
│   │   ├── n00b_attest_cabi.h         ← public API for non-ncc callers
│   │   │                                (flat C, no _kargs, no `+`)
│   │   ├── n00b_attest_statement.h    ← in-toto Statement v1 builder/parser
│   │   ├── n00b_attest_dsse.h         ← envelope encode/decode/sign/verify
│   │   ├── n00b_attest_sign.h         ← signer abstraction
│   │   ├── n00b_attest_oci.h          ← OCI v2 + referrers client
│   │   └── n00b_attest_pubkey.h       ← pubkey resolver
│   ├── chalk/                          ← (existing libchalk module)
│   ├── core/                           ← (libn00b)
│   ├── adt/                            ← (libn00b)
│   └── ...
├── src/
│   ├── attest/
│   │   ├── statement.c
│   │   ├── dsse.c
│   │   ├── sign.c
│   │   ├── backends/
│   │   │   ├── backend.c              ← uniform backend vtable
│   │   │   ├── keychain.m             ← macOS Keychain backend
│   │   │   ├── file.c                 ← mode-0600 file backend
│   │   │   ├── env.c                  ← env-var-direct backend
│   │   │   ├── op.c                   ← 1Password CLI shell-out
│   │   │   ├── vault.c                ← HashiCorp Vault
│   │   │   ├── aws_sm.c               ← AWS Secrets Manager
│   │   │   ├── gcp_sm.c               ← GCP Secret Manager
│   │   │   └── az_kv.c                ← Azure Key Vault
│   │   ├── pubkey/
│   │   │   ├── keyring.c              ← offline keyring file format + loader
│   │   │   └── directory.c            ← (Phase 2) hosted directory client
│   │   ├── oci/
│   │   │   ├── registry.c             ← v2 + referrers client
│   │   │   ├── auth.c                 ← Basic / Bearer / cred-helper / Keychain
│   │   │   └── manifest.c             ← OCI 1.1 artifact manifest shape
│   │   └── cli/n00b_attest.c          ← the `n00b-attest` CLI binary entry point
│   ├── chalk/                          ← (existing libchalk module)
│   ├── core/                           ← (libn00b)
│   └── ...
├── meson.build                         ← n00b's existing build; gains attest targets
└── scripts/build.sh                    ← n00b's existing tooling
```

Things deliberately **not** in this layout:

- **Separate `subprojects/`.** libn00b and libchalk are in the
  same repo; no wraps.
- **Separate `scripts/build.sh`.** n00b's existing build
  handles us.
- **Codec implementations.** ELF / Mach-O / PE / etc. mark
  insertion is libchalk's job. We never link an ELF parser.
- **Self-introspection helpers.** The in-container client
  library (`co_attest_self`) is a separate, tiny project that
  ships alongside but doesn't share this codebase — it has
  different dependency constraints (must run in customer
  containers without libn00b).
- **The Crayon adapter.** Lives in the Crayon tree under
  `src/attest/`; consumes this module via
  `n00b_attest_cabi.h`. See `03-crayon-integration.md`.

Two header roots. `n00b_attest.h` is the rich ncc-flavor surface
(uses `_kargs`, `+` varargs, rich strings) — meant for ncc-
compiled callers. `n00b_attest_cabi.h` is the stripped, plain-
C23 surface — meant for Crayon (clang) and any other caller that
can't take ncc syntax. Both surfaces resolve to the same symbols;
the C-ABI header is a generated translation of the ncc one.

`src/attest/backends/` is structured so each backend is
independently compilable and disable-able at meson config time.
Customer environments that don't link OpenSSL-based AWS / GCP /
Azure SDKs can build with those backends excluded; the registry
client and core signing path do not depend on them.

## 2. Build and link

Built by n00b's existing tooling (meson + `scripts/build.sh`)
per the n00b module convention. The attest module adds its
targets to n00b's existing `meson.build`. The build accepts
`--with/--without-attest-backend=<name>` flags so the AWS /
GCP / Azure / Vault / 1Password backends can be excluded in
constrained build environments.

Per D-016, v1 supports Ed25519 + ECDSA P-256 only. RSA is
deferred (potentially indefinitely).

The OS-specific cryptography stories:

- **macOS**: prefer the Security framework (`SecKey`, `SecItem`,
  `SecKeychain`) for in-Keychain operations because the private
  key bytes never need to leave the secure boundary. Falls back
  to plain Ed25519 / ECDSA implementations via libn00b crypto
  primitives for non-Keychain backends.
- **Linux**: plain Ed25519 / ECDSA via libn00b crypto
  primitives. PKCS#11 is a future hook — not v1.

The whole thing is **built by ncc** as part of the n00b build.
The non-ncc consumer story is the C-ABI header — see §3.
Crayon consumes `libn00b_attest_dep` via meson subproject from
its own tree (Crayon is a separate repo; it pulls the n00b repo
via `subprojects/n00b.wrap` and depends on the attest module's
public headers).

## 3. The C-ABI boundary

This is load-bearing. Crayon is compiled with stock Apple clang
(no ncc). Every symbol Crayon calls must be expressible in plain
C23. Implications:

- **No `_kargs` in `attest_cabi.h`.** Every optional parameter is
  carried in an explicit options struct that the caller fills in
  by name (designated initializers, also plain C23):
  ```c
  typedef struct {
      n00b_attest_allocator_t *allocator;   // nullable
      uint8_t                  algorithm;   // N00B_ATTEST_ALG_ED25519 etc.
      bool                     bundle_cert; // include cert chain in envelope
      bool                     tls_verify;  // default true (registry calls)
  } n00b_attest_sign_opts_t;

  int n00b_attest_sign(const uint8_t *predicate_json,
                       size_t          predicate_len,
                       const n00b_attest_subject_t *subjects,
                       size_t          n_subjects,
                       n00b_attest_signer_t *signer,
                       const n00b_attest_sign_opts_t *opts,
                       n00b_attest_envelope_t **out_envelope);
  ```
  The corresponding ncc-flavor source-of-truth signature lives in
  `n00b_attest.h`:
  ```c
  extern n00b_result_t(n00b_attest_envelope_t *)
  n00b_attest_sign(n00b_buffer_t                         *predicate_json,
                   n00b_list_t(n00b_attest_subject_t *)  *subjects,
                   n00b_attest_signer_t                  *signer) _kargs
  {
      uint8_t           algorithm   = N00B_ATTEST_ALG_ED25519;
      bool              bundle_cert = false;
      bool              tls_verify  = true;
      n00b_allocator_t *allocator   = nullptr;
  };
  ```
  The C-ABI form above is generated from this declaration: kwargs
  flatten into the opts struct (with `nullptr` for the allocator
  becoming the runtime default), the buffer becomes
  `(predicate_json, predicate_len)`, the list becomes
  `(subjects, n_subjects)`, the `n00b_result_t(...)` return
  becomes `(int, **out_envelope)`. The generator runs at build
  time; both headers are checked in for IDE convenience but the
  ncc form is authoritative.
- **No `+` varargs.** Where the ncc surface uses checked variadics
  (rare in this subsystem — almost everything is structured), the
  C-ABI form takes an explicit `(items, count)` pair.
- **No rich-string returns.** Where the ncc surface returns
  `n00b_string_t *`, the C-ABI form returns `(uint8_t *, size_t)`
  or a typed handle the caller releases via a paired free
  function.
- **No `n00b_result_t(...)` returns across the boundary.** Returns
  are `int` (0 OK, negative errno-style code) plus an out-pointer
  for the value. Inside libn00b we keep the Result discipline; at
  the boundary it flattens.
- **Allocator opt-in works through the boundary.** The C-ABI
  surface accepts a `n00b_attest_allocator_t *` (opaque pointer
  to an n00b allocator). Crayon either passes `nullptr` (use
  libn00b's runtime default) or threads through a per-build-
  session arena. This is the *only* way Crayon participates in
  allocator selection; it does not need to understand the n00b
  allocator surface internally.

The C-ABI header is generated from the ncc header by a script
that strips the ncc-specific syntax and re-types kwargs as a
struct. We do not maintain two copies of the declarations by
hand — the generator runs at build time and the C-ABI header is
a build product, checked in for IDE convenience but
authoritatively regenerated.

## 4. Allocator discipline

**This is the section reviewers must read most carefully** —
allocator misuse is the historical failure mode in libn00b work
under this team (see auto-memory). The rules below are
non-negotiable:

1. **Every allocating API takes an `.allocator = nullptr` kwarg.**
   No exceptions, including for "small" internal allocations.
   `nullptr` means "use the runtime allocator at call time" — it
   does NOT mean "skip the allocator parameter and call
   `malloc`."
2. **No `[[gnu::constructor]]` state holding caller allocations.**
   Module init can grab its own arena from the runtime default;
   anything stored long-term by the subsystem (signer caches,
   registry connection pools) lives in arenas the subsystem owns
   and tears down at module teardown. Caller-owned allocations
   are *only* held for the duration of the call that produced
   them.
3. **No hidden global allocator references.** No `static
   n00b_allocator_t *cached_default` anywhere. If you need the
   current default, call `n00b_get_default_allocator()` at use
   time.
4. **Backends respect the allocator too.** A backend that returns
   private-key bytes returns them in a buffer allocated from the
   caller-supplied allocator (or default), and the backend is
   responsible for zeroizing that buffer when the caller releases
   it via the backend's release entry point. No backend may
   secretly stash key material in its own arena and hand the
   caller a pointer with a lifetime tied to the backend instead
   of the call.
5. **Arena-friendliness is a hard requirement.** A caller can
   pass one arena into `n00b_attest_sign(...)` and free the whole
   arena at the end of the build session. We test this in a
   dedicated test pass (`tests/attest/arena_lifecycle.c`): build
   100 envelopes against a single arena, free the arena, valgrind
   says no leaks and the arena's whole allocation footprint
   collapses to zero.
6. **PR review checklist enforced.** A `n00b_attest_*` PR that
   touches allocating code must include a positive checklist item
   "I verified .allocator threads through every new call path."
   The pattern of regressions is "claude touches the n00b code
   base, hardcodes an allocator path, nobody notices" — this is
   the explicit guard rail.

## 5. Statement and envelope construction

### 5.1 Statement v1

`n00b_attest_statement_t` is an opaque builder. The flow:

```c
n00b_attest_statement_t *st = n00b_attest_statement_new(.allocator = arena);
n00b_attest_statement_add_subject(st,
                                  .name   = r"hello",
                                  .digest = sha256_buf);
n00b_attest_statement_set_predicate_type(st, r"https://slsa.dev/provenance/v1");
n00b_attest_statement_set_predicate_json(st, predicate_buf);
n00b_buffer_t *serialized = n00b_attest_statement_serialize(st);
```

Public signatures of the surface used above (ncc-flavor;
header lives in `include/n00b_attest.h`):

```c
extern n00b_attest_statement_t *
n00b_attest_statement_new(void) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

extern n00b_result_t(bool)
n00b_attest_statement_add_subject(n00b_attest_statement_t *st) _kargs
{
    n00b_string_t  *name   = nullptr;   /* required — defaulted nullptr only so
                                           the kwarg syntax accepts no-name
                                           calls in tests; runtime check fails
                                           if absent. */
    n00b_buffer_t  *digest = nullptr;   /* same */
};

extern n00b_result_t(bool)
n00b_attest_statement_set_predicate_type(n00b_attest_statement_t *st,
                                          n00b_string_t           *type_uri);

extern n00b_result_t(bool)
n00b_attest_statement_set_predicate_json(n00b_attest_statement_t *st,
                                          n00b_buffer_t           *predicate_json);

extern n00b_buffer_t *
n00b_attest_statement_serialize(n00b_attest_statement_t *st) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};
```

The Statement serializer produces **compact JSON with no
extraneous whitespace, byte-stable per construction order**,
using libn00b's `n00b_json_encode(.pretty = false)`
(see `/Users/viega/n00b/include/parsers/json.h:152-155`). This
level of stability is sufficient for our use case: DSSE
signing computes over payload bytes stored verbatim in
`envelope.payload`, and DSSE verification validates the
signature against those same stored bytes (the verifier never
re-encodes). JCS-strict canonicalization (sorted keys,
insertion-order-independent output) is **not** required —
see D-024 for the rationale. The implication for callers:
build the Statement object deterministically from your inputs
and the bytes will reproduce; do not rely on cross-producer
byte-identity between semantically-equivalent Statements
built via different code paths.

Predicate JSON is opaque to the builder — caller supplies an
already-serialized blob. The library does not validate predicate
shape (SLSA, SBOM, custom) against any schema; that's the
caller's responsibility. (We do ship validators for the SLSA
predicate and our own SBOM predicate as separate utility
functions, used by the CLI's `inspect` and `verify` paths.)

### 5.2 DSSE envelope

`n00b_attest_envelope_t` wraps a Statement payload with one or
more signatures. Payload is base64-encoded per the DSSE spec.
The PAE (Pre-Authentication Encoding) string follows DSSE:
`DSSEv1 <payloadType-len> <payloadType> <payload-len> <payload>`
joined with single spaces — this is the actual byte sequence the
signer signs.

`n00b_attest_envelope_sign(env, signer, .opts)` appends a
signature. Multi-signer envelopes are built by calling
`envelope_sign` multiple times with different signer handles.

`n00b_attest_envelope_verify(env, pubkey_resolver, .opts)`
verifies all signatures and returns one of:
- `OK` (every signature verified)
- `OK_PARTIAL` (at least one verified, at least one did not)
- `FAIL_NO_SIGNATURES`
- `FAIL_NO_KEY_MATCH` (no signature could be matched to a known
  pubkey)
- `FAIL_VERIFY` (a signature was matched and failed)

`OK_PARTIAL` is for the two-party-signoff use case where one
signer may not be trusted at verification time but another is.

### 5.3 Chalk integration: envelope → in-binary mark

n00b_attest's marking entry point is a thin wrapper that
delegates the byte-level work to libchalk:

```c
extern n00b_result_t(n00b_attest_mark_result_t *)
n00b_attest_mark_artifact(n00b_string_t                          *artifact_path,
                          n00b_list_t(n00b_attest_envelope_t *)  *envelopes) _kargs
{
    bool              bundled       = true;         /* full envelopes vs digest only */
    n00b_string_t    *registry_hint = nullptr;
    n00b_allocator_t *allocator     = nullptr;
};
```

Internally:

1. Build the chalk mark's `ATTESTATION` JSON tree:
   - `envelope_digest`, `predicate_types`, `registry_hint`,
     `signer_keyid` always.
   - `envelopes[]` only in bundled mode.
2. Construct a fresh `n00b_chalk_mark_t` via
   `n00b_chalk_mark_new()`.
3. Attach the ATTESTATION tree via
   `n00b_chalk_mark_set_attestation(mark, json)`.
4. Call `n00b_chalk_insert_file(path, mark)`. libchalk
   detects the codec, computes the unchalked hash, fills in
   `HASH` / `CHALK_ID` / `METADATA_ID` / `CHALK_VERSION` /
   `TIMESTAMP_WHEN_CHALKED`, and writes the marked artifact
   (or sidecar) in place.
5. Return the unchalked SHA-256 to the caller — this is the
   value that goes into the envelope's `subject.digest.sha256`
   when the envelope was signed at AUTH_CLOSE time.

The reverse — extracting the envelope from an existing mark —
uses libchalk's extract:

```c
extern n00b_result_t(n00b_attest_extract_result_t *)
n00b_attest_extract_from_artifact(n00b_string_t *artifact_path) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};
```

which calls `n00b_chalk_extract_file(path)`, pulls the
ATTESTATION sub-tree from the returned mark dict, and decodes
the envelopes it contains (or marks them as needing
registry-resident fetch in lazy mode).

These two entry points are the only places n00b_attest reaches
into libchalk. The codec details (Mach-O `LC_NOTE`, ELF
`.chalk.mark` section, PE `.chalk` section, ZIP entry comment,
.pyc trailer, etc.) are libchalk's contract — we don't
duplicate them.

**Re-signing.** Mach-O and PE binaries that were already
codesigned at insert time have their signatures invalidated
by the mark insertion. libchalk provides strip-helpers
(`n00b_chalk_macho_strip_signature`,
`n00b_chalk_pe_signature_kind`, etc.) and a documented
"strip → insert → re-sign" flow. n00b_attest **does not run
codesign / signtool itself** — the customer's build pipeline
owns the final signature. The Crayon adapter
(`03-crayon-integration.md` §1) places its intercept *before*
the customer's codesign step so the re-sign is the normal
terminal step of an unmodified build pipeline.

### 5.4 Signing algorithm and pubkey reference

DSSE envelopes carry per-signature metadata: `keyid` (an opaque
caller-defined identifier the verifier uses to resolve the public
key) and optionally `cert` (X.509 cert chain when using FR-7).
Our `keyid` convention is the full SHA-256 of the SPKI DER
encoding of the public key, hex-encoded (= 64 hex characters),
matching the cosign / sigstore ecosystem convention. Per D-039
(resolves DF-003) (D-039 is not yet logged; the orchestrator will
log it after this dispatch returns clean — pre-stage the
reference in source comments and the spec text). This is stable
across serialization roundtrips and lets a verifier look up the
public key in a keyring without parsing the envelope payload.

## 6. The signer abstraction

```c
typedef struct n00b_attest_signer n00b_attest_signer_t;

/* Resolve a signer. With no arguments, walks the default discovery
 * chain (FR-SM-2). With .ref set, uses that backend URI directly.
 * Returns Err on resolution failure with a structured n00b error.
 *
 * Note: per D-035 part 2 the n00b-attest module uses the project-
 * local `T * = nullptr` shape for optional pointer kwargs rather
 * than the cross-project canonical `n00b_option_t(T) =
 * n00b_option_none(T)`. Cross-project normalization is a later
 * cleanup WP per D-035; until then the as-implemented signature
 * for `.ref` is `n00b_string_t *ref = nullptr`. */
extern n00b_result_t(n00b_attest_signer_t *)
n00b_attest_signer_resolve(void) _kargs
{
    n00b_string_t    *ref       = nullptr;
    n00b_allocator_t *allocator = nullptr;
};

/* Sign arbitrary bytes; returns the signature bytes as a buffer. */
extern n00b_result_t(n00b_buffer_t *)
n00b_attest_signer_sign(n00b_attest_signer_t *s,
                        n00b_buffer_t        *bytes_to_sign) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/* Public-key bytes (SubjectPublicKeyInfo DER). Owned by the buffer. */
extern n00b_buffer_t *
n00b_attest_signer_pubkey_spki_der(n00b_attest_signer_t *s);

extern void n00b_attest_signer_release(n00b_attest_signer_t *s);
```

The signer abstraction is **deliberately tiny**: load by URI, sign
bytes, expose public key. Backends differ in how they implement
the load step and whether `sign` exfiltrates private key bytes or
delegates to an in-secure-boundary primitive (Keychain `SecKey`).

### 6.1 Backend interface

Function-pointer types do not carry `_kargs` (the kwarg rewrite
happens at the call site against a declared direct signature; a
function pointer is reassignable at runtime, so the rewrite has
nothing to bind against). Internal vtables therefore use plain
function pointers with primitive arguments. Where the
internal-call needs to thread an allocator or other knobs through,
the resolver passes a small opts struct **internal to the package**
(not exposed to library consumers):

```c
typedef struct {
    n00b_allocator_t *allocator;   /* nullable */
} n00b_attest_backend_call_opts_t;

typedef struct {
    n00b_string_t *scheme;          /* "keychain", "file", "op", ... */

    n00b_result_t(n00b_attest_signer_t *)
        (*load)(n00b_string_t                       *uri,
                const n00b_attest_backend_call_opts_t *opts);

    n00b_result_t(n00b_buffer_t *)
        (*sign)(n00b_attest_signer_t                *s,
                n00b_buffer_t                       *bytes,
                const n00b_attest_backend_call_opts_t *opts);

    n00b_buffer_t *
        (*pubkey)(n00b_attest_signer_t *s);

    void
        (*release)(n00b_attest_signer_t *s);
} n00b_attest_backend_t;
```

The public-surface `_kargs` API (§6 above) flattens its kwargs into
a `n00b_attest_backend_call_opts_t` once at the resolver edge; the
opts struct is internal to the package's private headers
(`include/private/attest/backends/`). External callers never see
it — they use the `_kargs` form. This is the **only** place an
opts-struct pattern appears on the ncc surface, justified by the
function-pointer constraint, and it lives in a private header so
it cannot leak.

Backends register themselves via a manifest known to the resolver
at library init time. Out-of-tree backends register via
`n00b_attest_register_backend(backend)` — useful for customer-
specific HSM integrations that don't ship in libn00b:

```c
extern n00b_result_t(bool)
n00b_attest_register_backend(n00b_attest_backend_t *backend);
```

### 6.2 Signer-key caching

A `n00b_attest_signer_t` returned by `resolve` is reusable for the
lifetime of a build session. Crayon's adapter resolves once per
`build_session_start`, signs N times during the session,
releases at `build_session_summary` time. This bounds the network
round-trip cost for remote backends (AWS SM, Vault, GCP) and
bounds the time window during which a backend has cached
sensitive material in memory.

The library does **not** maintain a process-wide signer cache.
Callers own the lifetime explicitly. (Rationale: process-wide
caches are footguns — if Crayon's adapter exits without
explicitly releasing, the cache survives across child workers,
risking accidental signing under the wrong identity. Per-session
lifetime is auditable.)

## 7. Public-key resolution for verification

`n00b_attest_pubkey_resolver_t` is the verifier-side dual of the
signer abstraction. The resolver maps `keyid` → public key bytes.

Resolution sources, in priority order:

1. **Caller-provided in-memory keyring** (passed at resolver
   construction).
2. **Local on-disk keyring** under `/etc/n00b-attest/keyring/`
   and `$XDG_CONFIG_HOME/n00b-attest/keyring/`. Each file is a
   PEM-encoded SPKI public key; the resolver indexes them by
   their computed `keyid`.
3. **(Phase 2)** CO-hosted public-key directory — optional,
   never required.

The resolver returns `NOT_FOUND` (not `FAIL`) when no key matches
a given `keyid`, so the caller can distinguish "no key registered"
from "key registered but signature wouldn't verify."

## 8. OCI registry client

The OCI registry surface in `include/private/attest/oci/` is
narrow: enough to push and discover OCI 1.1 artifact manifests
against a v2 registry, and enough to fetch manifest blobs by
digest. We do **not** ship a general-purpose OCI client — we
implement what the attest path needs and reject anything else.

### 8.1 Auth

`n00b_attest_oci_auth_t` is the auth handle. Sources, walked in
order:

1. Caller-provided handle (explicit construction).
2. Per-registry config in
   `$XDG_CONFIG_HOME/n00b-attest/registries.json`.
3. Docker-credential-helper invocation (the cred-helper protocol:
   the binary's name is `docker-credential-<store>`, it speaks
   JSON on stdin/stdout). We support whatever cred-helper the
   user already has wired up; `docker login` and friends keep
   working.
4. macOS Keychain item under
   `com.crashoverride.n00b-attest.registry.<host>`.
5. Anonymous.

The OCI distribution spec's token-exchange flow (`WWW-Authenticate:
Bearer realm=...,service=...,scope=...`) is implemented inline —
we don't shell out for it.

### 8.2 Push

`n00b_attest_oci_push_attestation(registry, image_ref,
image_digest, envelope, opts)`:

1. Build an OCI 1.1 artifact manifest:
   ```json
   {
     "schemaVersion": 2,
     "mediaType": "application/vnd.oci.image.manifest.v1+json",
     "artifactType": "application/vnd.in-toto+dsse",
     "config": { ... empty config blob ... },
     "layers": [
       { "mediaType": "application/vnd.in-toto+json",
         "digest": "sha256:<envelope-digest>",
         "size": <envelope-bytes> }
     ],
     "subject": {
       "mediaType": "application/vnd.oci.image.manifest.v1+json",
       "digest": "<image_digest>",
       "size": <image-manifest-size>
     },
     "annotations": {
       "com.crashoverride.attestation.predicate-type": "<URI>",
       "com.crashoverride.attestation.signer-keyid": "<hex>"
     }
   }
   ```
2. PUT the envelope blob.
3. PUT the manifest.
4. Return the manifest digest.

Annotations carry the predicate type + keyid up to manifest
level so the referrer-listing endpoint can filter / pre-sort
without fetching the envelope blob. This is what gives verifier
tooling a fast path to "find me the SLSA attestation, ignore
the SBOM."

### 8.3 Discover

`n00b_attest_oci_list_referrers(registry, image_ref,
image_digest, .artifact_type = "application/vnd.in-toto+dsse")`
hits `/v2/<name>/referrers/<digest>`, follows pagination,
returns a list of `(manifest_digest, predicate_type,
signer_keyid, manifest_bytes)` tuples. We do not auto-fetch the
envelopes — caller decides which to pull.

### 8.4 Hardening

Per NFR-5:

- Size cap on referrer-listing response page: 1 MiB per page.
- Recursion / depth cap on JSON parse: 32.
- Reject non-HTTPS redirects: a registry that 301s an HTTPS
  request to plain HTTP is treated as a hard error.
- Reject host changes on redirect unless the new host is in an
  explicit allow-list config option.
- All HTTP client knobs (timeouts, retry budgets) are bounded
  per-call, never global.

## 9. Verifier surface

The public verifier API is intentionally small:

```c
extern n00b_result_t(n00b_attest_verify_result_t *)
n00b_attest_verify(n00b_attest_envelope_t        *envelope,
                   n00b_attest_pubkey_resolver_t *resolver) _kargs
{
    /* If unset, subject-match is enforced. Disable only for
     * inspection-only flows where you knowingly want to read a
     * payload without binding it to a candidate artifact. */
    bool                                  require_subject_match = true;

    /* If set, the envelope's Statement.subject.digest.sha256 must
     * equal this digest. Otherwise the call only verifies the
     * signature and the (parsed) Statement is returned for the
     * caller to compare itself. */
    n00b_option_t(n00b_buffer_t *)        expected_subject_digest =
                                            n00b_option_none(n00b_buffer_t *);

    n00b_allocator_t                     *allocator = nullptr;
};
```

`n00b_attest_verify_result_t` carries: overall verdict (an enum
covering `OK`, `OK_PARTIAL`, `FAIL_NO_SIGNATURES`,
`FAIL_NO_KEY_MATCH`, `FAIL_VERIFY`, `FAIL_SUBJECT_MISMATCH`),
per-signature verdict list, the parsed Statement, and any
structured errors encountered along the way. The CLI's `n00b-
attest verify` wraps this; downstream policy engines link
directly.

Note that the result type is itself a heap object owned by the
caller; the outer `n00b_result_t(n00b_attest_verify_result_t *)`
distinguishes "verification machinery itself failed" (e.g., a
resolver I/O error, an out-of-memory) from "verification ran and
the envelope was rejected" — which is communicated through the
result struct's verdict field, not as an `Err`.

## 10. CLI shape

`n00b-attest <verb> [args]`:

```
n00b-attest sign       --predicate file.json --subject name:digest [--signer URI] [--out env.dsse]
n00b-attest verify     --envelope env.dsse --keyring dir [--expected-subject sha256:...]
n00b-attest inspect    --envelope env.dsse [--json]
n00b-attest push       --envelope env.dsse --image foo/bar@sha256:... [--registry ghcr.io]
n00b-attest pull       --image foo/bar@sha256:... --predicate-type slsa-provenance [--out env.dsse]
n00b-attest discover   --image foo/bar@sha256:... [--registry ghcr.io] [--json]
n00b-attest mark       <artifact> --envelope env.dsse [--lazy] [--registry-hint H]
n00b-attest unmark     <artifact>
n00b-attest extract    <artifact> [--json]
n00b-attest harvest    <image-ref-or-tarball> [--json]
n00b-attest setup      [--backend keychain|file|...] [--alg ed25519]
n00b-attest rotate     [--backend ...]
n00b-attest list-keys  [--keyring dir]
```

`mark` / `unmark` / `extract` delegate to libchalk's
`insert_file` / `delete_file` / `extract_file` with the
ATTESTATION-JSON shape n00b_attest defines. `harvest` walks an
OCI image's layers (pulled or local) and runs extract against
every file, reporting which are chalked and which aren't.

The CLI is a thin wrapper around the library calls. No business
logic lives here; everything is in `include/private/attest/`.

## 11. Threading and concurrency

The subsystem is **call-safe from multiple threads** at the
function-call level: any two concurrent calls into the library
do not corrupt internal state. Handles (signers, envelopes,
resolvers, registry clients) are **not** safe for concurrent use
on the same handle — each handle is single-owner. This matches
the libn00b convention.

The Crayon adapter uses one signer handle per build session and
serializes signing operations within that session on a dedicated
queue.

## 12. Testing harness

- **Unit tests**: per-source-file tests under `tests/attest/`,
  hand-rolled per `NCC.md` convention. Cover Statement
  serialization roundtrip, DSSE PAE generation, per-backend load
  + sign + release lifecycle, OCI manifest construction, JSON
  hardening (oversize input, deep nesting, malformed UTF-8).
- **Arena lifecycle test** (mentioned above): build N envelopes
  through one arena, free, leak-check.
- **Backend integration tests**: each remote backend (Vault, AWS
  SM, GCP SM, Azure KV) runs against a local mock; CI matrix
  flips backends on/off based on whether mock images are
  available.
- **End-to-end registry tests**: zot brought up by the test
  harness in a container. Push, discover, pull roundtrip.
- **Cross-tool interop smoke** (NFR-15 was rewritten away since
  cosign-compat dropped; the replacement is): sigstore-python
  in keyed mode verifies an envelope we produced. Runs once per
  release as a non-blocking compatibility canary.
- **Fuzz**: AFL/libFuzzer harnesses for the envelope parser, the
  manifest parser, and the JSON predicate validator.

## 13. Versioning and ABI

`include/n00b/attest.h` and `attest_cabi.h` carry a `#define
N00B_ATTEST_API_VERSION` integer. ABI breaks bump the major; the
library exposes both old and new symbols via versioned symbol
names for one release after a break.

The on-the-wire formats (DSSE, in-toto Statement, OCI manifest)
are spec-defined and don't move on our cadence. The
Crashappsec-owned predicate types (SBOM, builder-id URI) carry
their own `/v1`-suffixed URIs; we bump the URI on any
non-backwards-compatible field change.
