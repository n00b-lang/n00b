/*
 * WP-015 — system-install + trust-root protection.
 *
 * Implements the roster-lookup chain, the trust-root
 * fingerprint binding, and the optional rule-file signature
 * primitives described in `include/naudit/trust_root.h`. The
 * three decision points called out in the WP-015 prompt are
 * resolved inline:
 *
 *   - DF-CA — `@expected_roster_sha256` is a top-level
 *     directive in `audit-rules.bnf`, not a separate
 *     `audit/trust-root.bnf` file. The parsing hook lives in
 *     `guidance.c::file_field_handler`; this file consumes the
 *     resulting `guidance->expected_roster_sha256` field.
 *
 *   - DF-CB — SHA-256 uses libn00b's own `n00b_sha256_hash`
 *     (declared in `<core/sha256.h>`). The helper is a
 *     standalone libc-only SHA-256 implementation already
 *     present in the tree. Choice rationale: keeps the naudit
 *     diff free of a subprocess dependency on `shasum`, and
 *     avoids a 100-line inline reimplementation when a
 *     well-tested one already exists.
 *
 *   - DF-CC — `sign-rules` is exposed via the existing CLI's
 *     flag idiom (`naudit --sign-rules <rules-path> --key
 *     <key> --signer <id>`), wired in `src/naudit/cli.c`. The
 *     wrapper function here delegates to
 *     `n00b_audit_exemption_sign`.
 *
 * Per project DECISIONS.md D-005, this file's public functions
 * carry no `_kargs` block. Per D-008, null guards use the
 * `!ptr` idiom. Per the path-handling rule (auto-memory
 * `feedback_path_handling`), every function taking a path arg
 * calls `n00b_path_canonical` at entry.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/env.h"
#include "core/file.h"
#include "core/sha256.h"
#include "core/string.h"
#include "adt/option.h"
#include "adt/result.h"
#include "text/strings/format.h"
#include "util/path.h"

#include "naudit/errors.h"
#include "naudit/exemption.h"
#include "naudit/trust_root.h"

/* ================================================================ */
/* Roster lookup chain                                              */
/* ================================================================ */

/*
 * Read an env-var via libn00b's envp cache. Returns nullptr when
 * the variable is unset or empty (the chain treats unset and
 * empty identically — both mean "skip this slot").
 */
static n00b_string_t *
read_env_string(const char *name)
{
    n00b_string_t *nm  = n00b_string_from_cstr((char *)name);
    n00b_string_t *val = n00b_getenv(nm);
    if (!val || val->u8_bytes == 0) {
        return nullptr;
    }
    return val;
}

/*
 * Helper — canonicalize + test-for-existence. Returns the
 * canonicalized absolute path on success, nullptr when the file
 * doesn't exist (or `p` is empty).
 *
 * The chain treats "env-var set but pointing at a non-existent
 * file" as "slot is unusable, fall through to the next slot"
 * rather than as an error. Rationale: a stale env var on a CI
 * runner shouldn't bring the audit down with an unrecoverable
 * I/O error — the next slot (or the no-roster fall-through)
 * lets the user notice + recover.
 */
static n00b_string_t *
canonicalize_if_exists(n00b_string_t *p)
{
    if (!p || p->u8_bytes == 0) {
        return nullptr;
    }
    n00b_string_t *abs = n00b_path_canonical(p);
    if (!abs || !n00b_path_is_file(abs)) {
        return nullptr;
    }
    return abs;
}

/*
 * Resolve the ENV slot. Reads `NAUDIT_ROSTER`. Returns the
 * canonicalized path when set + the file exists; nullptr
 * otherwise.
 */
static n00b_string_t *
resolve_env_slot(void)
{
    n00b_string_t *raw = read_env_string("NAUDIT_ROSTER");
    return canonicalize_if_exists(raw);
}

/*
 * Resolve the SYSTEM slot. Honors `NAUDIT_SYSTEM_ROSTER`
 * (test-injection-only override) before the default
 * `/etc/naudit/allowed_signers`. The two env vars are NOT the
 * same: NAUDIT_ROSTER (handled by `resolve_env_slot`) is the
 * top-level user/CI override; NAUDIT_SYSTEM_ROSTER substitutes
 * the SYSTEM-slot path so tests can point at a tmp file
 * without requiring `/etc/` write access.
 */
static n00b_string_t *
resolve_system_slot(void)
{
    n00b_string_t *override_path = read_env_string("NAUDIT_SYSTEM_ROSTER");
    if (override_path) {
        return canonicalize_if_exists(override_path);
    }
    return canonicalize_if_exists(r"/etc/naudit/allowed_signers");
}

/*
 * Resolve the REPO slot. project_root may be nullptr (e.g.,
 * naudit invoked outside a project) — in that case the slot is
 * unavailable.
 */
