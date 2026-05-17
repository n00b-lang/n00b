# In-container attestation identity

**Status:** SPEC (STABLE)  
**Last updated:** 2026-05-17

The previous draft of this doc designed an injected
`/.crashoverride/attestation.json` file baked into a synthetic
image layer. That was reinventing what `libn00b_chalk` already
does. This draft replaces it.

---

## 1. Carrier: the binary's own chalk mark

Every artifact Crayon attests is **chalked in place** during the
build (see `03-crayon-integration.md` for the intercept
mechanism). The chalk mark embeds the artifact's attestation —
either the full signed DSSE envelope (bundled mode, default for
binaries) or the envelope digest + registry hint (lazy mode,
for size-sensitive cases).

Concretely, the mark's `ATTESTATION` field (a JSON tree
libchalk passes through verbatim) carries:

```json
{
  "envelope_digest":   "sha256:...",
  "predicate_types":   ["https://slsa.dev/provenance/v1",
                        "https://schemas.crashoverride.com/attestation/sbom/v1"],
  "registry_hint":     "ghcr.io",
  "signer_keyid":      "ab12cd34ef567890ab12cd34ef567890",

  // Present in bundled mode, absent in lazy mode:
  "envelopes": [
    {
      "predicate_type":  "https://slsa.dev/provenance/v1",
      "envelope_base64": "<DSSE bytes>"
    },
    {
      "predicate_type":  "https://schemas.crashoverride.com/attestation/sbom/v1",
      "envelope_base64": "..."
    }
  ]
}
```

The mark's other fields (`HASH`, `CHALK_ID`, `METADATA_ID`, etc.)
are computed by libchalk per its existing contract. The mark's
`HASH` field is the unchalked SHA-256 — the same value that
serves as the in-toto Statement's `subject.digest.sha256`. The
mark-vs-attestation cycle is self-consistent: a verifier
extracting the mark gets both the envelope (or its digest) and
the unchalked hash; the envelope's subject must equal that hash.

## 2. In-process identity query

The in-container client is a thin wrapper over libchalk's
extract path. The binary introspects its **own** image:

| Platform | Self-image lookup |
|---|---|
| macOS / Mach-O | `_dyld_get_image_header(0)` returns the running executable's header; walk load commands looking for `LC_NOTE` with owner `"chalk"`. |
| Linux / ELF | `dl_iterate_phdr` with a callback that picks the first entry (the running executable); locate the `.chalk.mark` section. |
| Windows / PE | `GetModuleHandleW(nullptr)` returns the running executable's base; locate the `.chalk` section via the PE headers. |

In every case, the mark bytes are read from the loaded image's
own pages — no filesystem I/O, no path resolution, no
`/proc/self/exe` race. The library calls
`n00b_chalk_extract_buffer` against those bytes and returns the
parsed mark.

Distribution shape:

- **Source-form library**: a single `co_attest_self.c` + paired
  header. Plain C, depends on libchalk for the extract call
  (or for true zero-dep builds, hand-rolls the few lines of
  Mach-O/ELF/PE header walking and JSON parsing it actually
  needs).
- **Pre-built static archives** for common platform/libc/arch
  combos, published as release assets.

The "true zero-dep" build is real and worth considering as the
default in-container form: the mark format is small enough (a
handful of JSON keys) that a 300-line C file with no dependencies
can extract the ATTESTATION field. Customer apps that don't want
libn00b in their container link the zero-dep version. Customer
tooling (admission controllers, verifiers, etc.) that already
runs libn00b uses the full libchalk extract path.

## 3. API

```c
#include <co_attest_self.h>

typedef struct co_attest_self_attestation_s {
    const char    *predicate_type;
    const char    *envelope_digest;     // "sha256:..."
    const uint8_t *envelope_bytes;      // nullable; bundled mode only
    size_t         envelope_len;
} co_attest_self_attestation_t;

typedef struct co_attest_self_identity_s {
    /* Hash that subjects this artifact in its attestation Statement.
     * Same value as the chalk mark's HASH field. The verifier uses
     * this to confirm the envelope's subject matches the running
     * binary. */
    const char *unchalked_sha256;       // "sha256:..."

    const char *registry_hint;
    const char *signer_keyid;

    const co_attest_self_attestation_t *attestations;
    size_t                              n_attestations;

    /* Diagnostic. */
    const char *chalk_version;
    const char *timestamp_when_chalked; // RFC3339
} co_attest_self_identity_t;

/* Returns 0 on success; negative on error:
 *   CO_ATTEST_SELF_ERR_NOT_FOUND   (-1)  — binary has no chalk mark
 *   CO_ATTEST_SELF_ERR_NO_ATT      (-2)  — mark present but no ATTESTATION field
 *   CO_ATTEST_SELF_ERR_MALFORMED   (-3)  — mark or ATTESTATION JSON invalid
 *   CO_ATTEST_SELF_ERR_PLATFORM    (-4)  — self-image lookup failed (rare)
 */
int  co_attest_self_load(co_attest_self_identity_t **out);
void co_attest_self_free(co_attest_self_identity_t *id);

/* Convenience: locate the first attestation matching predicate_type.
 * Returns 0 on found, CO_ATTEST_SELF_ERR_NOT_FOUND on miss.
 * `*out` is unchanged on miss. */
int co_attest_self_find(const co_attest_self_identity_t      *id,
                        const char                           *predicate_type,
                        const co_attest_self_attestation_t  **out);
```

