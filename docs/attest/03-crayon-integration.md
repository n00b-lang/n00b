# Crayon integration

**Status:** SPEC (STABLE)  
**Last updated:** 2026-05-17

This doc covers the Crayon-side adapter that wires the
`n00b_attest` library and `libn00b_chalk` into Crayon's
existing build-lifecycle event stream. The previous draft
designed the integration around a synthetic image-layer
augmentation; this draft replaces it with the
chalk-mark-at-ld-time model that aligns with how
libn00b_chalk and Crayon's existing ES sensor are built.

---

## 1. The two intercept points

We need to mark each binary *after* `ld` (or equivalent) has
produced the final bytes and *before* the customer's
`codesign` / `signtool` / IMA-evmctl seals them — because
chalk insertion mutates the binary's bytes and invalidates any
existing signature. Two candidate intercepts, both viable, with
different trade-offs:

### 1.1 ld-output intercept

**Mechanism.** ES `AUTH_CLOSE` on the file `ld` opened for
write. When `ld` is about to close its output file, the auth
event is held; we mark the binary in place; we ALLOW the close.
`ld` returns normally with no awareness that anything happened.
Linux equivalent: fanotify in permission mode (`FAN_OPEN_PERM`
on the same file path, or `FAN_CLOSE_WRITE` with explicit
hold-and-release semantics in fanotify-permission mode).

**Pros.**

- **Universal.** Every native build that produces a binary
  invokes a linker. Catches Linux ELF (which has no codesign
  story at all), unsigned macOS dev builds, and Windows builds
  alike.
- **Earliest possible.** The customer's downstream signing
  pipeline sees the marked binary as input; their existing
  `codesign --sign ...` / `signtool` invocations run unchanged
  and produce a normally-signed marked binary.
- **No PATH manipulation, no exec wrapping.** Customer's build
  system is unmodified.

**Cons.**

- **AUTH_CLOSE has a deadline.** macOS ES requires the auth
  response within the configured deadline (default ~5s; can be
  shortened by the kernel under pressure). Our mark-and-sign
  has to fit. Sign-against-Keychain typically completes in
  tens of ms; sign-against-AWS-SM is 100–500 ms; sign-against-
  Vault is similar. Worst case (Vault unreachable, exponential
  backoff) blows the deadline.
- **Linker exec patterns vary.** Some linkers produce
  intermediate `.tmp` files and `rename` to final path. Some
  build systems wrap `ld` in a `cc` driver and the link output
  is opened/closed multiple times during incremental link.
  We need a sharp definition of "this is the final ld output"
  to avoid double-marking, mis-timed marking, or missing it.
- **VM boundary on macOS.** When the build runs inside Docker
  Desktop's Linux VM (common for cross-builds), macOS ES
  doesn't see it. We get no signal until the image is
  pushed — too late for in-band marking. Linux Crayon's
  fanotify hook inside the container would cover this once
  Linux Crayon ships.

### 1.2 codesign wrap

**Mechanism.** ES `AUTH_EXEC` on `/usr/bin/codesign` (or the
`codesign` binary as resolved by `xcrun`). On exec, deny,
and substitute our wrapper that:

1. Parses the codesign argv to find the target path.
2. Marks the target binary in place via libchalk.
3. Re-execs the real `codesign` with the original argv (now
   operating on the marked bytes).

**Pros.**

- **Codesign is the natural seal moment.** Every shipped macOS
  binary calls codesign exactly once before notarization. Our
  intercept lines up with that contract: mark, then seal.
- **No deadline pressure.** AUTH_EXEC has more headroom than
  AUTH_CLOSE; the wrapper runs in its own process with its
  own time budget.
- **Predictable target.** The argv tells us what to mark, where.
  No "which file did ld actually produce" disambiguation.

**Cons.**

- **macOS-specific.** Doesn't cover Linux ELF (no codesign at
  all). For Linux we still need the ld intercept (or its
  fanotify analog).
