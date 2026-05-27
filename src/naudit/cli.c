/*
 * WP-001 Phase 4 — CLI driver (library entry).
 *
 * Implements `n00b_audit_run_cli`, the library-shaped CLI entry
 * declared in `include/audit/audit.h`. Wires up libn00b's
 * `slay/commander.h`-based argv parser, resolves the guidance file
 * (flag override OR discovery walk from cwd), loads guidance,
 * builds the engine, runs `check_file` on the positional target,
 * and emits terminal violations to stdout via
 * `n00b_audit_print_terminal`.
 *
 * The thin `main()` in `src/tools/n00b_audit.c` is a wrapper that
 * converts `char *argv[]` to `n00b_string_t *audit_argv[]`,
 * invokes `n00b_audit_run_cli`, and translates the result into the
 * process exit code.
 *
 * Exit-code contract (ok-branch int payload):
 *   - 0  no violations found.
 *   - 1  at least one violation.
 *   - 2  internal error (with a stderr diagnostic already emitted).
 *
 * Per project DECISIONS.md D-005, this implementation's public
 * function carries no `_kargs` block. Per D-008, null-pointer
 * guards use the `!ptr` boolean-conversion idiom.
 *
 * Capture-mechanism note. Diagnostics go through libn00b's stderr
 * topic via `n00b_eprintf` / `n00b_print(.., .fd = 2)`. The Phase
 * 4 regression test captures both fds via POSIX `pipe()` +
 * `dup2()` before invoking `n00b_audit_run_cli`; see `output.c`
 * for the W-4 capture-mechanism rationale.
 *
 * NCC.md "Rich string literals" gotcha: `r"..."` literals cannot
 * carry `«#»` substitution markers or use C-style adjacent-literal
 * concatenation. Format templates with substitutions in this file
 * are plain C strings passed to `n00b_eprintf` / `n00b_printf`.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "adt/list.h"
#include "adt/result.h"
#include "conduit/print.h"
#include "slay/commander.h"
#include "text/strings/format.h"
#include "text/strings/string_ops.h"
#include "util/path.h"

#include "naudit/naudit.h"
#include "naudit/engine.h"
#include "naudit/errors.h"
#include "naudit/guidance.h"
#include "naudit/output.h"
#include "naudit/violation.h"

/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

/*
 * Build the commander spec for the `n00b-audit` CLI:
 *   - one positional `file` (required, exactly one),
 *   - `--guidance <path>` flag (optional),
 *   - `--format <terminal|json>` flag (optional, default terminal).
 *
 * Returns a finalized commander, or nullptr on internal failure.
 */
static n00b_cmdr_t *
build_cmdr(void)
{
    n00b_cmdr_t *c = n00b_cmdr_new();
    if (!c) {
        return nullptr;
    }
    n00b_cmdr_set_name(c, n00b_string_from_cstr("n00b-audit"));

    n00b_string_t *empty = n00b_string_from_cstr("");

    /* One positional `file`, min=1, max=1 — exactly one target. */
    n00b_cmdr_add_positional(c, empty,
                             n00b_string_from_cstr("file"),
                             N00B_CMDR_TYPE_WORD, 1, 1);

    /* --guidance <path> */
    n00b_cmdr_add_flag(c, empty,
                       n00b_string_from_cstr("--guidance"),
                       N00B_CMDR_TYPE_WORD, true,
                       n00b_string_from_cstr(
                           "Override the discovery walk and load this guidance file."));

    /* --format <terminal|json> */
    n00b_cmdr_add_flag(c, empty,
                       n00b_string_from_cstr("--format"),
                       N00B_CMDR_TYPE_WORD, true,
                       n00b_string_from_cstr(
                           "Output format: 'terminal' (default) or 'json' (Phase 5+)."));

    n00b_cmdr_finalize(c);
    return c;
}