static n00b_string_t *
resolve_repo_slot(n00b_string_t *project_root)
{
    if (!project_root || project_root->u8_bytes == 0) {
        return nullptr;
    }
    n00b_string_t *abs_root = n00b_path_canonical(project_root);
    if (!abs_root) {
        return nullptr;
    }
    n00b_string_t *candidate = n00b_path_simple_join(
        abs_root, r"audit/allowed_signers");
    return canonicalize_if_exists(candidate);
}

n00b_result_t(n00b_option_t(n00b_string_t *))
n00b_audit_resolve_roster_path(n00b_string_t *project_root)
{
    n00b_string_t *env_path = resolve_env_slot();
    if (env_path) {
        return n00b_result_ok(n00b_option_t(n00b_string_t *),
                              n00b_option_set(n00b_string_t *, env_path));
    }
    n00b_string_t *sys_path = resolve_system_slot();
    if (sys_path) {
        return n00b_result_ok(n00b_option_t(n00b_string_t *),
                              n00b_option_set(n00b_string_t *, sys_path));
    }
    n00b_string_t *repo_path = resolve_repo_slot(project_root);
    if (repo_path) {
        return n00b_result_ok(n00b_option_t(n00b_string_t *),
                              n00b_option_set(n00b_string_t *, repo_path));
    }
    return n00b_result_ok(n00b_option_t(n00b_string_t *),
                          n00b_option_none(n00b_string_t *));
}

n00b_audit_roster_source_t
n00b_audit_roster_source_kind(n00b_string_t *project_root)
{
    if (resolve_env_slot()) {
        return N00B_AUDIT_ROSTER_SOURCE_ENV;
    }
    if (resolve_system_slot()) {
        return N00B_AUDIT_ROSTER_SOURCE_SYSTEM;
    }
    if (resolve_repo_slot(project_root)) {
        return N00B_AUDIT_ROSTER_SOURCE_REPO;
    }
    return N00B_AUDIT_ROSTER_SOURCE_NONE;
}

n00b_audit_roster_resolution_t
n00b_audit_resolve_roster(n00b_string_t *project_root)
{
    n00b_audit_roster_resolution_t out = {
        .path   = nullptr,
        .source = N00B_AUDIT_ROSTER_SOURCE_NONE,
    };
    n00b_string_t *env_path = resolve_env_slot();
    if (env_path) {
        out.path   = env_path;
        out.source = N00B_AUDIT_ROSTER_SOURCE_ENV;
        return out;
    }
    n00b_string_t *sys_path = resolve_system_slot();
    if (sys_path) {
        out.path   = sys_path;
        out.source = N00B_AUDIT_ROSTER_SOURCE_SYSTEM;
        return out;
    }
    n00b_string_t *repo_path = resolve_repo_slot(project_root);
    if (repo_path) {
        out.path   = repo_path;
        out.source = N00B_AUDIT_ROSTER_SOURCE_REPO;
        return out;
    }
    return out;
}

/* ================================================================ */
/* SHA-256                                                           */
/* ================================================================ */

/*
 * Hex-encode a 32-byte digest as a 64-character lowercase
 * `n00b_string_t *`. The conversion uses a fixed lookup table
 * rather than `n00b_cformat` per-byte — the call site is hot
 * enough (per audit invocation) that we prefer direct bytes-out
 * over a printf-style loop.
 */
static n00b_string_t *
digest_to_hex(const uint8_t digest[32])
{
    static const char hex_chars[] = "0123456789abcdef";
    char              buf[64];
    for (int i = 0; i < 32; i++) {
        uint8_t b = digest[i];
        buf[2 * i + 0] = hex_chars[(b >> 4) & 0xf];
        buf[2 * i + 1] = hex_chars[b & 0xf];
    }
    return n00b_string_from_raw(buf, 64);
}

n00b_result_t(n00b_string_t *)
n00b_audit_roster_sha256(n00b_string_t *roster_path)
{
    if (!roster_path) {
        return n00b_result_err(n00b_string_t *,
                               N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND);
    }
    roster_path = n00b_path_canonical(roster_path);

    auto fr = n00b_file_open(roster_path, .kind = N00B_FILE_KIND_MMAP);
    if (n00b_result_is_err(fr)) {
        return n00b_result_err(n00b_string_t *,
                               N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND);
    }
    n00b_file_t *f  = n00b_result_get(fr);
    auto         br = n00b_file_as_buffer(f);
    n00b_file_close(f);
    if (n00b_result_is_err(br)) {
        return n00b_result_err(n00b_string_t *,
                               N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND);
    }
    n00b_buffer_t *buf = n00b_result_get(br);

    /*
     * libn00b's `n00b_sha256_hash` writes the 8-word digest in
     * host-endian uint32 order; to produce the canonical hex
     * representation that `sha256sum` / `openssl dgst -sha256`
     * emit, we read each uint32 big-endian into a byte buffer
     * before hex-encoding. This is the conventional SHA-256
     * output convention.
     */
    n00b_sha256_digest_t hv;
    n00b_sha256_hash(buf->data, (size_t)buf->byte_len, hv);

    uint8_t bytes[32];
    for (int i = 0; i < 8; i++) {
        uint32_t w = hv[i];
        bytes[4 * i + 0] = (uint8_t)((w >> 24) & 0xff);
        bytes[4 * i + 1] = (uint8_t)((w >> 16) & 0xff);
        bytes[4 * i + 2] = (uint8_t)((w >> 8) & 0xff);
        bytes[4 * i + 3] = (uint8_t)(w & 0xff);
    }
    return n00b_result_ok(n00b_string_t *, digest_to_hex(bytes));
}