- **Wraps a security-critical tool.** Substituting `codesign`
  is operationally sensitive — if our wrapper crashes or is
  misconfigured, the customer's build pipeline stops. The
  failure mode is loud, but the blast radius is the customer's
  entire signing capability on that host.
- **Re-codesigning on iteration.** If the customer codesigns
  the same binary multiple times during development (debug
  iterations), we'd mark it multiple times unless we dedupe by
  detecting an existing mark and skipping. Chalk's
  insert-after-extract idempotency handles this cleanly: if
  the existing mark's ATTESTATION matches what we'd write, skip.
- **Doesn't cover unsigned dev builds.** If the customer's dev
  workflow doesn't call codesign at all, this intercept never
  fires.

### 1.3 Recommendation

**Both, with the ld intercept as the default and codesign-wrap
as an opt-in or platform-specific complement.**

- **Linux**: ld intercept only. There's no codesign.
- **macOS**: ld intercept as primary (catches dev builds,
  catches builds with no downstream codesign, runs early). The
  customer's codesign then runs over a marked binary as its
  normal final step.
- **macOS, opt-in**: codesign-wrap as a secondary intercept for
  customers who specifically want the "seal moment" semantics
  or who are uncomfortable with auth-close-deadline risk.
- **Windows**: signtool-wrap, analogous to codesign-wrap. Not
  v1.

The detailed deadline-budget analysis for ld intercept and the
double-mark dedupe rules are open questions — see
`05-open-questions.md` OQ-INTERCEPT-1 and OQ-INTERCEPT-2.

## 2. Where the adapter lives in the Crayon tree

```
include/private/attest/
├── publisher.h            ← warehouse subscriber, mark orchestration
├── slsa_mapping.h         ← build.artifact → SLSA Provenance v1 predicate
├── sbom_mapping.h         ← package_contributions → CO SBOM predicate
├── rollup.h               ← container layer-walk + mark harvest
├── intercept.h            ← ES hooks: ld AUTH_CLOSE, codesign AUTH_EXEC
└── spool.h                ← persistent queue for deferred sign/push

src/attest/
├── publisher.m
├── slsa_mapping.m
├── sbom_mapping.m
├── rollup.m
├── intercept.m
└── spool.m
```

The subsystem links:

- `libn00b_attest` (this project) — Statement / DSSE /
  signing / registry surfaces. C-ABI consumed via
  `n00b_attest_cabi.h` (Crayon is clang-built, not ncc).
- `libn00b_chalk` (existing) — mark construction + codec
  insertion + unchalked hashing. Also C-ABI; see chalk's
  existing headers.

## 3. The per-artifact flow

```
[ ld AUTH_CLOSE on binary X ]
      │
      ▼
  intercept.m hooks the event, captures (path, file_handle)
      │
      ▼
  publisher.m correlates with the in-flight build session:
    - which build_session is this ld a descendant of?
    - which build.artifact event will Crayon emit for it?
      │  (We use the session, not the in-flight artifact event —
      │   the build.artifact event fires later, at quiesce time.
      │   We need to mark now, before AUTH_CLOSE deadline.)
      │
      ▼
  publisher.m builds a *preliminary* in-toto Statement using
  what it has at AUTH_CLOSE time: subject = unchalked SHA-256
  of the ld output, predicateType = SLSA Provenance v1,
  predicate = the partial provenance available from this
  build session's in-flight state (build_tool, build_tool_argv,
  compile_operations observed so far).
      │
      ▼
  publisher.m signs the DSSE envelope (n00b_attest_sign).
      │
      ▼
  publisher.m constructs a chalk mark with the envelope
  embedded (bundled mode) or its digest (lazy mode) in the
  ATTESTATION field, and calls
  n00b_chalk_insert_file(path, mark).
      │
      ▼
  AUTH_CLOSE → ALLOW. ld returns; customer's downstream
  codesign / strip / etc. run over the marked binary.
```

