// n00b.c — Main n00b CLI entry point.
//
// Provides compile, run, and repl subcommands via the commander module.
// Default mode (no subcommand) is repl.
//
// The "run" and default paths load the n00b grammar, parse each source
// file, run the annotation walk, codegen via MIR, JIT-compile, and
// execute the first file's _main.
//
// Debug/inspection flags (--debug-tokens, --debug-grammar, etc.) add
// diagnostic output on top of normal execution.

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "core/string.h"
#include "conduit/print.h"
#include "parsers/scanner.h"
#include "parsers/token_stream.h"
#include "slay/annot_walk.h"
#include "slay/codegen.h"
#include "n00b/n00b_compile_binary.h"
#include "n00b/embed.h"
#include "n00b/embed_ffi.h"
#include "n00b/n00b_compile.h"
#include "n00b/n00b_type_map.h"
#include "slay/bnf.h"
#include "slay/cf_label.h"
#include "slay/cdg.h"
#include "slay/cfg.h"
#include "slay/commander.h"
#include "slay/dfg.h"
#include "slay/debug.h"
#include "slay/grammar.h"
#include "slay/earley.h"
#include "slay/n00b_parse.h"
#include "n00b/n00b_tokenizer.h"
#include "slay/parse_tree.h"
#include "slay/symtab.h"
#include "slay/token.h"
#include "slay/diagnostic.h"
#include "slay/analyze.h"
#include "internal/slay/earley_internal.h"
#include "typecheck/types.h"
#include "typecheck/print.h"

// From n00b_repl.c.
extern int n00b_repl_run(n00b_grammar_t *grammar);

// ============================================================================
// Debug options
// ============================================================================

typedef struct {
    bool              show_tokens;
    bool              show_grammar;
    bool              show_states;
    bool              show_lr0;
    bool              show_cfg;
    bool              show_cdg;
    bool              show_dfg;
    bool              show_labels;
    bool              show_symtab;
    bool              run_analyze;
    bool              run_typecheck;
    bool              quiet;
    n00b_parse_mode_t parse_mode;
} debug_opts_t;

// ============================================================================
// Compile helpers
// ============================================================================

static const char *
compile_compiler(void)
{
    const char *env = getenv("N00B_COMPILER");

    if (env && *env) {
        return env;
    }

    return "clang";
}

static unsigned long
compile_process_id(void)
{
#ifdef _WIN32
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

static bool
compile_temp_path(char *buf, size_t buf_len, const char *stem, int index, const char *suffix)
{
    const char *dir = getenv("TMPDIR");

#ifdef _WIN32
    if (!dir || !*dir) {
        dir = getenv("TEMP");
    }
    if (!dir || !*dir) {
        dir = getenv("TMP");
    }
#endif
    if (!dir || !*dir) {
#ifdef _WIN32
        dir = ".";
#else
        dir = "/tmp";
#endif
    }

    size_t      dir_len = strlen(dir);
    const char *sep     = "";

    if (dir_len > 0) {
        char last = dir[dir_len - 1];

        if (last != '/' && last != '\\') {
            sep = "/";
        }
    }

    int n = snprintf(buf,
                     buf_len,
                     "%s%sn00b_%s_%lu_%d%s",
                     dir,
                     sep,
                     stem,
                     compile_process_id(),
                     index,
                     suffix);

    return n > 0 && (size_t)n < buf_len;
}

static int
compile_spawn_wait(const char **argv)
{
    const char *program = argv[0];

#ifdef _WIN32
    int argc = 0;

    while (argv[argc]) {
        argc++;
    }

    const char **spawn_argv = calloc((size_t)argc + 1, sizeof(char *));

    if (!spawn_argv) {
        fprintf(stderr, "error: cannot allocate compiler argument list\n");
        return 1;
    }

    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        bool        q   = false;

        if (!arg || !*arg) {
            q = true;
        }
        else {
            for (const char *p = arg; *p; p++) {
                if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'
                    || *p == '"') {
                    q = true;
                    break;
                }
            }
        }

        if (!q) {
            size_t len = strlen(arg);
            char  *dup = malloc(len + 1);

            if (!dup) {
                fprintf(stderr, "error: cannot allocate compiler argument\n");
                for (int j = 0; j < i; j++) {
                    free((void *)spawn_argv[j]);
                }
                free(spawn_argv);
                return 1;
            }

            memcpy(dup, arg, len + 1);
            spawn_argv[i] = dup;
            continue;
        }

        size_t len = arg ? strlen(arg) : 0;
        char  *out = malloc(len * 2 + 3);

        if (!out) {
            fprintf(stderr, "error: cannot allocate compiler argument\n");
            for (int j = 0; j < i; j++) {
                free((void *)spawn_argv[j]);
            }
            free(spawn_argv);
            return 1;
        }

        char  *dst        = out;
        size_t backslashes = 0;

        *dst++ = '"';

        for (size_t j = 0; j < len; j++) {
            char ch = arg[j];

            if (ch == '\\') {
                backslashes++;
                continue;
            }

            if (ch == '"') {
                while (backslashes--) {
                    *dst++ = '\\';
                    *dst++ = '\\';
                }
                *dst++ = '\\';
                *dst++ = '"';
                backslashes = 0;
                continue;
            }

            while (backslashes--) {
                *dst++ = '\\';
            }
            backslashes = 0;
            *dst++      = ch;
        }

        while (backslashes--) {
            *dst++ = '\\';
            *dst++ = '\\';
        }

        *dst++ = '"';
        *dst   = '\0';

        spawn_argv[i] = out;
    }

    spawn_argv[argc] = NULL;

    intptr_t rc = _spawnvp(_P_WAIT, program, spawn_argv);

    for (int i = 0; i < argc; i++) {
        free((void *)spawn_argv[i]);
    }
    free(spawn_argv);

    if (rc == -1) {
        fprintf(stderr, "error: spawn(%s) failed: %s\n", program, strerror(errno));
        return 127;
    }

    return (int)rc;