Sentinels distinguish the four states a caller actually
cares about: not chalked, chalked-but-no-attestation,
chalked-but-corrupt, or runtime lookup glitch. The "schema-
mismatch" sentinel from the prior draft is gone — the chalk
mark format is stable (the JSON ATTESTATION blob can evolve;
libchalk's frame doesn't change underneath us).

## 4. Verification from outside the container

Pieces a verifier needs:

1. **The running binary itself.** The verifier reads the binary
   bytes (host filesystem access, `kubectl cp`, runtime API,
   etc.) and computes its unchalked SHA-256 via
   `n00b_chalk_hash_buffer`. This is the *actual* hash.
2. **The chalk mark from those same bytes.** Extracted via
   libchalk. Yields the ATTESTATION JSON.
3. **The attestation envelope.** Either from the mark's bundled
   `envelopes[]` or fetched from the registry hinted in the
   mark (`/v2/<name>/referrers/<image-digest>` then filter by
   the envelope digest the mark advertises).
4. **The public key.** Resolved from the verifier's keyring via
   the `signer_keyid` field. See `02-architecture.md` §7.

The check:

- The envelope's DSSE signature verifies under the resolved
  public key.
- The envelope's in-toto Statement's `subject.digest.sha256`
  equals the actual unchalked SHA-256 from step 1.
- (Optional) The mark's `HASH` field also equals that hash —
  redundant check, useful as an integrity sanity test.

Three things to notice:

- **Authority comes from the bytes, not the mark.** The mark is
  just a pointer. A malicious mark that points at a forged
  envelope fails the signature step; a malicious mark that
  points at a real envelope for a different binary fails the
  subject-match step.
- **The verifier never has to trust the container's filesystem
  structure or runtime state.** It needs the bytes; how it gets
  them is its problem.
- **No image-digest cross-check is required.** The image digest
  is irrelevant to the per-binary check. Container rollup
  attestations (which *are* keyed to image digest) are
  verified separately and independently — see
  `03-crayon-integration.md` §4.

## 5. Tampering attempts and how each is caught

| Attack | Caught by |
|---|---|
| Modify the binary's bytes after build | Step 1's unchalked SHA-256 changes; doesn't match the envelope's subject |
| Strip the chalk mark | Step 2 returns NOT_FOUND; verifier reports "unattested" |
| Replace the mark with one pointing at a different valid envelope | Step's subject-match check fails — that envelope's subject is for a different binary |
| Replace the mark with one pointing at an envelope signed by an attacker-controlled key | Step 4: the attacker's keyid won't resolve in the verifier's keyring |
| Forge an envelope with the correct subject digest but bogus signature | Step's signature verification fails |
| Modify the ATTESTATION JSON to claim a different signer_keyid | The signer_keyid is a hint; signature verification doesn't trust it — it tries the keyring's keys and checks signatures. The hint is for fast lookup, not authority |
| Inject a chalk mark into a previously unchalked binary | The injected mark's HASH must match the unchalked binary; the attacker would have to forge a valid signed envelope for that hash, which requires the legitimate signing key |

The single thing **not** caught by the binary's own runtime
introspection alone: a malicious process running *as* the
chalked binary that returns a false answer from
`co_attest_self_load`. But the binary's own runtime answer is
never the authoritative source — it's a self-report. Authority
is always the external verifier doing steps 1–4 against the
binary's bytes.

## 6. Containers: harvesting per-binary marks

When the container rollup is built (`03-…` §4), Crayon walks
the pushed image's layers and runs libchalk extract against
every file. Files that come back with a mark contribute one
`resolvedDependencies` entry to the rollup Statement, citing the
file's path inside the image, its unchalked SHA-256, and the
attestation envelope digest from its mark. Files without a mark
land in a "unchalked_contents" byproduct so the operator can see
what's in the image that *isn't* attested.

This means the in-container identity surface and the container
rollup attestation are connected by a property — **every chalked
binary in the image is referenced by the rollup, and the
rollup's signature transitively covers them** — without any new
mechanism. The chalk mark already says "look at this envelope";
the rollup already says "I bundle these envelopes." Verifiers
can walk in either direction.

## 7. What this is not

- **Not a runtime policy enforcer.** Tells the caller what the
  binary claims; doesn't decide what to do.
- **Not measurement / remote attestation.** No TPM, no SEV, no
  SGX. The trust root is the build-time signer.
- **Not a path-based file probe.** No
  `/.crashoverride/attestation.json` to read or to defend.
  Identity comes from the binary's own bytes.