### 3.1 The "preliminary" problem and the late-update solution

The Crayon `build.artifact` event has rich content:
package_contributions, network_requests, sub_processes, the
full producing_chain — but those aren't available at
AUTH_CLOSE time. They're aggregated by the build-lifecycle
service over the quiesce window (a few seconds after ld
completes). We're constrained to mark now or never.

Resolution: the chalk mark carries the **envelope digest**, not
the envelope's full predicate. The envelope itself is signed
and pushed to the registry *later* — when the
`build.artifact` event lands with the full data. At that point
the publisher:

1. Builds the *final* in-toto Statement with all the rich data.
2. Signs the DSSE envelope.
3. Confirms the envelope's digest equals the digest already
   embedded in the chalk mark.

For step 3 to succeed deterministically, the in-toto Statement
that gets signed at AUTH_CLOSE time and the one signed at
quiesce time must have the **same canonical bytes**. We solve
this by computing the envelope digest at AUTH_CLOSE time over
a *placeholder* Statement whose `predicate.runDetails`
contains a stable opaque token (e.g.,
`{"placeholder": "<build_session_op_id>"}`), embedding that
digest in the mark, and signing+pushing the *real* Statement at
quiesce — accepting that the final envelope's digest differs
from the placeholder.

That's the cleaner alternative: **mark with a placeholder
digest at AUTH_CLOSE; reconcile at quiesce by patching the
mark.**

Two ways to "patch" a chalk mark after the fact, both viable:

- **Sidecar.** Write `<artifact-path>.chalk` next to the
  binary containing the real envelope digest. Slight UX
  friction (a sibling file appears) but no second mutation of
  the binary.
- **Re-mark.** Extract the placeholder mark, replace with the
  real one, re-insert. Avoids the sidecar but touches the
  binary a second time — and if the customer has already
  codesigned in the gap between AUTH_CLOSE and quiesce, we
  invalidate the signature.

Recommendation: **sidecar at quiesce**, with the in-binary mark
carrying a fixed-format placeholder digest that the verifier
knows to treat as "look in the sidecar." This avoids any
post-codesign mutation of the binary.

This is OQ-MARK-RECONCILE-1 — surfaced for review before
implementation.

## 4. Container rollup synthesis

The previous draft proposed augmenting the container image
with a synthetic layer carrying an identity file. That whole
mechanism dissolves. The new flow:

### 4.1 Trigger

`build.session_summary` arrives with `session_artifacts[]`
including one or more `artifact_kind: container_image` entries.
For each:

1. The image manifest is fetched from the local OCI store (if
   `--load`) or pulled from the registry (if `--push`).
2. For each layer in the manifest, the layer blob is pulled
   and tar-walked. For each file in the layer, we call
   `n00b_chalk_extract_buffer`. Files with a mark contribute
   to the rollup.
3. The rollup Statement is built (see §4.2), signed, and
   pushed as a referrer of the image's manifest digest.

### 4.2 Rollup Statement shape

```
subject = { name: <image_ref>, digest: { sha256: <manifest digest> } }
predicateType = "https://slsa.dev/provenance/v1"
predicate.buildDefinition.buildType =
   "https://schemas.crashoverride.com/crayon/build-type/container-image/v1"
predicate.buildDefinition.resolvedDependencies = [
   /* one entry per chalked file in the image */
   {
     name: "<layer-digest>:<path-in-layer>",
     digest: { sha256: <file's unchalked SHA-256, from mark.HASH> },
     annotations: {
       "com.crashoverride.attestation.envelope-digest":
                <envelope digest from mark.ATTESTATION.envelope_digest>,
       "com.crashoverride.attestation.predicate-type":
                <first predicate_type from mark.ATTESTATION.predicate_types>
     }
   },
   /* plus session_packages_installed entries */
]
predicate.runDetails.byproducts = [
   { name: "session_ops", content: <flat op list from session_summary> },
   { name: "network_requests", content: <from session_summary> },
   { name: "unchalked_contents",
     content: [ /* paths in image that had no chalk mark */ ] }
]
```