#else
    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "error: fork() failed: %s\n", strerror(errno));
        return 1;
    }

    if (pid == 0) {
        execvp(program, (char *const *)argv);
        fprintf(stderr, "error: execvp(%s) failed: %s\n", program, strerror(errno));
        _exit(127);
    }

    int status;

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "error: waitpid() failed: %s\n", strerror(errno));
        return 1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    return 1;
#endif
}

// ============================================================================
// Token dumping (ported from n00b_dev.c)
// ============================================================================

static void
dump_tokens(n00b_token_stream_t *ts)
{
    int index = 0;

    while (true) {
        n00b_token_info_t *tok = n00b_stream_next(ts);

        if (!tok || tok->tid == N00B_TOK_EOF) {
            break;
        }

        const char *text = "";
        size_t      tlen = 0;

        if (n00b_option_is_set(tok->value)) {
            n00b_string_t *val = n00b_option_get(tok->value);
            text               = val->data;
            tlen               = val->u8_bytes;
        }

        printf("%4d  tid=%-20lld L%u:C%u  \"%.*s\"\n",
               index++,
               (long long)tok->tid,
               tok->line,
               tok->column,
               (int)tlen,
               text);
    }
}

// ============================================================================
// Symbol table dumping (ported from n00b_dev.c)
// ============================================================================

static const char *
sym_kind_str(n00b_sym_kind_t kind)
{
    switch (kind) {
    case N00B_SYM_VARIABLE:
        return "var";
    case N00B_SYM_FUNCTION:
        return "func";
    case N00B_SYM_TYPEDEF:
        return "type";
    case N00B_SYM_TAG:
        return "tag";
    case N00B_SYM_ENUM_CONST:
        return "enum";
    case N00B_SYM_LABEL:
        return "label";
    case N00B_SYM_PARAM:
        return "param";
    default:
        return "?";
    }
}

