// n00b.c — Main n00b CLI entry point.
//
// Provides compile, run, and repl subcommands via the commander module.
// Default mode (no subcommand) is repl.
//
// The "run" and default paths load the n00b grammar, parse each source
// file, run the annotation walk, codegen via MIR, JIT-compile, and
// execute the first file's _main.

#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "core/string.h"
#include "conduit/print.h"
#include "parsers/scanner.h"
#include "parsers/token_stream.h"
#include "slay/annot_walk.h"
#include "slay/bnf.h"
#include "slay/codegen.h"
#include "slay/commander.h"
#include "slay/diagnostic.h"
#include "slay/grammar.h"
#include "slay/n00b_parse.h"
#include "slay/n00b_tokenizer.h"
#include "n00b/n00b_compile.h"
#include "n00b/n00b_type_map.h"

// ============================================================================
// Version
// ============================================================================

static void
print_version(void)
{
    n00b_printf("n00b «#».«#».«#»",
                (int64_t)N00B_VERS_MAJOR,
                (int64_t)N00B_VERS_MINOR,
                (int64_t)N00B_VERS_PATCH);
}

// ============================================================================
// Usage text
// ============================================================================

static void
print_usage(void)
{
    n00b_eprintf(
        "Usage: n00b [command] [options] [files...]\n"
        "\n"
        "Commands:\n"
        "  compile   Compile n00b source files\n"
        "  run       Run n00b source files\n"
        "  repl      Interactive REPL (default)\n"
        "  help      Show this help message\n"
        "\n"
        "Options:\n"
        "  -h, --help       Show this help message\n"
        "  -V, --version    Show version information\n"
        "  -v, --verbose    Enable verbose output");
}

// ============================================================================
// Commander setup
// ============================================================================

static n00b_cmdr_t *
build_commander(void)
{
    n00b_cmdr_t *c = n00b_cmdr_new();

    n00b_cmdr_set_name(c, r"n00b");

    // Root-level flags.
    n00b_cmdr_add_flag(c, n00b_string_empty(), r"--help",
                        N00B_CMDR_TYPE_BOOL, false, r"Show help message");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--help", r"-h");

    n00b_cmdr_add_flag(c, n00b_string_empty(), r"--version",
                        N00B_CMDR_TYPE_BOOL, false, r"Show version");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--version", r"-V");

    n00b_cmdr_add_flag(c, n00b_string_empty(), r"--verbose",
                        N00B_CMDR_TYPE_BOOL, false, r"Verbose output");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--verbose", r"-v");

    // Root positional args (for bare `n00b foo.n00b` → repl with file).
    n00b_cmdr_add_positional(c, n00b_string_empty(), r"file",
                              N00B_CMDR_TYPE_WORD, 0, -1);

    // compile subcommand.
    n00b_cmdr_add_command(c, r"compile", r"Compile n00b source files");
    n00b_cmdr_add_positional(c, r"compile", r"file",
                              N00B_CMDR_TYPE_WORD, 1, -1);

    // run subcommand.
    n00b_cmdr_add_command(c, r"run", r"Run n00b source files");
    n00b_cmdr_add_positional(c, r"run", r"file",
                              N00B_CMDR_TYPE_WORD, 1, -1);

    // repl subcommand.
    n00b_cmdr_add_command(c, r"repl", r"Interactive REPL");
    n00b_cmdr_add_positional(c, r"repl", r"file",
                              N00B_CMDR_TYPE_WORD, 0, -1);

    // help subcommand.
    n00b_cmdr_add_command(c, r"help", r"Show detailed help");

    return c;
}

// ============================================================================
// Grammar loading (from n00b_dev.c)
// ============================================================================

static n00b_grammar_t *
load_n00b_grammar(void)
{
    FILE *f = NULL;

    // Try relative paths from typical working directories.
    n00b_string_t *try_paths[] = {
        r"grammars/n00b.bnf",
        r"../grammars/n00b.bnf",
        r"../../grammars/n00b.bnf",
        NULL,
    };

    for (n00b_string_t **p = try_paths; *p; p++) {
        f = fopen((*p)->data, "r");

        if (f) {
            break;
        }
    }

    if (!f) {
        // Try MESON_SOURCE_ROOT.
        const char *srcroot = getenv("MESON_SOURCE_ROOT");

        if (srcroot) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/grammars/n00b.bnf", srcroot);
            f = fopen(path, "r");
        }
    }

    if (!f) {
        n00b_eprintf("error: cannot find grammars/n00b.bnf");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

    n00b_string_t *bnf_text = n00b_string_from_cstr(buf);
    free(buf);

    n00b_grammar_t *g = n00b_grammar_new();
    n00b_grammar_set_error_recovery(g, false);

    bool ok = n00b_bnf_load(bnf_text, r"module", g);

    if (!ok) {
        n00b_eprintf("error: n00b_bnf_load failed");
        n00b_grammar_free(g);
        return NULL;
    }

    return g;
}

// ============================================================================
// Source file reading
// ============================================================================