/*
 * Convert the n00b_string_t-shaped argv we accept on the library
 * surface into the const-char-pointer array commander expects.
 * Allocates via libn00b's GC (zero-initialized). Returns nullptr on
 * any null entry in argv (treated as a bad-args programming error).
 *
 * `argc_out` is set to argc-1 because commander conventionally
 * receives argv+1 (the program name is implicit).
 *
 * `n00b_string_t` data fields are NUL-terminated per the Phase 2
 * loader's precedent (`require_string_field` in guidance.c uses
 * `n00b_string_from_cstr` whose output is documented as
 * NUL-terminated), so passing `s->data` as `const char *` to
 * commander is safe.
 */
static const char **
build_cargv(int argc, n00b_string_t *argv[], int *argc_out)
{
    if (argc <= 0 || !argv) {
        return nullptr;
    }
    /* commander conventionally receives argv+1 (program name is
     * implicit). With argc==1 we pass 0 entries — commander will
     * then complain about the missing positional, which is the
     * correct behavior. */
    int n = argc - 1;
    if (n < 0) {
        n = 0;
    }

    const char **out = n00b_alloc_array(const char *, (size_t)(n + 1));
    for (int i = 0; i < n; i++) {
        n00b_string_t *s = argv[i + 1];
        if (!s) {
            return nullptr;
        }
        out[i] = (const char *)s->data;
    }
    out[n] = nullptr;
    *argc_out = n;
    return out;
}

/*
 * Compose a stderr diagnostic combining a short prefix and a
 * n00b_audit_err_str describing the error code, then return ok-2.
 */
static n00b_result_t(int)
diagnose_and_fail(const char *prefix, int err_code)
{
    n00b_eprintf("n00b-audit: «#»: «#»",
                 n00b_string_from_cstr(prefix),
                 n00b_audit_err_str(err_code));
    return n00b_result_ok(int, 2);
}

/* ---------------------------------------------------------------- */
/* Public entry                                                     */
/* ---------------------------------------------------------------- */

