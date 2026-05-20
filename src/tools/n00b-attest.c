/* src/tools/n00b-attest.c — `n00b-attest` CLI binary entry point.
 *
 * Thin shim around the library-shaped verb cores declared in
 * `include/attest/n00b_attest_cli.h`. Parses argv via
 * slay/commander (D-033 OQ-4), binds stdin / stdout / file paths
 * around the verb cores, and dispatches.
 *
 * Argv shape (per `docs/attest/02-architecture.md` §10 + D-023):
 *
 *     n00b-attest <verb> [args]
 *
 * WP-002 wired the `sign` verb; WP-003 Phase 4 wires the `verify`
 * verb. Other verbs (`inspect`, `push`, `pull`, etc.) are
 * registered as commander subcommands so they appear in `--help`
 * discovery, but each invocation prints a "not yet implemented"
 * diagnostic to stderr and exits 1 (DF-004 disposition (a) per
 * user direction 2026-05-18). Their library-shaped cores arrive
 * in later WPs.
 *
 * The `verify` verb uses a 3-code exit shape (D-044 OQ-1 (b)):
 *   - exit 0 — Ok(true): the envelope verified.
 *   - exit 1 — Ok(false): verdict failure (signature didn't
 *     verify, no matching keyid, empty signatures[], tampered
 *     payload, etc.).
 *   - exit 2 — Err(...): machinery failure (malformed envelope,
 *     scheme mismatch, missing file, etc.).
 *
 * Library-API-first (WP-002 plan §727): every line of business
 * logic lives behind the library surface; this file is purely
 * I/O binding. The regression test in
 * `test/unit/test_attest_cli_sign.c` drives the library core
 * directly via in-memory buffers and does NOT spawn this binary.
 *
 * I/O discipline: per `n00b-api-guidelines.md` §2.10 / §7.4 /
 * §11.3 / §13 and user direction 2026-05-18, this file uses
 * only n00b primitives for I/O — no `<stdio.h>`, no `<errno.h>`,
 * no `fopen` / `fread` / `fwrite` / `fprintf` / `stdin` /
 * `stdout` / `stderr` / `strerror` / `strcmp`. Path-based reads
 * and writes go through `n00b_file_open` + `n00b_file_read` +
 * `n00b_file_write` + `n00b_file_close`; diagnostics go through
 * `n00b_eprintf`.
 *
 * Default-stdout writes go through the runtime-managed fd-1
 * owner via `n00b_stdout()` + `n00b_fd_owner_write` (D-062 / DF-007
 * part B). Default-stdin reads go through the runtime-managed fd-0
 * owner via `n00b_stdin()` + `n00b_fd_owner_read_all`, the
 * read-side analogue of `n00b_fd_owner_write` added alongside the
 * stdout migration. Both helpers reach the same fd owners the
 * conduit substrate manages internally, avoiding a `/dev/std*`
 * `n00b_file_open` dup-and-reparent round-trip.
 */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/runtime.h"
#include "core/alloc.h"
#include "core/file.h"
#include "core/std_streams.h"
#include "conduit/print.h"
#include "conduit/fd_managed.h"
#include "text/strings/string_ops.h"
#include "slay/commander.h"
#include "parsers/json.h"
#include "attest/n00b_attest.h"

// ---------------------------------------------------------------------------
// Forward-roadmap verb list (DF-004 (a)). Each entry surfaces in
// `--help` output via commander registration; invocation routes to
// the "not yet implemented" diagnostic + exit 1.
// ---------------------------------------------------------------------------

typedef struct {
    n00b_string_t *name;
    n00b_string_t *doc;
} verb_stub_t;

// Lazily-initialized table of forward-roadmap stub verbs. The
// table itself is `static`; the `n00b_string_t *` slots are
// populated on first use via `build_pending_verbs()` because
// `r"..."` rich-string literals materialize at runtime through
// the n00b allocator (they are NOT C-language constant
// expressions usable in a `static` initializer).
//
// WP-003 Phase 4 promoted `verify` out of this table to a real
// commander-registered subcommand backed by
// `n00b_attest_cli_verify`; the array shrank 12 → 11. WP-004
// Phase 2 promoted `push` similarly, shrinking the array to 10.
// WP-004 Phase 3 promoted `discover` + `pull`, shrinking it to 8.
static verb_stub_t k_pending_verbs[8];
static size_t      k_num_pending_verbs = 0;

static void
build_pending_verbs(void)
{
    if (k_num_pending_verbs != 0) {
        return;
    }
    size_t i = 0;

    k_pending_verbs[i++] = (verb_stub_t){
        r"inspect",
        r"(not yet implemented) Inspect a DSSE envelope without verifying"};
    k_pending_verbs[i++] = (verb_stub_t){
        r"mark",
        r"(not yet implemented) Mark a binary artifact with an envelope"};
    k_pending_verbs[i++] = (verb_stub_t){
        r"unmark",
        r"(not yet implemented) Remove the attestation mark from an artifact"};
    k_pending_verbs[i++] = (verb_stub_t){
        r"extract",
        r"(not yet implemented) Extract an attestation mark from an artifact"};
    k_pending_verbs[i++] = (verb_stub_t){
        r"harvest",
        r"(not yet implemented) Walk an OCI image's layers for marks"};
    k_pending_verbs[i++] = (verb_stub_t){
        r"setup",
        r"(not yet implemented) Generate / configure a signer backend"};
    k_pending_verbs[i++] = (verb_stub_t){
        r"rotate",
        r"(not yet implemented) Rotate a signer key"};
    k_pending_verbs[i++] = (verb_stub_t){
        r"list-keys",
        r"(not yet implemented) List keys in a keyring directory"};

    k_num_pending_verbs = i;
}