`unchalked_contents` is deliberately surfaced so operators can
see what's in the image that *isn't* attested. The set is
usually large (every library shipped by the base image, every
config file, every shell script) — and that's important to
make visible rather than hide.

### 4.3 Push ordering

The constituent per-artifact envelopes were already pushed at
their respective quiesce times during the build. The rollup
push happens after the image push; if the image went out via
`buildx --push` we observe the push completion through
`CRAYON_BUILD_EVENT_REGISTRY_REQUEST` and trigger the rollup
synthesis then.

### 4.4 What we no longer need

- No image augmentation, no synthetic layer, no
  `/.crashoverride/attestation.json` file.
- No buildx interception or wrapper command.
- No `--push` vs `--load` UX divergence.
- No "two digests in registry" follow-up rewrite.

The image's identity (its manifest digest) is whatever the
build tool produces; the rollup attestation references that
digest as a separate registry artifact. Verifiers walk from
image → registry referrers → rollup envelope → per-binary
envelopes via the per-binary chalk marks (or via the rollup's
resolvedDependencies).

## 5. The CO-native SBOM predicate

(Unchanged in shape from Draft 0; restated here so this doc
stands alone.)

**Predicate type:** `https://schemas.crashoverride.com/
attestation/sbom/v1`. Carried as a separate signed envelope,
paired with the build-provenance envelope under the same
subject. For per-artifact SBOMs the subject is the artifact's
unchalked SHA-256; for container SBOMs the subject is the
image manifest digest.

The predicate includes components with `kind`, `ecosystem`,
`name`, `version`, `digest`, `paths`, `drawn_in_by`, and a
per-component `provenance` subtree carrying the build-session
op chain that drew the component in plus the registry source
URL when applicable. Container-scope SBOMs add an `attribution`
section mapping components back to contributing artifacts and
layers.

Full schema sketched in the prior draft's §5 — promoting to a
spec file (`sbom-predicate-v1.md`) before implementation is
on the open-questions list.

## 6. Signing identity acquisition

(The publisher calls `n00b_attest_signer_resolve()` with no
arguments at the start of each session — no-arg invocation
walks the default discovery chain. It holds the handle for the
session's lifetime and releases it at session-summary. See
`02-architecture.md` §6 for the signature and the discovery
chain.)

## 7. Spool

(Unchanged in role but smaller in scope: the spool is now only
for **deferred signing** and **deferred pushing**. There's no
in-image-augmentation queue because there's no image
augmentation. Layout: `pending/`, `pushed/`, `failed/`. Drainer
retries with exponential backoff. Per-failure warehouse events
preserve observability.)

The spool still earns its keep — when ld AUTH_CLOSE fires and
the signer happens to be unreachable, we'd otherwise block the
linker for seconds. Spool the placeholder-marked binary's
sign-and-push work, allow the close, settle later. The
deadline-pressure mitigation pattern.

## 8. Self-attestation

Crayon attests itself. The auto-memory entry
`feedback_no_manual_codesign.md` forbids ad-hoc codesign on
Crayon binaries; the canonical path is `scripts/release.sh`
(via `scripts/dev-install.sh` for dev installs). Self-attest
slots into release.sh between build and codesign:

1. `meson` builds the binaries.
2. **`n00b-attest` marks each Crayon binary** with a Statement
   signed by Crashappsec's release signer (a distinct identity
   from the customer-runtime signer).
3. `scripts/release.sh` invokes `codesign --sign <Crash
   Override Developer ID>` over the marked bytes.
4. Notarization + staple proceed normally.