static n00b_buffer_t *
read_source_file(n00b_string_t *path)
{
    FILE *f = fopen(path->data, "r");

    if (!f) {
        n00b_eprintf("error: cannot open '«#»'", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *raw = malloc((size_t)len + 1);
    fread(raw, 1, (size_t)len, f);
    raw[len] = '\0';
    fclose(f);

    n00b_buffer_t *buf = n00b_buffer_from_bytes(raw, (int64_t)len);
    free(raw);

    return buf;
}

// ============================================================================
// Run mode: parse → annotate → codegen → JIT → execute
// ============================================================================

static int
run_file(n00b_grammar_t *g, n00b_string_t *source_file, bool verbose)
{
    n00b_buffer_t *buf = read_source_file(source_file);

    if (!buf) {
        return 1;
    }

    // Tokenize.
    n00b_scanner_t      *scanner = n00b_scanner_new(buf, n00b_lang_tokenize, g);
    n00b_token_stream_t *ts      = n00b_token_stream_new(scanner);

    // Parse.
    n00b_parse_result_t *r = n00b_grammar_parse(g, ts, N00B_PARSE_MODE_DEFAULT);

    if (!n00b_parse_result_ok(r)) {
        n00b_string_t *err = n00b_parse_result_error_string(r);
        n00b_eprintf("Parse failed: «#»", err);

        n00b_string_t *expected = n00b_parse_result_expected_string(r);

        if (expected->u8_bytes > 0) {
            n00b_eprintf("Expected: «#»", expected);
        }

        n00b_parse_result_free(r);
        return 1;
    }

    n00b_parse_tree_t *tree = n00b_parse_result_tree(r);

    if (verbose) {
        n00b_printf("Parsed «#» OK", source_file);
    }

    // Annotation walk.
    n00b_annot_result_t *ar = n00b_compile_walk(g, tree);

    // Diagnostic accumulator.
    n00b_diag_ctx_t *diag_ctx = n00b_diag_ctx_new();

    if (ar && ar->tc_ctx) {
        n00b_diag_import_tc_errors(diag_ctx, ar->tc_ctx);
    }

    // Codegen + JIT + execute.
    n00b_cg_session_t *session = n00b_cg_session_new(
        g, .type_map = n00b_type_map);

    bool    run_ok     = false;
    int64_t run_result = n00b_cg_session_run_module(
        session, tree, .annot = ar, .ok = &run_ok);

    int exit_code;

    if (run_ok) {
        if (verbose) {
            n00b_printf("=> «#»", run_result);
        }

        exit_code = (int)run_result;
    }
    else {
        n00b_eprintf("error: codegen/JIT failed for '«#»'", source_file);
        exit_code = 1;
    }

    // Print diagnostics.
    n00b_diag_print_all(diag_ctx, buf->data, source_file->data);

    if (n00b_diag_has_errors(diag_ctx)) {
        exit_code = 1;
    }

    n00b_cg_session_free(session);
    n00b_diag_ctx_free(diag_ctx);

    if (ar && ar->symtab) {
        n00b_symtab_free(ar->symtab);
    }

    n00b_parse_result_free(r);

    return exit_code;
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    n00b_cmdr_t *cmdr = build_commander();

    // No arguments → default to repl mode (stub for now).
    if (argc <= 1) {
        n00b_printf("n00b repl (not yet implemented)");
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 0;
    }

    n00b_cmdr_result_t *result = n00b_cmdr_parse(cmdr, argc - 1,
                                                   (const char **)&argv[1]);

    if (!result || !result->ok) {
        int32_t nerr = n00b_cmdr_error_count(result);

        for (int32_t i = 0; i < nerr; i++) {
            n00b_string_t *err = n00b_cmdr_error_get(result, i);
            n00b_eprintf("n00b: «#»", err);
        }

        print_usage();
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 1;
    }

    // Handle --help.
    if (n00b_cmdr_flag_bool(result, r"--help")) {
        print_usage();
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 0;
    }

    // Handle --version.
    if (n00b_cmdr_flag_bool(result, r"--version")) {
        print_version();
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 0;
    }

    // Determine mode from subcommand, default to "repl".
    n00b_string_t *cmd  = n00b_cmdr_result_command(result);
    const char    *mode = "repl";

    if (cmd && cmd->u8_bytes > 0) {
        mode = cmd->data;
    }

    bool    verbose = n00b_cmdr_flag_bool(result, r"--verbose");
    int32_t nargs   = n00b_cmdr_arg_count(result);

    // help subcommand.
    if (strcmp(mode, "help") == 0) {
        print_usage();
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 0;
    }

    // compile subcommand (stub for now).
    if (strcmp(mode, "compile") == 0) {
        n00b_printf("compile: not yet implemented");
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 1;
    }

    // run subcommand or default repl-with-files: JIT all files, execute
    // the first one.
    if (strcmp(mode, "run") == 0 || (strcmp(mode, "repl") == 0 && nargs > 0)) {
        if (nargs == 0) {
            n00b_eprintf("error: 'run' requires at least one source file");
            n00b_cmdr_result_free(result);
            n00b_cmdr_free(cmdr);
            n00b_shutdown();
            return 1;
        }

        // Load grammar once for all files.
        n00b_grammar_t *g = load_n00b_grammar();

        if (!g) {
            n00b_cmdr_result_free(result);
            n00b_cmdr_free(cmdr);
            n00b_shutdown();
            return 1;
        }

        // Run the first file.  (Future: JIT all files into one session,
        // then execute the first file's _main.)
        n00b_string_t *first = n00b_cmdr_arg_str(result, 0);
        int exit_code = run_file(g, first, verbose);

        n00b_grammar_free(g);
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return exit_code;
    }

    // Bare repl mode (no files).
    n00b_printf("n00b repl (not yet implemented)");

    n00b_cmdr_result_free(result);
    n00b_cmdr_free(cmdr);
    n00b_shutdown();
    return 0;
}