// ---------------------------------------------------------------------------
// stdin / stdout / file binding (the "shim" half of the verb).
// ---------------------------------------------------------------------------

// Read every byte from the given path into a freshly-allocated
// n00b_buffer_t. Used for explicit `--statement <path>` /
// `--envelope <path>` arguments; the default-stdin fallback uses
// `read_all_from_stdin` (the runtime-managed fd-0 owner path).
//
// Returns Ok(buffer) on success; Err(errno) propagated from the
// underlying `n00b_file_open` / `n00b_file_read` failure.
static n00b_result_t(n00b_buffer_t *)
read_all_from_path(n00b_string_t *path)
{
    auto open_r = n00b_file_open(path, .mode = N00B_FILE_R);
    if (n00b_result_is_err(open_r)) {
        return n00b_result_err(n00b_buffer_t *, n00b_result_get_err(open_r));
    }
    n00b_file_t *f = n00b_result_get(open_r);

    n00b_buffer_t *acc = n00b_buffer_new(0);
    while (!n00b_file_at_eof(f)) {
        auto read_r = n00b_file_read(f, 65536);
        if (n00b_result_is_err(read_r)) {
            n00b_err_t code = n00b_result_get_err(read_r);
            n00b_file_close(f);
            return n00b_result_err(n00b_buffer_t *, code);
        }
        n00b_buffer_t *chunk = n00b_result_get(read_r);
        if (chunk == nullptr || chunk->byte_len == 0) {
            break;
        }
        n00b_buffer_concat(acc, chunk);
    }
    n00b_file_close(f);
    return n00b_result_ok(n00b_buffer_t *, acc);
}

// Read every byte from the process's stdin via the runtime-managed
// fd-0 owner. Reaches the same owner the conduit substrate manages
// internally; avoids the `/dev/stdin` `n00b_file_open` indirection
// (which would dup fd 0 and create a parallel owner). D-062 /
// DF-007 part B migration.
//
// `n00b_fd_owner_read_all` subscribes to the owner's read topic,
// blocks on the inbox CV until TOPIC_CLOSED arrives, and returns the
// accumulated bytes as a single buffer. Returns Ok(buffer) on
// success; Err(N00B_CONDUIT_ERR_*) on stdin unavailable or read
// failure.
static n00b_result_t(n00b_buffer_t *)
read_all_from_stdin(void)
{
    n00b_conduit_fd_owner_t *owner = n00b_stdin();
    if (owner == nullptr) {
        return n00b_result_err(n00b_buffer_t *, EBADF);
    }
    return n00b_fd_owner_read_all(owner);
}

// Write every byte of `buf` to the process's stdout via the
// runtime-managed fd-1 owner. Reaches the same owner that
// `n00b_printf(.fd = 1)` reaches; avoids the `/dev/stdout`
// `n00b_file_open` indirection (which would dup fd 1 and create
// a parallel owner). D-062 / DF-007 part B migration.
//
// `n00b_fd_owner_write` internally loops over short writes and
// EAGAIN until either the entry is fully drained or a fatal
// write error surfaces, so a single call is sufficient. Returns
// Ok(true) on success; Err(errno) on stdout unavailable or write
// failure (e.g., EPIPE on a closed downstream).
static n00b_result_t(bool)
write_buffer_to_stdout(n00b_buffer_t *buf)
{
    n00b_conduit_fd_owner_t *owner = n00b_stdout();
    if (owner == nullptr) {
        return n00b_result_err(bool, EBADF);
    }
    if (buf == nullptr || buf->byte_len == 0) {
        return n00b_result_ok(bool, true);
    }
    auto wr = n00b_fd_owner_write(owner, buf->data, buf->byte_len);
    if (n00b_result_is_err(wr)) {
        return n00b_result_err(bool, (int)n00b_result_get_err(wr));
    }
    return n00b_result_ok(bool, true);
}

// Write every byte of `buf` to the given path. Returns Ok(true)
// on success; Err(errno) on any failure. Replaces the prior
// `-1`-sentinel `write_buffer_to_file` shim (W-2 fix).
static n00b_result_t(bool)
write_all_to_path(n00b_string_t *path, n00b_buffer_t *buf)
{
    auto open_r = n00b_file_open(path, .mode = N00B_FILE_W);
    if (n00b_result_is_err(open_r)) {
        return n00b_result_err(bool, n00b_result_get_err(open_r));
    }
    n00b_file_t *f = n00b_result_get(open_r);

    size_t remaining = buf->byte_len;
    const char *p    = (const char *)buf->data;
    while (remaining > 0) {
        auto wr = n00b_file_write(f, p, remaining);
        if (n00b_result_is_err(wr)) {
            n00b_err_t code = n00b_result_get_err(wr);
            n00b_file_close(f);
            return n00b_result_err(bool, code);
        }
        size_t n = n00b_result_get(wr);
        if (n == 0) {
            // No progress + no error → treat as a short-write
            // pseudo-failure rather than spin.
            n00b_file_close(f);
            return n00b_result_err(bool, EIO);
        }
        p         += n;
        remaining -= n;
    }
    n00b_file_close(f);
    return n00b_result_ok(bool, true);
}

