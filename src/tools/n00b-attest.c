/* src/tools/n00b-attest.c — `n00b-attest` CLI binary entry point.
 *
 * Thin shim around the library-shaped verb cores declared in
 * `include/attest/n00b_attest_cli.h`. Parses argv via
 * slay/commander (D-033 OQ-4), binds stdin / stdout / file paths
 * around the verb core, and dispatches.
 *
 * Argv shape (per `docs/attest/02-architecture.md` §10 + D-023):
 *
 *     n00b-attest <verb> [args]
 *
 * Only the `sign` verb is wired in WP-002. Other verbs (`verify`,
 * `inspect`, `push`, `pull`, etc.) are registered as commander
 * subcommands so they appear in `--help` discovery, but each
 * invocation prints a "not yet implemented" diagnostic to stderr
 * and exits 1 (DF-004 disposition (a) per user direction
 * 2026-05-18). Their library-shaped cores arrive in later WPs.
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
 * `n00b_file_write` + `n00b_file_close`; stdin and stdout are
 * accessed via the POSIX `/dev/stdin` and `/dev/stdout` paths
 * fed into the same primitives (n00b has no canonical
 * stdin/stdout helper yet — see flagged_for_orchestrator at
 * WP-002 closeout); diagnostics go through `n00b_eprintf`.
 */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/runtime.h"
#include "core/alloc.h"
#include "core/file.h"
#include "conduit/print.h"
#include "text/strings/string_ops.h"
#include "slay/commander.h"
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
static verb_stub_t k_pending_verbs[12];
static size_t      k_num_pending_verbs = 0;

static void
build_pending_verbs(void)
{
    if (k_num_pending_verbs != 0) {
        return;
    }
    size_t i = 0;

    k_pending_verbs[i++] = (verb_stub_t){
        r"verify",
        r"(not yet implemented) Verify a DSSE envelope against a keyring"};
    k_pending_verbs[i++] = (verb_stub_t){
        r"inspect",
        r"(not yet implemented) Inspect a DSSE envelope without verifying"};
    k_pending_verbs[i++] = (verb_stub_t){
        r"push",
        r"(not yet implemented) Push an envelope to an OCI 1.1 registry"};
    k_pending_verbs[i++] = (verb_stub_t){
        r"pull",
        r"(not yet implemented) Pull an envelope from an OCI 1.1 registry"};
    k_pending_verbs[i++] = (verb_stub_t){
        r"discover",
        r"(not yet implemented) Discover envelopes on an OCI registry"};
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
// n00b_buffer_t. Used both for `--statement <path>` and (when
// `path` is `/dev/stdin`) for the default stdin fallback.
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
    }
    else {
        // Fallback path-based stdin access. n00b currently lacks
        // a canonical stdin-reading helper; `/dev/stdin` is
        // POSIX-portable across macOS and Linux (the two
        // platforms n00b targets) and dispatches through the
        // same `n00b_file_open` machinery as any other path.
        // Tracking the ergonomics gap as a deferral at WP-002
        // closeout.
        stmt_path = n00b_string_from_cstr("/dev/stdin");
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

    // Optional: --out (default stdout).
    n00b_string_t *out_path = nullptr;
    if (n00b_cmdr_flag_present(result, r"--out")) {
        out_path = n00b_cmdr_flag_str(result, r"--out");
        if (out_path == nullptr || out_path->u8_bytes == 0) {
            n00b_eprintf("n00b-attest sign: --out <path> may not be empty");
            return 1;
        }
    }
    else {
        // Fallback path-based stdout access — same rationale as
        // the stdin fallback above.
        out_path = n00b_string_from_cstr("/dev/stdout");
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
        "  sign       Sign an in-toto Statement into a DSSE envelope");
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
    n00b_string_t *sign_name = r"sign";
    if (n00b_unicode_str_eq(cmd, sign_name)) {
        rc = verb_sign(result);
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