/*
 * Case-insensitive byte-wise equality on two hex strings. Both
 * inputs are pre-validated to be 64-char lowercase hex by the
 * caller (the directive parser accepts only lowercase hex), but
 * we still tolerate uppercase / mixed-case here so a hand-edited
 * `audit-rules.bnf` with uppercase hex still verifies.
 */
static bool
hex_strings_equal_ci(n00b_string_t *a, n00b_string_t *b)
{
    if (!a || !b) {
        return false;
    }
    if (a->u8_bytes != b->u8_bytes) {
        return false;
    }
    for (size_t i = 0; i < a->u8_bytes; i++) {
        char ca = a->data[i];
        char cb = b->data[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

n00b_result_t(int)
n00b_audit_verify_roster_fingerprint(n00b_string_t *roster_path,
                                      n00b_string_t *expected_sha256_hex)
{
    if (!roster_path || !expected_sha256_hex
        || expected_sha256_hex->u8_bytes == 0) {
        return n00b_result_err(int,
                               N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND);
    }

    auto hr = n00b_audit_roster_sha256(roster_path);
    if (n00b_result_is_err(hr)) {
        return n00b_result_err(int, n00b_result_get_err(hr));
    }
    n00b_string_t *actual = n00b_result_get(hr);

    if (hex_strings_equal_ci(actual, expected_sha256_hex)) {
        return n00b_result_ok(int, 0);
    }
    return n00b_result_ok(int, 1);
}

/* ================================================================ */
/* Rule-file signature wrappers                                      */
/* ================================================================ */

n00b_result_t(int)
n00b_audit_rules_verify_signature(n00b_string_t *audit_rules_path,
                                   n00b_string_t *roster_path,
                                   n00b_string_t *signer_id)
{
    if (!audit_rules_path || !roster_path || !signer_id) {
        return n00b_result_err(int, N00B_AUDIT_ERR_ENGINE_BAD_ARGS);
    }
    /* Path-canonicalization rule. */
    audit_rules_path = n00b_path_canonical(audit_rules_path);
    roster_path      = n00b_path_canonical(roster_path);

    /*
     * Check for the `.sig` sibling first so the loader can emit
     * a distinct "unsigned" warning (graded policy per § 6.3)
     * without going through ssh-keygen.
     */
    n00b_string_t *sig_path = n00b_cformat("«#».sig", audit_rules_path);
    if (!n00b_path_is_file(sig_path)) {
        return n00b_result_ok(int, 1);
    }

    auto vr = n00b_audit_exemption_verify(audit_rules_path, roster_path,
                                          signer_id);
    if (n00b_result_is_err(vr)) {
        int err = n00b_result_get_err(vr);
        switch (err) {
        case N00B_AUDIT_ERR_EXEMPTION_NO_SIGNATURE:
            /*
             * Race: the `.sig` existed at our pre-check but
             * disappeared before ssh-keygen looked. Treat as
             * unsigned.
             */
            return n00b_result_ok(int, 1);
        case N00B_AUDIT_ERR_EXEMPTION_BAD_SIGNATURE:
        case N00B_AUDIT_ERR_EXEMPTION_UNKNOWN_SIGNER:
            return n00b_result_ok(int, 2);
        default:
            return n00b_result_err(int, err);
        }
    }
    return n00b_result_ok(int, 0);
}

n00b_result_t(int)
n00b_audit_sign_rules(n00b_string_t *audit_rules_path,
                       n00b_string_t *key_path,
                       n00b_string_t *signer_id)
{
    if (!audit_rules_path || !key_path || !signer_id) {
        return n00b_result_err(int, N00B_AUDIT_ERR_ENGINE_BAD_ARGS);
    }
    /* Path-canonicalization rule. The WP-012 sign helper also
     * canonicalizes internally; doing it here too is harmless
     * and keeps this wrapper's contract self-documenting. */
    audit_rules_path = n00b_path_canonical(audit_rules_path);
    key_path         = n00b_path_canonical(key_path);
    return n00b_audit_exemption_sign(audit_rules_path, key_path, signer_id);
}