static void
dump_symtab_recursive(n00b_parse_tree_t *t, int depth)
{
    if (!t || n00b_tree_is_leaf(t)) {
        return;
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(t);

    if (pn->scope) {
        n00b_scope_t *scope  = pn->scope;
        int           indent = scope->depth;

        for (int i = 0; i < indent; i++) {
            printf("  ");
        }

        if (scope->name && scope->name->data && scope->name->u8_bytes > 0) {
            printf("scope \"%.*s\" (depth %d):\n",
                   (int)scope->name->u8_bytes,
                   scope->name->data,
                   scope->depth);
        }
        else {
            printf("scope (depth %d):\n", scope->depth);
        }

        n00b_sym_entry_t *entry = scope->first_in_scope;

        while (entry) {
            for (int i = 0; i < indent + 1; i++) {
                printf("  ");
            }

            uint32_t line = 0;

            if (entry->decl_node) {
                n00b_parse_tree_t *first = n00b_pt_first_token(entry->decl_node);

                if (first) {
                    n00b_token_info_t *tok = n00b_parse_node_token(first);

                    if (tok) {
                        line = tok->line;
                    }
                }
            }

            if (entry->type_var) {
                n00b_string_t *ts = n00b_tc_type_to_string(entry->type_var);
                printf("%-6s %.*s : %.*s  line %u",
                       sym_kind_str(entry->kind),
                       (int)entry->name->u8_bytes,
                       entry->name->data,
                       (int)ts->u8_bytes,
                       ts->data,
                       line);
            }
            else {
                printf("%-6s %.*s  line %u",
                       sym_kind_str(entry->kind),
                       (int)entry->name->u8_bytes,
                       entry->name->data,
                       line);
            }

            if (entry->exposed_scope) {
                n00b_scope_t *es = entry->exposed_scope;

                if (es->name && es->name->data && es->name->u8_bytes > 0) {
                    printf("  [exposes \"%.*s\"]", (int)es->name->u8_bytes, es->name->data);
                }
                else {
                    printf("  [exposes scope]");
                }
            }

            printf("\n");

            entry = entry->next_in_scope;
        }
    }

    size_t nc = n00b_tree_num_children(t);

    for (size_t i = 0; i < nc; i++) {
        dump_symtab_recursive(n00b_tree_child(t, i), depth + 1);
    }
}

static void
dump_symtab(n00b_parse_tree_t *tree)
{
    dump_symtab_recursive(tree, 0);
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
// Grammar loading
// ============================================================================

static n00b_grammar_t *
load_n00b_grammar(n00b_string_t *grammar_file)
{
    FILE *f = NULL;

    if (grammar_file && grammar_file->u8_bytes > 0) {
        f = fopen(grammar_file->data, "r");

        if (!f) {
            n00b_eprintf("error: cannot open grammar file '«#»'", grammar_file);
            return NULL;
        }
    }
    else {
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
        "  -h, --help              Show this help message\n"
        "  -V, --version           Show version information\n"
        "  -v, --verbose           Enable verbose output\n"
        "  -q, --quiet             Suppress non-essential output\n"
        "\n"
        "Debug flags:\n"
        "  --debug-tokens          Dump token stream\n"
        "  --debug-grammar         Print grammar\n"
        "  --debug-states          Dump Earley parse states\n"
        "  --debug-lr0             Print LR(0) table\n"
        "  --debug-cfg             Show control flow graph\n"
        "  --debug-cdg             Show control dependence graph\n"
        "  --debug-dfg             Show data flow graph\n"
        "  --debug-labels          Show CF labels\n"
        "  -s, --debug-symtab      Dump symbol table\n"
        "  -a, --debug-analyze     Run analysis pass\n"
        "  -t, --debug-typecheck   Run type checking\n"
        "  --debug-all             Enable all debug outputs\n"
        "  --debug-earley          Force Earley parser\n"
        "  --debug-pwz             Force PWZ parser\n"
        "  --debug-grammar-file    Custom grammar file path");
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
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--help",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Show help message");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--help", r"-h");

    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--version",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Show version");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--version", r"-V");

    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--verbose",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Verbose output");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--verbose", r"-v");

    // Debug flags.
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-tokens",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Dump token stream");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-grammar",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Print grammar");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-states",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Dump Earley parse states");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-lr0",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Print LR(0) table");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-cfg",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Show control flow graph");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-cdg",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Show control dependence graph");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-dfg",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Show data flow graph");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-labels",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Show CF labels");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-symtab",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Dump symbol table");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--debug-symtab", r"-s");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-analyze",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Run analysis pass");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--debug-analyze", r"-a");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-typecheck",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Run type checking");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--debug-typecheck", r"-t");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-all",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Enable all debug outputs");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-earley",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Force Earley parser");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-pwz",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Force PWZ parser");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--debug-grammar-file",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Custom grammar file path");
    n00b_cmdr_add_flag(c,
                       n00b_string_empty(),
                       r"--quiet",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Suppress non-essential output");
    n00b_cmdr_add_flag_alias(c, n00b_string_empty(), r"--quiet", r"-q");

    // Root positional args (for bare `n00b foo.n` → repl with file).
    n00b_cmdr_add_positional(c, n00b_string_empty(), r"file", N00B_CMDR_TYPE_WORD, 0, -1);

    // compile subcommand.
    n00b_cmdr_add_command(c, r"compile", r"Compile n00b source files");
    n00b_cmdr_add_positional(c, r"compile", r"file", N00B_CMDR_TYPE_WORD, 1, -1);
    n00b_cmdr_add_flag(c,
                       r"compile",
                       r"--output",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"Output binary path");
    n00b_cmdr_add_flag_alias(c, r"compile", r"--output", r"-o");
    n00b_cmdr_add_flag(c,
                       r"compile",
                       r"--lib-dir",
                       N00B_CMDR_TYPE_WORD,
                       true,
                       r"libn00b.a directory");
    n00b_cmdr_add_flag(c,
                       r"compile",
                       r"--keep-objects",
                       N00B_CMDR_TYPE_BOOL,
                       false,
                       r"Keep .o files");

    // run subcommand.
    n00b_cmdr_add_command(c, r"run", r"Run n00b source files");
    n00b_cmdr_add_positional(c, r"run", r"file", N00B_CMDR_TYPE_WORD, 1, -1);

    // repl subcommand.
    n00b_cmdr_add_command(c, r"repl", r"Interactive REPL");
    n00b_cmdr_add_positional(c, r"repl", r"file", N00B_CMDR_TYPE_WORD, 0, -1);

    // help subcommand.
    n00b_cmdr_add_command(c, r"help", r"Show detailed help");

    return c;
}