n00b_result_t(int)
n00b_audit_run_cli(int argc, n00b_string_t *argv[])
{
    if (argc <= 0 || !argv) {
        return n00b_result_err(int, N00B_AUDIT_ERR_CLI_BAD_ARGS);
    }

    /* Build + parse the commander spec. */
    n00b_cmdr_t *c = build_cmdr();
    if (!c) {
        n00b_eprintf("n00b-audit: «#»",
                     n00b_string_from_cstr(
                         "internal error: failed to build commander spec"));
        return n00b_result_ok(int, 2);
    }

    int cargc = 0;
    const char **cargv = build_cargv(argc, argv, &cargc);
    if (!cargv) {
        return n00b_result_err(int, N00B_AUDIT_ERR_CLI_BAD_ARGS);
    }

    n00b_cmdr_result_t *parse = n00b_cmdr_parse(c, cargc, cargv);
    if (!parse || !parse->ok) {
        if (parse) {
            int32_t errn = n00b_cmdr_error_count(parse);
            for (int32_t i = 0; i < errn; i++) {
                n00b_eprintf("n00b-audit: «#»",
                             n00b_cmdr_error_get(parse, i));
            }
        }
        n00b_eprintf("n00b-audit: «#»",
                     n00b_string_from_cstr(
                         "usage: n00b-audit [--guidance <path>] [--format terminal|json] <file>"));
        if (parse) {
            n00b_cmdr_result_free(parse);
        }
        return n00b_result_err(int, N00B_AUDIT_ERR_CLI_ARGS);
    }

    /* Positional `file` is required (min=1) so the parser would have
     * rejected the argv if it were absent. Defensive guard anyway. */
    if (n00b_cmdr_arg_count(parse) < 1) {
        n00b_cmdr_result_free(parse);
        return n00b_result_err(int, N00B_AUDIT_ERR_CLI_ARGS);
    }
    n00b_string_t *target = n00b_cmdr_arg_str(parse, 0);
    if (!target || target->u8_bytes == 0) {
        n00b_cmdr_result_free(parse);
        return n00b_result_err(int, N00B_AUDIT_ERR_CLI_ARGS);
    }

    /* --format handling. terminal (default) and json are the v1
     * values; everything else is rejected. The Phase 4 stub that
     * stderr-diagnosed json + returned ok-2 has been replaced (in
     * Phase 5) by a real dispatch to `n00b_audit_print_json` below;
     * here we just validate the flag value and remember the choice. */
    n00b_string_t *fmt_flag     = n00b_string_from_cstr("--format");
    n00b_string_t *fmt_terminal = n00b_string_from_cstr("terminal");
    n00b_string_t *fmt_json     = n00b_string_from_cstr("json");
    n00b_string_t *fmt = nullptr;
    if (n00b_cmdr_flag_present(parse, fmt_flag)) {
        fmt = n00b_cmdr_flag_str(parse, fmt_flag);
    }
    bool want_json = false;
    if (fmt && fmt->u8_bytes > 0) {
        if (n00b_unicode_str_eq(fmt, fmt_json)) {
            want_json = true;
        }
        else if (!n00b_unicode_str_eq(fmt, fmt_terminal)) {
            n00b_eprintf("n00b-audit: unrecognized --format value: «#» (use terminal or json)",
                         fmt);
            n00b_cmdr_result_free(parse);
            return n00b_result_err(int, N00B_AUDIT_ERR_CLI_ARGS);
        }
    }

    /* Resolve the guidance file path. */
    n00b_string_t *guidance_flag = n00b_string_from_cstr("--guidance");
    n00b_string_t *guidance_path = nullptr;
    if (n00b_cmdr_flag_present(parse, guidance_flag)) {
        guidance_path = n00b_cmdr_flag_str(parse, guidance_flag);
    }
    if (!guidance_path || guidance_path->u8_bytes == 0) {
        n00b_string_t *cwd = n00b_get_current_directory();
        if (!cwd) {
            n00b_cmdr_result_free(parse);
            return diagnose_and_fail(
                "cwd lookup failed",
                N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND);
        }
        auto disc = n00b_audit_find_guidance_file(cwd);
        if (n00b_result_is_err(disc)) {
            n00b_cmdr_result_free(parse);
            return diagnose_and_fail(
                "guidance discovery",
                n00b_result_get_err(disc));
        }
        guidance_path = n00b_result_get(disc);
    }

    /* Load guidance. */
    auto gr = n00b_audit_load_guidance(guidance_path);
    if (n00b_result_is_err(gr)) {
        n00b_cmdr_result_free(parse);
        return diagnose_and_fail("load guidance",
                                 n00b_result_get_err(gr));
    }
    n00b_audit_guidance_t *guidance = n00b_result_get(gr);

    /* Build engine. */
    auto er = n00b_audit_engine_new(guidance);
    if (n00b_result_is_err(er)) {
        n00b_cmdr_result_free(parse);
        return diagnose_and_fail("engine build",
                                 n00b_result_get_err(er));
    }
    n00b_audit_engine_t *engine = n00b_result_get(er);

    /* Check the target. */
    auto cr = n00b_audit_engine_check_file(engine, target);
    if (n00b_result_is_err(cr)) {
        n00b_cmdr_result_free(parse);
        return diagnose_and_fail("check file",
                                 n00b_result_get_err(cr));
    }
    n00b_list_t(n00b_audit_violation_t *) *violations = n00b_result_get(cr);

    /* Render — dispatch on `--format`. Both renderers share the
     * same return-shape contract; on err, emit a stderr diagnostic
     * and return ok-2 (the W-4 internal-error path). */
    n00b_result_t(int) pr = want_json
                                ? n00b_audit_print_json(violations, guidance)
                                : n00b_audit_print_terminal(violations, guidance);
    if (n00b_result_is_err(pr)) {
        n00b_cmdr_result_free(parse);
        return diagnose_and_fail("render", n00b_result_get_err(pr));
    }

    int exit_code = (n00b_list_len(*violations) == 0) ? 0 : 1;
    n00b_cmdr_result_free(parse);
    return n00b_result_ok(int, exit_code);
}