This is special-cased — the production Crayon build pipeline
calls `n00b_attest` directly rather than going through the ES
intercept path. The intercept path is for customer builds;
Crayon's own builds use the library directly so we don't have
to install a sensor to attest ourselves.

(Same pattern chalk's own release pipeline already follows.)

## 9. CLI evolution

`crayon attest` retains its existing two implicit modes
(stream + spawn-wrap) but gains explicit verbs. Updated tree:

```
crayon attest stream     [--json] [--verbose]            # current no-cmd
crayon attest cmd <cmd> [args...]                        # current with-cmd
crayon attest publish    [--once | --watch]              # drain spool
crayon attest verify     <binary-or-image> [--keyring D]
crayon attest inspect    <binary-or-envelope>            # extract & pretty
crayon attest queue      list | clear-failed | retry-failed
crayon attest sign       <statement.json> [--signer URI] [--out env.dsse]
crayon attest mark       <binary> [--envelope E | --predicate P]   # manual mark
crayon attest unmark     <binary>                                  # remove
crayon attest harvest    <image-ref-or-tarball>          # list chalked contents
```

`mark`, `unmark`, and `harvest` are the new verbs that
correspond to libchalk's `insert_file`, `delete_file`, and
the layer-walk operation. They're operator/debug surfaces;
the steady-state flow doesn't use them — the ES intercept
does the marking, the rollup synthesizer does the harvesting.

Wrapper-style verbs (`crayon docker`, `crayon buildx`) from
the previous draft are deleted. We don't need them: the
intercept happens at link time, not push time, and the build
tool's behavior is irrelevant downstream.

## 10. Configuration surface

`$crayon_state_root/attest/config.json`:

```json
{
  "intercept": {
    "ld":       { "enabled": true,  "auth_close_deadline_ms": 4000 },
    "codesign": { "enabled": false, "mode": "wrap-and-mark" }
  },
  "signer": {
    "ref": "keychain:com.crashoverride.n00b-attest.signer"
  },
  "publish": {
    "enabled": true,
    "targets": [ /* per-registry routing, unchanged from Draft 0 */ ]
  },
  "spool": {
    "max_bytes":             1073741824,
    "pushed_retention_days": 30
  },
  "rollup": {
    "enabled":              true,
    "harvest_marks":        true,
    "include_unchalked":    true
  },
  "mark": {
    "bundle_envelopes":     true,     /* bundled-mode by default */
    "reconcile":            "sidecar" /* "sidecar" | "rewrite" */
  },
  "sbom": {
    "include_producer_chain": true,
    "max_chain_depth":        16
  }
}
```

## 11. What doesn't change in Crayon's existing code

- The build-lifecycle service — same events, same schema.
- The warehouse — same bus, same proxy semantics. We add new
  warehouse kinds for adapter diagnostics
  (`attest.signer_unavailable`, `attest.mark_inserted`,
  `attest.reconcile_failed`, etc.).
- The ES sensor framework — we add new event subscriptions
  (AUTH_CLOSE on ld outputs; optionally AUTH_EXEC on codesign),
  but the sensor itself is the same one Crayon already runs.
- The existing `crayon attest stream` / `crayon attest cmd`
  UX. Reframed under verbs, but functional behavior preserved.

## 12. Open questions specific to Crayon integration

Cross-referenced in `05-open-questions.md`:

- OQ-INTERCEPT-1: ld AUTH_CLOSE deadline budget.
- OQ-INTERCEPT-2: detecting "this is the final ld output" vs
  intermediate / re-link.
- OQ-MARK-RECONCILE-1: sidecar vs binary re-mutation at
  quiesce-time reconcile.
- OQ-PROC-1: in-process inside warehouse daemon vs standalone
  launchd-managed service.
- OQ-INTERP-1: do we sign `interp.end` attestations?
- OQ-SBOM-SCHEMA-1: promote the SBOM predicate sketch to its
  own spec file before implementation.