// Defined in n00b_repl.c (shared between n00b and n00b_dev executables).
extern bool n00b_load_builtins(n00b_grammar_t *g, n00b_cg_session_t *session);

// ============================================================================
// Run mode: parse → annotate → codegen → JIT → execute
// ============================================================================

static int
run_file(n00b_grammar_t *g, n00b_string_t *source_file, bool verbose, debug_opts_t *dbg)
{
    n00b_buffer_t *buf = read_source_file(source_file);

    if (!buf) {
        return 1;
    }

    // Pre-parse debug output.
    if (dbg->show_grammar) {
        printf("=== Grammar ===\n");
        n00b_grammar_print(g, stdout);
        printf("\n");
    }

    if (dbg->show_lr0) {
        printf("=== LR(0) Table ===\n");
        n00b_lr0_print(g, stdout);
        printf("\n");
    }

    // Tokenize.
    n00b_scanner_t      *scanner = n00b_scanner_new(buf, n00b_lang_tokenize, g);
    n00b_token_stream_t *ts      = n00b_token_stream_new(scanner);

    if (dbg->show_tokens) {
        printf("=== Tokens ===\n");
        dump_tokens(ts);
        printf("\n");
        n00b_stream_reset(ts);
    }

    // Parse.
    n00b_parse_tree_t   *tree = NULL;
    n00b_parse_result_t *r    = NULL;

    if (dbg->show_states) {
        // Direct Earley parse so we can dump states.
        printf("=== Earley Parse (with state dump) ===\n");

        n00b_earley_parser_t *earley = n00b_earley_new(g);
        bool                  ok     = n00b_earley_parse(earley, ts);

        printf("\n=== Earley States ===\n");
        n00b_parser_print_states(earley, stdout, true);
        printf("\n");

        if (ok) {
            int32_t count = n00b_earley_parse_count(earley);

            if (!dbg->quiet) {
                printf("Parse OK — %d completion(s)\n\n", count);
            }

            n00b_parse_forest_t forest     = n00b_earley_get_forest(earley);
            int32_t             tree_count = n00b_parse_forest_count(&forest);

            if (tree_count > 0) {
                tree = n00b_parse_forest_best(&forest);

                if (!dbg->quiet) {
                    printf("=== Parse Tree ===\n");
                    n00b_tree_print(tree, g, stdout);
                    printf("\n");
                }
            }

            n00b_parse_forest_free(&forest);
        }
        else {
            printf("Parse FAILED.\n");

            n00b_earley_diagnostics_t diag = {0};
            n00b_earley_extract_diagnostics(earley, &diag);
            fprintf(stderr,
                    "Error at line %u, col %u",
                    diag.error_loc.line,
                    diag.error_loc.column);

            if (diag.error_loc.got && diag.error_loc.got->data) {
                fprintf(stderr,
                        ": got '%.*s'",
                        (int)diag.error_loc.got->u8_bytes,
                        diag.error_loc.got->data);
            }

            fprintf(stderr, "\n");

            if (diag.expected_count > 0) {
                fprintf(stderr, "Expected:");

                for (int32_t i = 0; i < diag.expected_count; i++) {
                    n00b_string_t *tname = n00b_get_terminal_name(g, diag.expected_ids[i]);

                    if (tname && tname->data) {
                        fprintf(stderr, " %.*s", (int)tname->u8_bytes, tname->data);
                    }
                    else {
                        fprintf(stderr, " tid=%lld", (long long)diag.expected_ids[i]);
                    }
                }

                fprintf(stderr, "\n");
            }

            if (diag.expected_ids)
                n00b_free(diag.expected_ids);
            if (diag.expected_desc)
                n00b_free(diag.expected_desc);
            if (diag.active_ctx)
                n00b_free(diag.active_ctx);

            n00b_earley_free(earley);
            return 1;
        }

        n00b_earley_free(earley);
    }
    else {
        n00b_parse_mode_t mode      = dbg->parse_mode;
        const char       *mode_name = (mode == N00B_PARSE_MODE_EARLEY_ONLY) ? "earley"
                                    : (mode == N00B_PARSE_MODE_PWZ_ONLY)    ? "pwz"
                                                                            : "default";

        if (!dbg->quiet) {
            printf("=== Parse (mode: %s) ===\n", mode_name);
        }

        r = n00b_grammar_parse(g, ts, mode);

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

        tree               = n00b_parse_result_tree(r);
        int32_t tree_count = n00b_parse_result_tree_count(r);

        if (!dbg->quiet) {
            printf("Parse OK — %d tree(s)%s\n\n",
                   tree_count,
                   n00b_parse_result_ambiguous(r) ? " [AMBIGUOUS]" : "");
            printf("=== Parse Tree ===\n");
            n00b_tree_print(tree, g, stdout);
            printf("\n");
        }
    }

    if (!tree) {
        if (r) {
            n00b_parse_result_free(r);
        }

        return 1;
    }

    if (verbose && !dbg->quiet) {
        n00b_printf("Parsed «#» OK", source_file);
    }

    // Annotation walk.
    n00b_annot_result_t *ar = n00b_compile_walk(g, tree);

    // Diagnostic accumulator.
    n00b_diag_ctx_t *diag_ctx = n00b_diag_ctx_new();

    // Import type-check errors (always, for the final diagnostic print).
    if (ar && ar->tc_ctx) {
        n00b_diag_import_tc_errors(diag_ctx, ar->tc_ctx);
    }

    // Symbol table.
    if (dbg->show_symtab) {
        printf("\n=== Symbol Table ===\n");
        dump_symtab(tree);
        printf("\n");
    }

    // CF labels.
    if (dbg->show_labels && ar && ar->cf_labels) {
        printf("\n=== CF Labels ===\n");
        n00b_cf_labels_print(ar->cf_labels, g, stdout);
        printf("\n");
    }

    // CFG / CDG / DFG + Analysis.
    bool need_graphs = dbg->show_cfg || dbg->show_cdg || dbg->show_dfg || dbg->run_analyze;

    if (need_graphs && ar && ar->cf_labels) {
        n00b_cfg_t *cfg = n00b_build_cfg(ar->cf_labels, tree, r"module", ar->symtab);

        if (cfg) {
            if (dbg->show_cfg) {
                printf("=== CFG ===\n");
                n00b_cfg_print(cfg, g, stdout);
                printf("\n");
            }

            n00b_cdg_t *cdg = NULL;

            if (dbg->show_cdg) {
                cdg = n00b_build_cdg(cfg);

                if (cdg) {
                    printf("=== CDG ===\n");
                    n00b_cdg_print(cdg, g, stdout);
                    printf("\n");
                }
                else {
                    printf("(CDG build returned NULL)\n");
                }
            }

            n00b_dfg_t *dfg = NULL;

            if (dbg->show_dfg || dbg->run_analyze) {
                dfg = n00b_build_dfg(cfg, ar->cf_labels, g, ar);

                if (dfg && dbg->show_dfg) {
                    printf("=== DFG ===\n");
                    n00b_dfg_print(dfg, g, stdout);
                    printf("\n");
                }
                else if (!dfg) {
                    printf("(DFG build returned NULL)\n");
                }
            }

            // Run analysis.
            if (dbg->run_analyze && dfg) {
                n00b_analyze_ctx_t actx = {
                    .cfg       = cfg,
                    .cdg       = cdg,
                    .dfg       = dfg,
                    .symtab    = ar->symtab,
                    .cf_labels = ar->cf_labels,
                    .annot     = ar,
                    .grammar   = g,
                    .diag      = diag_ctx,
                    .func_name = r"module",
                };

                n00b_analyze_all(&actx);
            }

            if (dfg) {
                n00b_dfg_free(dfg);
            }

            if (cdg) {
                n00b_cdg_free(cdg);
            }

            n00b_cfg_free(cfg);
        }
        else if (!dbg->quiet) {
            printf("(CFG build returned NULL)\n");
        }
    }

    // Codegen + JIT + execute.
    n00b_dict_untyped_t *embed_reg = n00b_embed_registry_new();
    n00b_ffi_embed_register(embed_reg);

    n00b_cg_session_t *session
        = n00b_cg_session_new(g, .type_map = n00b_type_map, .embed_registry = embed_reg);

    // Load builtins (print, etc.) before the user module.
    n00b_load_builtins(g, session);

    bool    run_ok     = false;
    int64_t run_result = n00b_cg_session_run_module(session, tree, .annot = ar, .ok = &run_ok);

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

    if (r) {
        n00b_parse_result_free(r);
    }

    return exit_code;
}