// ---------------------------------------------------------------------------
// Commander setup.
// ---------------------------------------------------------------------------

static n00b_cmdr_t *
build_commander(void)
{
    n00b_cmdr_t *c = n00b_cmdr_new();
    n00b_cmdr_set_name(c, r"n00b-attest");

    // Root-level flags.
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--help",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Show help message");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--help", r"-h");

    // sign subcommand.
    n00b_cmdr_add_command(c,
                          r"sign",
                          r"Sign an in-toto Statement into a DSSE envelope");
    n00b_cmdr_add_flag(c,
                       r"sign",
                       r"--key",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Signer URI (e.g., file:///path/to/key.pem)");
    n00b_cmdr_add_flag(c,
                       r"sign",
                       r"--statement",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Read Statement JSON from this path (default: stdin)");
    n00b_cmdr_add_flag(c,
                       r"sign",
                       r"--out",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Write envelope JSON to this path (default: stdout)");

    // verify subcommand (WP-003 Phase 4).
    n00b_cmdr_add_command(c,
                          r"verify",
                          r"Verify a DSSE envelope against a public key");
    n00b_cmdr_add_flag(c,
                       r"verify",
                       r"--key",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Verifier URI (e.g., file:///path/to/pubkey.pem)");
    n00b_cmdr_add_flag(c,
                       r"verify",
                       r"--envelope",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Read envelope JSON from this path (default: stdin)");

    // push subcommand (WP-004 Phase 2).
    n00b_cmdr_add_command(c,
                          r"push",
                          r"Push a DSSE envelope to an OCI 1.1 registry as a referrer");
    n00b_cmdr_add_flag(c,
                       r"push",
                       r"--envelope",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Read envelope JSON from this path (default: stdin)");
    n00b_cmdr_add_flag(c,
                       r"push",
                       r"--image",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"OCI image reference (e.g., ghcr.io/foo/bar@sha256:...)");
    n00b_cmdr_add_flag(c,
                       r"push",
                       r"--registry",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Registry hostname override (e.g., ghcr.io)");

    // discover subcommand (WP-004 Phase 3).
    n00b_cmdr_add_command(c,
                          r"discover",
                          r"List recorded attestations for an OCI image digest");
    n00b_cmdr_add_flag(c,
                       r"discover",
                       r"--image",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"OCI image reference (e.g., ghcr.io/foo/bar@sha256:...)");
    n00b_cmdr_add_flag(c,
                       r"discover",
                       r"--registry",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Registry hostname override (e.g., ghcr.io)");
    n00b_cmdr_add_flag(c,
                       r"discover",
                       r"--artifact-type",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"OCI artifactType filter (default: no filter)");
    n00b_cmdr_add_flag(c,
                       r"discover",
                       r"--json",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Emit JSON instead of human-readable text");

    // pull subcommand (WP-004 Phase 3).
    n00b_cmdr_add_command(c,
                          r"pull",
                          r"Pull a DSSE envelope from an OCI 1.1 registry");
    n00b_cmdr_add_flag(c,
                       r"pull",
                       r"--image",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"OCI image reference (e.g., ghcr.io/foo/bar@sha256:...)");
    n00b_cmdr_add_flag(c,
                       r"pull",
                       r"--predicate-type",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Predicate-type URI to filter on (required)");
    n00b_cmdr_add_flag(c,
                       r"pull",
                       r"--registry",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Registry hostname override (e.g., ghcr.io)");
    n00b_cmdr_add_flag(c,
                       r"pull",
                       r"--out",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Write envelope bytes to this path (default: stdout)");

    // Forward-roadmap stubs — registered so `--help` discovery lists
    // them, but invocation routes through the not-yet-implemented
    // diagnostic in `main` (DF-004 disposition (a)).
    for (size_t i = 0; i < k_num_pending_verbs; i++) {
        n00b_cmdr_add_command(c,
                              k_pending_verbs[i].name,
                              k_pending_verbs[i].doc);
    }

    return c;
}

// ---------------------------------------------------------------------------
// `sign` verb shim — binds I/O around the library core.
// ---------------------------------------------------------------------------

static void
print_attest_error_diagnostic(const char *prefix, n00b_err_t code)
{
    n00b_string_t *prefix_s = n00b_string_from_cstr(prefix);
    n00b_string_t *msg      = n00b_attest_err_str(code);
    if (msg != nullptr && msg->u8_bytes > 0) {
        n00b_eprintf("n00b-attest: «#»: «#» (code «#»)",
                     prefix_s,
                     msg,
                     (int64_t)code);
    }
    else {
        n00b_eprintf("n00b-attest: «#» (code «#»)",
                     prefix_s,
                     (int64_t)code);
    }
}

// n00b currently lacks a canonical `n00b_errno_str(int)` helper
// (a §2.10-clean replacement for libc `strerror`); errno-shaped
// codes returned by `n00b_file_*` primitives are surfaced
// numerically in diagnostics until that helper lands. The gap
// is flagged for orchestrator follow-up at WP-002 closeout.

static int
verb_sign(n00b_cmdr_result_t *result)
{
    // Required: --key.
    if (!n00b_cmdr_flag_present(result, r"--key")) {
        n00b_eprintf("n00b-attest sign: missing required --key <uri>");
        return 1;
    }
    n00b_string_t *key_uri = n00b_cmdr_flag_str(result, r"--key");
    if (key_uri == nullptr || key_uri->u8_bytes == 0) {
        n00b_eprintf("n00b-attest sign: --key <uri> may not be empty");
        return 1;
    }

    // Optional: --statement (default stdin).
    n00b_buffer_t *stmt_bytes = nullptr;
    n00b_string_t *stmt_path  = nullptr;
    if (n00b_cmdr_flag_present(result, r"--statement")) {
        stmt_path = n00b_cmdr_flag_str(result, r"--statement");
        if (stmt_path == nullptr || stmt_path->u8_bytes == 0) {
            n00b_eprintf(
                "n00b-attest sign: --statement <path> may not be empty");
            return 1;
        }

        auto read_r = read_all_from_path(stmt_path);
        if (n00b_result_is_err(read_r)) {
            n00b_err_t code = n00b_result_get_err(read_r);
            n00b_eprintf("n00b-attest sign: cannot read Statement '«#»' "
                         "(errno «#»)",
                         stmt_path,
                         (int64_t)code);
            return 1;
        }
        stmt_bytes = n00b_result_get(read_r);
    }
    else {
        // Default-stdin: reach the runtime-managed fd-0 owner via
        // `n00b_stdin()` + `n00b_fd_owner_read_all` (D-062 / DF-007
        // part B). Same owner the conduit substrate uses internally.
        auto read_r = read_all_from_stdin();
        if (n00b_result_is_err(read_r)) {
            n00b_err_t code = n00b_result_get_err(read_r);
            n00b_eprintf("n00b-attest sign: cannot read Statement from stdin "
                         "(errno «#»)",
                         (int64_t)code);
            return 1;
        }
        stmt_bytes = n00b_result_get(read_r);
    }

    if (stmt_bytes == nullptr || stmt_bytes->byte_len == 0) {
        n00b_eprintf("n00b-attest sign: empty Statement input");
        return 1;
    }

    // Dispatch through the library-shaped verb core.
    auto r = n00b_attest_cli_sign(stmt_bytes, key_uri);
    if (n00b_result_is_err(r)) {
        print_attest_error_diagnostic("sign failed",
                                      n00b_result_get_err(r));
        return 1;
    }
    n00b_buffer_t *envelope_json = n00b_result_get(r);

    // Optional: --out (default stdout). When --out is not given we
    // reach the runtime-managed fd-1 owner directly via
    // `n00b_stdout()` (D-062 / DF-007 part B); when --out IS given
    // we go through the path-based file API.
    if (n00b_cmdr_flag_present(result, r"--out")) {
        n00b_string_t *out_path = n00b_cmdr_flag_str(result, r"--out");
        if (out_path == nullptr || out_path->u8_bytes == 0) {
            n00b_eprintf("n00b-attest sign: --out <path> may not be empty");
            return 1;
        }
        auto wr_r = write_all_to_path(out_path, envelope_json);
        if (n00b_result_is_err(wr_r)) {
            n00b_err_t code = n00b_result_get_err(wr_r);
            n00b_eprintf("n00b-attest sign: cannot write envelope to '«#»' "
                         "(errno «#»)",
                         out_path,
                         (int64_t)code);
            return 1;
        }
    }
    else {
        auto wr_r = write_buffer_to_stdout(envelope_json);
        if (n00b_result_is_err(wr_r)) {
            n00b_err_t code = n00b_result_get_err(wr_r);
            n00b_eprintf("n00b-attest sign: cannot write envelope to stdout "
                         "(errno «#»)",
                         (int64_t)code);
            return 1;
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// `verify` verb shim — binds I/O around the library core.
// ---------------------------------------------------------------------------
//
// 3-code exit shape per D-044 OQ-1 (b):
//   - Ok(true)  → return 0  (verified).
//   - Ok(false) → return 1  (verdict failure).
//   - Err(...)  → return 2  (machinery failure).
//
// `verb_sign` returns `1` on every non-zero path; `verb_verify`
// deliberately keeps the verdict/Err axes distinct so wrapping
// shell scripts can tell signature failure (auditable) apart from
// machinery failure (malformed input / missing file).
static int
verb_verify(n00b_cmdr_result_t *result)
{
    // Required: --key.
    if (!n00b_cmdr_flag_present(result, r"--key")) {
        n00b_eprintf("n00b-attest verify: missing required --key <uri>");
        return 2;
    }
    n00b_string_t *key_uri = n00b_cmdr_flag_str(result, r"--key");
    if (key_uri == nullptr || key_uri->u8_bytes == 0) {
        n00b_eprintf("n00b-attest verify: --key <uri> may not be empty");
        return 2;
    }

    // Optional: --envelope (default stdin). Mirrors the
    // sign-side --statement default per D-033 OQ-5.
    n00b_buffer_t *env_bytes = nullptr;
    if (n00b_cmdr_flag_present(result, r"--envelope")) {
        n00b_string_t *env_path = n00b_cmdr_flag_str(result, r"--envelope");
        if (env_path == nullptr || env_path->u8_bytes == 0) {
            n00b_eprintf(
                "n00b-attest verify: --envelope <path> may not be empty");
            return 2;
        }

        auto read_r = read_all_from_path(env_path);
        if (n00b_result_is_err(read_r)) {
            n00b_err_t code = n00b_result_get_err(read_r);
            n00b_eprintf("n00b-attest verify: cannot read envelope '«#»' "
                         "(errno «#»)",
                         env_path,
                         (int64_t)code);
            return 2;
        }
        env_bytes = n00b_result_get(read_r);
    }
    else {
        // Default-stdin: reach the runtime-managed fd-0 owner via
        // `n00b_stdin()` + `n00b_fd_owner_read_all` (D-062 / DF-007
        // part B). Matches `verb_sign`'s default-stdin handling.
        auto read_r = read_all_from_stdin();
        if (n00b_result_is_err(read_r)) {
            n00b_err_t code = n00b_result_get_err(read_r);
            n00b_eprintf("n00b-attest verify: cannot read envelope from stdin "
                         "(errno «#»)",
                         (int64_t)code);
            return 2;
        }
        env_bytes = n00b_result_get(read_r);
    }

    if (env_bytes == nullptr || env_bytes->byte_len == 0) {
        n00b_eprintf("n00b-attest verify: empty envelope input");
        return 2;
    }

    // Dispatch through the library-shaped verb core.
    auto r = n00b_attest_cli_verify(env_bytes, key_uri);
    if (n00b_result_is_err(r)) {
        // Machinery failure → exit 2.
        print_attest_error_diagnostic("verify failed",
                                      n00b_result_get_err(r));
        return 2;
    }

    if (n00b_result_get(r) == true) {
        // Verdict-true → exit 0 (no stdout output; verify produces
        // no envelope artifact, only a verdict).
        return 0;
    }

    // Verdict-false → exit 1. Emit a structured stderr line so
    // log scrapers can distinguish the "the signature did not
    // verify" case from the silent-pass.
    n00b_eprintf("n00b-attest verify: signature did not verify");
    return 1;
}

// ---------------------------------------------------------------------------
// `push` verb shim — binds I/O around the library core.
// ---------------------------------------------------------------------------
//
// Two-code exit shape per §10.1 (verbs that return only Ok/Err):
//   - Ok(manifest_digest) → return 0 (print digest to stdout).
//   - Err(...)            → return 1 (structured diagnostic to stderr).
//
// No verdict shape here — the push either uploaded the referrer
// or it didn't. There's no "signed but didn't verify" axis on the
// producer side.
static int
verb_push(n00b_cmdr_result_t *result)
{
    // Required: --image.
    if (!n00b_cmdr_flag_present(result, r"--image")) {
        n00b_eprintf("n00b-attest push: missing required --image <ref>");
        return 1;
    }
    n00b_string_t *image_ref = n00b_cmdr_flag_str(result, r"--image");
    if (image_ref == nullptr || image_ref->u8_bytes == 0) {
        n00b_eprintf("n00b-attest push: --image <ref> may not be empty");
        return 1;
    }

    // Optional: --registry override.
    n00b_string_t *registry_override = nullptr;
    if (n00b_cmdr_flag_present(result, r"--registry")) {
        registry_override = n00b_cmdr_flag_str(result, r"--registry");
        if (registry_override == nullptr || registry_override->u8_bytes == 0) {
            n00b_eprintf(
                "n00b-attest push: --registry <host> may not be empty");
            return 1;
        }
    }

    // Optional: --envelope (default stdin).
    n00b_buffer_t *env_bytes = nullptr;
    if (n00b_cmdr_flag_present(result, r"--envelope")) {
        n00b_string_t *env_path = n00b_cmdr_flag_str(result, r"--envelope");
        if (env_path == nullptr || env_path->u8_bytes == 0) {
            n00b_eprintf(
                "n00b-attest push: --envelope <path> may not be empty");
            return 1;
        }

        auto read_r = read_all_from_path(env_path);
        if (n00b_result_is_err(read_r)) {
            n00b_err_t code = n00b_result_get_err(read_r);
            n00b_eprintf("n00b-attest push: cannot read envelope '«#»' "
                         "(errno «#»)",
                         env_path,
                         (int64_t)code);
            return 1;
        }
        env_bytes = n00b_result_get(read_r);
    }
    else {
        // Default-stdin: reach the runtime-managed fd-0 owner via
        // `n00b_stdin()` + `n00b_fd_owner_read_all` (D-062 / DF-007
        // part B). Matches verb_sign / verb_verify's default-stdin path.
        auto read_r = read_all_from_stdin();
        if (n00b_result_is_err(read_r)) {
            n00b_err_t code = n00b_result_get_err(read_r);
            n00b_eprintf("n00b-attest push: cannot read envelope from stdin "
                         "(errno «#»)",
                         (int64_t)code);
            return 1;
        }
        env_bytes = n00b_result_get(read_r);
    }

    if (env_bytes == nullptr || env_bytes->byte_len == 0) {
        n00b_eprintf("n00b-attest push: empty envelope input");
        return 1;
    }

    // Dispatch through the library-shaped verb core.
    auto r = n00b_attest_cli_push(env_bytes,
                                   image_ref,
                                   .registry_override = registry_override);
    if (n00b_result_is_err(r)) {
        print_attest_error_diagnostic("push failed",
                                      n00b_result_get_err(r));
        return 1;
    }
    n00b_string_t *manifest_digest = n00b_result_get(r);

    // Print the manifest digest to stdout (one line, `sha256:<hex>
    // \n`). `.fd = 1` is explicit so ncc's transform packs the
    // leading variadic args into the vargs struct — bare
    // `n00b_printf(fmt, arg)` calls bypass the transform under the
    // current ncc release (D-043 retrospective finding).
    n00b_printf("«#»", manifest_digest, .fd = 1);
    return 0;
}

// ---------------------------------------------------------------------------
// `discover` verb shim — binds I/O around the library core.
// ---------------------------------------------------------------------------
//
// Two-code exit shape: Ok(list) → 0 (even when list is empty);
// Err → 1 with structured diagnostic. Empty list emits the
// "no attestations found" line on stdout (not stderr) so the
// output channel is consistent across populated / empty cases.
static int
verb_discover(n00b_cmdr_result_t *result)
{
    if (!n00b_cmdr_flag_present(result, r"--image")) {
        n00b_eprintf("n00b-attest discover: missing required --image <ref>");
        return 1;
    }
    n00b_string_t *image_ref = n00b_cmdr_flag_str(result, r"--image");
    if (image_ref == nullptr || image_ref->u8_bytes == 0) {
        n00b_eprintf("n00b-attest discover: --image <ref> may not be empty");
        return 1;
    }

    n00b_string_t *registry_override = nullptr;
    if (n00b_cmdr_flag_present(result, r"--registry")) {
        registry_override = n00b_cmdr_flag_str(result, r"--registry");
        if (registry_override == nullptr
            || registry_override->u8_bytes == 0) {
            n00b_eprintf("n00b-attest discover: --registry <host> may not be empty");
            return 1;
        }
    }

    n00b_string_t *artifact_type = nullptr;
    if (n00b_cmdr_flag_present(result, r"--artifact-type")) {
        artifact_type = n00b_cmdr_flag_str(result, r"--artifact-type");
        if (artifact_type == nullptr || artifact_type->u8_bytes == 0) {
            n00b_eprintf("n00b-attest discover: --artifact-type may not be empty");
            return 1;
        }
    }

    bool json_out = n00b_cmdr_flag_bool(result, r"--json");

    auto r = n00b_attest_cli_discover(image_ref,
                                       .registry_override = registry_override,
                                       .artifact_type     = artifact_type);
    if (n00b_result_is_err(r)) {
        print_attest_error_diagnostic("discover failed",
                                      n00b_result_get_err(r));
        return 1;
    }
    n00b_list_t(n00b_attest_oci_referrer_t *) *refs = n00b_result_get(r);

    if (json_out) {
        // Build a canonical (compact) JSON array of objects via the
        // n00b JSON encoder. Per D-024 the wire form is .pretty=false.
        n00b_json_node_t *arr = n00b_json_array_new();
        size_t nrefs = refs->len;
        for (size_t i = 0; i < nrefs; i++) {
            n00b_attest_oci_referrer_t *e = refs->data[i];
            if (e == nullptr) {
                continue;
            }
            n00b_json_node_t *obj = n00b_json_object_new();
            if (e->manifest_digest != nullptr) {
                // Materialize NUL-terminated copies for the JSON
                // encoder (its string-set primitive expects C strings).
                char *mdg = n00b_alloc_array(char,
                                             e->manifest_digest->u8_bytes + 1);
                memcpy(mdg,
                       e->manifest_digest->data,
                       e->manifest_digest->u8_bytes);
                mdg[e->manifest_digest->u8_bytes] = '\0';
                n00b_json_object_put(obj, "manifest_digest",
                                     n00b_json_string_new(mdg));
            }
            if (e->predicate_type != nullptr) {
                char *pt = n00b_alloc_array(char,
                                            e->predicate_type->u8_bytes + 1);
                memcpy(pt,
                       e->predicate_type->data,
                       e->predicate_type->u8_bytes);
                pt[e->predicate_type->u8_bytes] = '\0';
                n00b_json_object_put(obj, "predicate_type",
                                     n00b_json_string_new(pt));
            }
            if (e->signer_keyid != nullptr) {
                char *sk = n00b_alloc_array(char,
                                            e->signer_keyid->u8_bytes + 1);
                memcpy(sk,
                       e->signer_keyid->data,
                       e->signer_keyid->u8_bytes);
                sk[e->signer_keyid->u8_bytes] = '\0';
                n00b_json_object_put(obj, "signer_keyid",
                                     n00b_json_string_new(sk));
            }
            n00b_json_array_push(arr, obj);
        }
        char *encoded = n00b_json_encode(arr, .pretty = false);
        n00b_string_t *enc_s = encoded != nullptr
                                   ? n00b_string_from_cstr(encoded)
                                   : r"[]";
        n00b_printf("«#»", enc_s, .fd = 1);
        return 0;
    }

    if (refs->len == 0) {
        n00b_printf("no attestations found", .fd = 1);
        return 0;
    }

    // Plain text: one line per referrer, tab-separated columns.
    for (size_t i = 0; i < refs->len; i++) {
        n00b_attest_oci_referrer_t *e = refs->data[i];
        if (e == nullptr) {
            continue;
        }
        n00b_string_t *mdg = e->manifest_digest != nullptr
                                 ? e->manifest_digest
                                 : r"(missing)";
        n00b_string_t *pt = e->predicate_type != nullptr
                                ? e->predicate_type
                                : r"(missing)";
        n00b_string_t *sk = e->signer_keyid != nullptr
                                ? e->signer_keyid
                                : r"(missing)";
        n00b_printf("«#»\t«#»\t«#»", mdg, pt, sk, .fd = 1);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// `pull` verb shim — binds I/O around the library core.
// ---------------------------------------------------------------------------
//
// Two-code exit shape: Ok(envelope) → 0 (write bytes to --out or
// stdout); Err → 1 with structured diagnostic.
static int
verb_pull(n00b_cmdr_result_t *result)
{
    if (!n00b_cmdr_flag_present(result, r"--image")) {
        n00b_eprintf("n00b-attest pull: missing required --image <ref>");
        return 1;
    }
    n00b_string_t *image_ref = n00b_cmdr_flag_str(result, r"--image");
    if (image_ref == nullptr || image_ref->u8_bytes == 0) {
        n00b_eprintf("n00b-attest pull: --image <ref> may not be empty");
        return 1;
    }

    if (!n00b_cmdr_flag_present(result, r"--predicate-type")) {
        n00b_eprintf("n00b-attest pull: missing required --predicate-type <URI>");
        return 1;
    }
    n00b_string_t *predicate_type = n00b_cmdr_flag_str(result,
                                                       r"--predicate-type");
    if (predicate_type == nullptr || predicate_type->u8_bytes == 0) {
        n00b_eprintf("n00b-attest pull: --predicate-type may not be empty");
        return 1;
    }

    n00b_string_t *registry_override = nullptr;
    if (n00b_cmdr_flag_present(result, r"--registry")) {
        registry_override = n00b_cmdr_flag_str(result, r"--registry");
        if (registry_override == nullptr
            || registry_override->u8_bytes == 0) {
            n00b_eprintf("n00b-attest pull: --registry <host> may not be empty");
            return 1;
        }
    }

    auto r = n00b_attest_cli_pull(image_ref,
                                   predicate_type,
                                   .registry_override = registry_override);
    if (n00b_result_is_err(r)) {
        print_attest_error_diagnostic("pull failed",
                                      n00b_result_get_err(r));
        return 1;
    }
    n00b_buffer_t *envelope = n00b_result_get(r);

    // Default-stdout: runtime fd-1 owner (D-062 / DF-007 part B).
    // Path-based --out goes through the file API as before.
    if (n00b_cmdr_flag_present(result, r"--out")) {
        n00b_string_t *out_path = n00b_cmdr_flag_str(result, r"--out");
        if (out_path == nullptr || out_path->u8_bytes == 0) {
            n00b_eprintf("n00b-attest pull: --out <path> may not be empty");
            return 1;
        }
        auto wr_r = write_all_to_path(out_path, envelope);
        if (n00b_result_is_err(wr_r)) {
            n00b_err_t code = n00b_result_get_err(wr_r);
            n00b_eprintf("n00b-attest pull: cannot write envelope to '«#»' "
                         "(errno «#»)",
                         out_path,
                         (int64_t)code);
            return 1;
        }
    }
    else {
        auto wr_r = write_buffer_to_stdout(envelope);
        if (n00b_result_is_err(wr_r)) {
            n00b_err_t code = n00b_result_get_err(wr_r);
            n00b_eprintf("n00b-attest pull: cannot write envelope to stdout "
                         "(errno «#»)",
                         (int64_t)code);
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// --help / usage text.
// ---------------------------------------------------------------------------

static void
print_usage(void)
{
    n00b_printf(
        "Usage: n00b-attest <verb> [args]\n"
        "\n"
        "Verbs:\n"
        "  sign       Sign an in-toto Statement into a DSSE envelope\n"
        "  verify     Verify a DSSE envelope against a public key\n"
        "  push       Push a DSSE envelope to an OCI 1.1 registry as a referrer\n"
        "  discover   List recorded attestations for an OCI image digest\n"
        "  pull       Pull a DSSE envelope from an OCI 1.1 registry");
    for (size_t i = 0; i < k_num_pending_verbs; i++) {
        n00b_string_t *vname = k_pending_verbs[i].name;
        n00b_string_t *vdoc  = k_pending_verbs[i].doc;
        // `.fd = 1` (stdout) is explicit so ncc's transform sees a
        // kwarg and packs the leading variadic args into the vargs
        // struct — bare `n00b_printf(fmt, a, b)` calls bypass the
        // transform under the current ncc release.
        n00b_printf("  «#» «#»", vname, vdoc, .fd = 1);
    }
    n00b_printf(
        "\n"
        "sign flags:\n"
        "  --key <uri>        Signer URI (required, e.g. "
        "file:///path/to/key.pem)\n"
        "  --statement <path> Read Statement JSON from path "
        "(default: stdin)\n"
        "  --out <path>       Write envelope JSON to path "
        "(default: stdout)\n"
        "\n"
        "verify flags:\n"
        "  --key <uri>        Verifier URI (required, e.g. "
        "file:///path/to/pubkey.pem)\n"
        "  --envelope <path>  Read envelope JSON from path "
        "(default: stdin)\n"
        "\n"
        "verify exit codes:\n"
        "  0  verified\n"
        "  1  verdict failure (signature did not verify)\n"
        "  2  machinery failure (malformed envelope, missing key, "
        "etc.)\n"
        "\n"
        "push flags:\n"
        "  --image <ref>      OCI image reference (required, e.g. "
        "ghcr.io/foo/bar@sha256:...)\n"
        "  --envelope <path>  Read envelope JSON from path "
        "(default: stdin)\n"
        "  --registry <host>  Registry hostname override (e.g. ghcr.io)\n"
        "\n"
        "discover flags:\n"
        "  --image <ref>           OCI image reference (required)\n"
        "  --registry <host>       Registry hostname override\n"
        "  --artifact-type <type>  Server-side artifactType filter\n"
        "  --json                  Emit JSON instead of text\n"
        "\n"
        "pull flags:\n"
        "  --image <ref>            OCI image reference (required)\n"
        "  --predicate-type <URI>   Predicate-type to filter on (required)\n"
        "  --registry <host>        Registry hostname override\n"
        "  --out <path>             Write envelope bytes to path "
        "(default: stdout)\n"
        "\n"
        "Global flags:\n"
        "  -h, --help         Show this help message");
}

// ---------------------------------------------------------------------------
// main.
// ---------------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);
    n00b_attest_module_init();
    build_pending_verbs();

    n00b_cmdr_t *cmdr = build_commander();

    // No arguments → print help and exit 0 (commander convention
    // matching the `n00b` tool's bare-invocation handling at
    // src/tools/n00b.c:1995, but routed to help rather than a
    // default verb because n00b-attest has no sensible default
    // action).
    if (argc <= 1) {
        print_usage();
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 0;
    }

    n00b_cmdr_result_t *result = n00b_cmdr_parse(cmdr,
                                                 argc - 1,
                                                 (const char **)&argv[1]);
    if (result == nullptr || !result->ok) {
        int32_t nerr = n00b_cmdr_error_count(result);
        for (int32_t i = 0; i < nerr; i++) {
            n00b_string_t *err = n00b_cmdr_error_get(result, i);
            n00b_eprintf("n00b-attest: «#»", err);
        }
        print_usage();
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 1;
    }

    if (n00b_cmdr_flag_bool(result, r"--help")) {
        print_usage();
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 0;
    }

    n00b_string_t *cmd = n00b_cmdr_result_command(result);
    if (cmd == nullptr || cmd->u8_bytes == 0) {
        // Bare invocation with flags but no verb. Treat as help.
        print_usage();
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 0;
    }

    int rc;
    n00b_string_t *sign_name     = r"sign";
    n00b_string_t *verify_name   = r"verify";
    n00b_string_t *push_name     = r"push";
    n00b_string_t *discover_name = r"discover";
    n00b_string_t *pull_name     = r"pull";
    if (n00b_unicode_str_eq(cmd, sign_name)) {
        rc = verb_sign(result);
    }
    else if (n00b_unicode_str_eq(cmd, verify_name)) {
        rc = verb_verify(result);
    }
    else if (n00b_unicode_str_eq(cmd, push_name)) {
        rc = verb_push(result);
    }
    else if (n00b_unicode_str_eq(cmd, discover_name)) {
        rc = verb_discover(result);
    }
    else if (n00b_unicode_str_eq(cmd, pull_name)) {
        rc = verb_pull(result);
    }
    else {
        // DF-004 (a): the verb is in the forward roadmap (registered
        // with commander so it surfaces in --help) but its library
        // core arrives in a later WP. Diagnose and exit 1.
        bool known_stub = false;
        for (size_t i = 0; i < k_num_pending_verbs; i++) {
            if (n00b_unicode_str_eq(cmd, k_pending_verbs[i].name)) {
                known_stub = true;
                break;
            }
        }
        if (known_stub) {
            n00b_eprintf("n00b-attest: '«#»' is not yet implemented in this "
                         "build",
                         cmd);
        }
        else {
            n00b_eprintf("n00b-attest: unknown verb '«#»'", cmd);
        }
        rc = 1;
    }

    n00b_cmdr_result_free(result);
    n00b_cmdr_free(cmdr);
    n00b_shutdown();
    return rc;
}
