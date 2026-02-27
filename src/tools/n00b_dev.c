// n00b_dev.c — Development tool for parsing n00b source files.
//
// Loads the n00b grammar, tokenizes an input file, parses it, runs
// the annotation walk, and prints the annotated parse tree.
//
// Usage: n00b_dev <source-file> [--earley | --pwz | --tokens | --grammar]

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "parsers/scanner.h"
#include "parsers/token_stream.h"
#include "slay/annot_walk.h"
#include "slay/bnf.h"
#include "slay/cf_label.h"
#include "slay/debug.h"
#include "slay/grammar.h"
#include "slay/n00b_parse.h"
#include "slay/n00b_tokenizer.h"
#include "slay/parse_tree.h"
#include "slay/symtab.h"
#include "slay/token.h"

// ============================================================================
// Grammar loading
// ============================================================================

static n00b_grammar_t *
load_n00b_grammar(void)
{
    const char *paths[] = {
        "grammars/n00b.bnf",
        "../grammars/n00b.bnf",
        "../../grammars/n00b.bnf",
        NULL,
    };

    const char *srcroot = getenv("MESON_SOURCE_ROOT");
    FILE       *f       = NULL;

    for (const char **p = paths; *p; p++) {
        f = fopen(*p, "r");

        if (f) {
            break;
        }
    }

    if (!f && srcroot) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/grammars/n00b.bnf", srcroot);
        f = fopen(path, "r");
    }

    if (!f) {
        fprintf(stderr, "error: cannot find grammars/n00b.bnf\n");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

    n00b_string_t bnf_text = n00b_string_from_cstr(buf);
    free(buf);

    n00b_grammar_t *g = n00b_grammar_new();
    n00b_grammar_set_error_recovery(g, false);

    bool ok = n00b_bnf_load(bnf_text, *r"module", g);

    if (!ok) {
        fprintf(stderr, "error: n00b_bnf_load failed for n00b.bnf\n");
        n00b_grammar_free(g);
        return NULL;
    }

    return g;
}

// ============================================================================
// Source file reading
// ============================================================================

static char *
read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "r");

    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

    *out_len = (size_t)len;
    return buf;
}

// ============================================================================
// Token dumping
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
            n00b_string_t val = n00b_option_get(tok->value);
            text = val.data;
            tlen = val.u8_bytes;
        }

        printf("%4d  tid=%-20lld L%u:C%u  \"%.*s\"\n",
               index++, (long long)tok->tid, tok->line, tok->column,
               (int)tlen, text);
    }
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    bool                show_tokens  = false;
    bool                show_grammar = false;
    n00b_parse_mode_t   mode         = N00B_PARSE_MODE_DEFAULT;
    const char         *source_file  = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--earley") == 0) {
            mode = N00B_PARSE_MODE_EARLEY_ONLY;
        }
        else if (strcmp(argv[i], "--pwz") == 0) {
            mode = N00B_PARSE_MODE_PWZ_ONLY;
        }
        else if (strcmp(argv[i], "--tokens") == 0) {
            show_tokens = true;
        }
        else if (strcmp(argv[i], "--grammar") == 0) {
            show_grammar = true;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        }
        else {
            source_file = argv[i];
        }
    }

    if (!source_file) {
        fprintf(stderr, "usage: n00b_dev <source-file> "
                        "[--earley|--pwz] [--tokens] [--grammar]\n");
        return 1;
    }

    // Load grammar.
    n00b_grammar_t *g = load_n00b_grammar();

    if (!g) {
        return 1;
    }

    if (show_grammar) {
        printf("=== Grammar ===\n");
        n00b_grammar_print(g, stdout);
        printf("\n");
    }

    // Read source.
    size_t src_len;
    char  *src = read_file(source_file, &src_len);

    if (!src) {
        return 1;
    }

    // Tokenize.
    n00b_buffer_t       *buf     = n00b_buffer_from_bytes(src, (int64_t)src_len);
    n00b_scanner_t      *scanner = n00b_scanner_new(buf, n00b_lang_tokenize, g);
    n00b_token_stream_t *ts      = n00b_token_stream_new(scanner);

    if (show_tokens) {
        printf("=== Tokens ===\n");
        dump_tokens(ts);
        printf("\n");
        n00b_stream_reset(ts);
    }

    // Parse.
    const char *mode_name = (mode == N00B_PARSE_MODE_EARLEY_ONLY) ? "earley"
                          : (mode == N00B_PARSE_MODE_PWZ_ONLY)    ? "pwz"
                                                                  : "default";

    printf("=== Parse (mode: %s) ===\n", mode_name);

    n00b_parse_result_t *r = n00b_grammar_parse(g, ts, mode);

    if (!n00b_parse_result_ok(r)) {
        n00b_string_t err = n00b_parse_result_error_string(r);
        fprintf(stderr, "Parse failed: %.*s\n",
                (int)err.u8_bytes, err.data);

        n00b_string_t expected = n00b_parse_result_expected_string(r);

        if (expected.u8_bytes > 0) {
            fprintf(stderr, "Expected: %.*s\n",
                    (int)expected.u8_bytes, expected.data);
        }

        n00b_parse_result_free(r);
        free(src);
        return 1;
    }

    int32_t tree_count = n00b_parse_result_tree_count(r);
    printf("Parse OK — %d tree(s)%s\n\n",
           tree_count,
           n00b_parse_result_ambiguous(r) ? " [AMBIGUOUS]" : "");

    // Print parse tree.
    n00b_parse_tree_t *tree = n00b_parse_result_tree(r);

    printf("=== Parse Tree ===\n");
    n00b_tree_print(tree, g, stdout);
    printf("\n");

    // Annotation walk.
    printf("=== Annotation Walk ===\n");
    n00b_annot_result_t *ar = n00b_annot_walk_tree_full(g, tree);

    if (ar && ar->symtab) {
        printf("Symtab populated.\n");
        n00b_symtab_free(ar->symtab);
    }
    else {
        printf("(no annotations fired)\n");
    }

    // Cleanup.
    n00b_parse_result_free(r);
    free(src);

    n00b_shutdown();
    return 0;
}