// ============================================================================
// Compile mode: parse → codegen → .o → link
// ============================================================================

static int
compile_files(n00b_grammar_t *g, n00b_cmdr_result_t *result, bool verbose)
{
#ifdef _WIN32
    (void)g;
    (void)result;
    (void)verbose;
    fprintf(stderr, "error: compile mode is not supported on Windows yet\n");
    return 1;
#endif

    int            nargs        = n00b_cmdr_arg_count(result);
    n00b_string_t *output       = n00b_cmdr_flag_str(result, r"--output");
    n00b_string_t *lib_dir_s    = n00b_cmdr_flag_str(result, r"--lib-dir");
    bool           keep_objects = n00b_cmdr_flag_bool(result, r"--keep-objects");

    const char *lib_dir = (lib_dir_s && lib_dir_s->u8_bytes > 0) ? lib_dir_s->data : NULL;

    // Default output name: first source file with extension stripped.
    char output_path[1024];

    if (output && output->u8_bytes > 0) {
        snprintf(output_path, sizeof(output_path), "%.*s", (int)output->u8_bytes, output->data);
    }
    else {
        n00b_string_t *first = n00b_cmdr_arg_str(result, 0);
        snprintf(output_path, sizeof(output_path), "%.*s", (int)first->u8_bytes, first->data);

        // Strip .n extension.
        char *dot = strrchr(output_path, '.');

        if (dot) {
            *dot = '\0';
        }

#ifdef _WIN32
        strncat(output_path, ".exe", sizeof(output_path) - strlen(output_path) - 1);
#endif
    }

    // Allocate arrays for object file paths.
    // +1 for the startup shim.
    const char **obj_paths   = calloc((size_t)(nargs + 1), sizeof(const char *));
    char       **obj_paths_m = calloc((size_t)(nargs + 1), sizeof(char *)); // owned copies
    int          n_objs      = 0;

    // Create a codegen session with FFI embed support.
    n00b_dict_untyped_t *compile_embed_reg = n00b_embed_registry_new();
    n00b_ffi_embed_register(compile_embed_reg);

    n00b_cg_session_t *session = n00b_cg_session_new(g,
                                                     .type_map       = n00b_type_map,
                                                     .embed_registry = compile_embed_reg);

    // Load builtins (print, etc.) before user modules.
    n00b_load_builtins(g, session);

    // Compile each source file.
    for (int i = 0; i < nargs; i++) {
        n00b_string_t *source_file = n00b_cmdr_arg_str(result, i);
        n00b_buffer_t *buf         = read_source_file(source_file);

        if (!buf) {
            n00b_eprintf("error: cannot read '«#»'", source_file);
            goto fail;
        }

        // Tokenize.
        n00b_scanner_t      *scanner = n00b_scanner_new(buf, n00b_lang_tokenize, g);
        n00b_token_stream_t *ts      = n00b_token_stream_new(scanner);

        // Parse.
        n00b_parse_result_t *r = n00b_grammar_parse(g, ts, N00B_PARSE_MODE_DEFAULT);

        if (!n00b_parse_result_ok(r)) {
            n00b_string_t *err = n00b_parse_result_error_string(r);
            n00b_eprintf("Parse failed for '«#»': «#»", source_file, err);
            n00b_parse_result_free(r);
            goto fail;
        }

        n00b_parse_tree_t *tree = n00b_parse_result_tree(r);

        // Annotation walk.
        n00b_annot_result_t *ar = n00b_compile_walk(g, tree);

        // Compile to machine code.
        n00b_module_code_t *mod = n00b_cg_session_compile_module(session, tree, .annot = ar);

        if (!mod) {
            n00b_eprintf("error: codegen failed for '«#»'", source_file);
            n00b_parse_result_free(r);
            goto fail;
        }

        // Emit .o file.
        n00b_buffer_t *obj_buf = n00b_emit_object_file(mod);

        if (!obj_buf) {
            n00b_eprintf("error: object file emission failed for '«#»'", source_file);
            n00b_parse_result_free(r);
            goto fail;
        }

        // Write .o to a temp file.
        char obj_path[1024];

        if (!compile_temp_path(obj_path, sizeof(obj_path), "module", i, ".o")) {
            n00b_eprintf("error: cannot create temporary object path");
            n00b_parse_result_free(r);
            goto fail;
        }

        FILE *fp = fopen(obj_path, "wb");

        if (!fp) {
            n00b_eprintf("error: cannot write '«#»'", n00b_string_from_cstr(obj_path));
            n00b_parse_result_free(r);
            goto fail;
        }

        fwrite(obj_buf->data, 1, obj_buf->byte_len, fp);
        fclose(fp);

        obj_paths_m[n_objs] = strdup(obj_path);
        obj_paths[n_objs]   = obj_paths_m[n_objs];
        n_objs++;

        n00b_parse_result_free(r);

        if (verbose) {
            printf("  compiled %.*s -> %s\n",
                   (int)source_file->u8_bytes,
                   source_file->data,
                   obj_path);
        }
    }

    // Generate startup shim.
    {
        const char *startup_src
            = "#include <stdint.h>\n"
              "extern void n00b_init_simple(int, char**);\n"
              "extern void n00b_shutdown(void);\n"
              "extern int64_t _main(void);\n"
              "int main(int argc, char **argv) {\n"
              "    n00b_init_simple(argc, argv);\n"
              "    int64_t r = _main();\n"
              "    n00b_shutdown();\n"
              "    return (int)r;\n"
              "}\n";

        // Write startup source to temp file.
        char startup_c_buf[1024];
        char startup_o_buf[1024];

        if (!compile_temp_path(startup_c_buf, sizeof(startup_c_buf), "startup", 0, ".c")
            || !compile_temp_path(startup_o_buf, sizeof(startup_o_buf), "startup", 0, ".o")) {
            n00b_eprintf("error: cannot create temporary startup path");
            goto fail;
        }

        const char *startup_c = startup_c_buf;
        FILE       *fp        = fopen(startup_c, "w");

        if (!fp) {
            n00b_eprintf("error: cannot write startup shim");
            goto fail;
        }

        fputs(startup_src, fp);
        fclose(fp);

        // Compile startup shim via clang.
        const char *startup_o = startup_o_buf;
        const char *compiler  = compile_compiler();

        const char *argv[8];
        int         ai = 0;

        argv[ai++] = compiler;
#ifdef _WIN32
        argv[ai++] = "--target=x86_64-w64-windows-gnu";
#endif
        argv[ai++] = "-c";
        argv[ai++] = "-o";
        argv[ai++] = startup_o;
        argv[ai++] = startup_c;
        argv[ai]   = NULL;

        if (compile_spawn_wait(argv) != 0) {
            n00b_eprintf("error: startup shim compilation failed");
            goto fail;
        }

        obj_paths_m[n_objs] = strdup(startup_o);
        obj_paths[n_objs]   = obj_paths_m[n_objs];
        n_objs++;

        // Clean up temp .c file.
        remove(startup_c);
    }

    // Link everything.
    {
        int rc = n00b_link_binary(obj_paths, n_objs, output_path, lib_dir);

        if (rc != 0) {
            fprintf(stderr, "error: linking failed (exit code %d)\n", rc);
            goto fail;
        }

        if (verbose) {
            printf("  linked -> %s\n", output_path);
        }
    }

    // Clean up temp .o files.
    if (!keep_objects) {
        for (int i = 0; i < n_objs; i++) {
            remove(obj_paths[i]);
        }
    }
    else {
        printf("Object files:\n");

        for (int i = 0; i < n_objs; i++) {
            printf("  %s\n", obj_paths[i]);
        }
    }

    for (int i = 0; i < n_objs; i++) {
        free(obj_paths_m[i]);
    }

    free(obj_paths);
    free(obj_paths_m);
    n00b_cg_session_free(session);
    return 0;

fail:
    for (int i = 0; i < n_objs; i++) {
        if (!keep_objects) {
            remove(obj_paths[i]);
        }

        free(obj_paths_m[i]);
    }

    free(obj_paths);
    free(obj_paths_m);
    n00b_cg_session_free(session);
    return 1;
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

    // No arguments → default to repl mode.
    if (argc <= 1) {
        n00b_grammar_t *g = load_n00b_grammar(NULL);

        if (!g) {
            n00b_cmdr_free(cmdr);
            n00b_shutdown();
            return 1;
        }

        int rc = n00b_repl_run(g);
        n00b_grammar_free(g);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return rc;
    }

    n00b_cmdr_result_t *result = n00b_cmdr_parse(cmdr, argc - 1, (const char **)&argv[1]);

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

    // Extract debug flags.
    debug_opts_t dbg = {0};

    dbg.show_tokens   = n00b_cmdr_flag_bool(result, r"--debug-tokens");
    dbg.show_grammar  = n00b_cmdr_flag_bool(result, r"--debug-grammar");
    dbg.show_states   = n00b_cmdr_flag_bool(result, r"--debug-states");
    dbg.show_lr0      = n00b_cmdr_flag_bool(result, r"--debug-lr0");
    dbg.show_cfg      = n00b_cmdr_flag_bool(result, r"--debug-cfg");
    dbg.show_cdg      = n00b_cmdr_flag_bool(result, r"--debug-cdg");
    dbg.show_dfg      = n00b_cmdr_flag_bool(result, r"--debug-dfg");
    dbg.show_labels   = n00b_cmdr_flag_bool(result, r"--debug-labels");
    dbg.show_symtab   = n00b_cmdr_flag_bool(result, r"--debug-symtab");
    dbg.run_analyze   = n00b_cmdr_flag_bool(result, r"--debug-analyze");
    dbg.run_typecheck = n00b_cmdr_flag_bool(result, r"--debug-typecheck");
    dbg.quiet         = n00b_cmdr_flag_bool(result, r"--quiet");
    dbg.parse_mode    = N00B_PARSE_MODE_DEFAULT;

    if (n00b_cmdr_flag_bool(result, r"--debug-earley")) {
        dbg.parse_mode = N00B_PARSE_MODE_EARLEY_ONLY;
    }
    else if (n00b_cmdr_flag_bool(result, r"--debug-pwz")) {
        dbg.parse_mode = N00B_PARSE_MODE_PWZ_ONLY;
    }

    // --debug-all enables all debug outputs.
    if (n00b_cmdr_flag_bool(result, r"--debug-all")) {
        dbg.show_tokens   = true;
        dbg.show_grammar  = true;
        dbg.show_states   = true;
        dbg.show_lr0      = true;
        dbg.show_cfg      = true;
        dbg.show_cdg      = true;
        dbg.show_dfg      = true;
        dbg.show_labels   = true;
        dbg.show_symtab   = true;
        dbg.run_analyze   = true;
        dbg.run_typecheck = true;
    }

    n00b_string_t *grammar_file = n00b_cmdr_flag_str(result, r"--debug-grammar-file");

    // help subcommand.
    if (strcmp(mode, "help") == 0) {
        print_usage();
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 0;
    }

    // compile subcommand.
    if (strcmp(mode, "compile") == 0) {
#ifdef _WIN32
        fprintf(stderr, "error: compile mode is not supported on Windows yet\n");
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return 1;
#endif

        n00b_grammar_t *g = load_n00b_grammar(grammar_file);

        if (!g) {
            n00b_cmdr_result_free(result);
            n00b_cmdr_free(cmdr);
            n00b_shutdown();
            return 1;
        }

        int rc = compile_files(g, result, verbose);
        n00b_grammar_free(g);
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return rc;
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
        n00b_grammar_t *g = load_n00b_grammar(grammar_file);

        if (!g) {
            n00b_cmdr_result_free(result);
            n00b_cmdr_free(cmdr);
            n00b_shutdown();
            return 1;
        }

        // Run the first file.  (Future: JIT all files into one session,
        // then execute the first file's _main.)
        n00b_string_t *first     = n00b_cmdr_arg_str(result, 0);
        int            exit_code = run_file(g, first, verbose, &dbg);

        n00b_grammar_free(g);
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return exit_code;
    }

    // Bare repl mode (no files).
    {
        n00b_grammar_t *g = load_n00b_grammar(grammar_file);

        if (!g) {
            n00b_cmdr_result_free(result);
            n00b_cmdr_free(cmdr);
            n00b_shutdown();
            return 1;
        }

        int rc = n00b_repl_run(g);
        n00b_grammar_free(g);
        n00b_cmdr_result_free(result);
        n00b_cmdr_free(cmdr);
        n00b_shutdown();
        return rc;
    }
}
