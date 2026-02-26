/**
 * @file compile_packrat.c
 * @brief Bootstrap compiler: compile_file() using the packrat parser.
 *
 * ## Pipeline Overview
 *
 * NCC processes a single C source file through these stages:
 *
 * | Stage | Function / Module | Description |
 * |-------|-------------------|-------------|
 * | 1. Read    | `ncc_buf_read_file` / `ncc_buf_read_stream` | Load source into `ncc_buf_t` |
 * | 2. CPP     | `ncc_invoke_preprocessor` (ncc.c) | Run underlying compiler with `-E` |
 * | 3. Lex     | `lex()` (lex.c) | Tokenize CPP output into `tok_t[]` |
 * | 4. Prefix  | `prefix_apply_transforms` (prefix_xform.c) | Literal modifier rewrites (e.g.
 * `r"..."`) | | 5. Parse   | `parse_translation_unit_st` (parse.c) | Packrat parse into
 * `tnode_t` tree | | 6. Flatten | `register_flatten_xform` (xform_flatten.c) | Collapse
 * recursive-descent chains (wildcard, post-order) | | 7. Xforms  | Semantic transforms (see
 * below) | Rewrite NCC extensions to standard C | | 8. Emit    | `final_output` (output.c) |
 * Pipe transformed C to underlying compiler |
 *
 * Stages 1-2 happen in `ncc.c`; stages 3-8 happen here in `compile_file()`.
 * With `-E -E`, the pipeline stops after stage 2 (raw CPP output).
 * With `--dump-tokens`, the pipeline stops after stage 4.
 *
 * ## Semantic Transform Ordering (Stage 7)
 *
 * All semantic transforms run in a single `xform_apply()` pass over the
 * flattened tree.  Within that pass, dispatch is by node type (`nt_id`),
 * and transforms registered on the same NT fire in registration order.
 * Registration order (and why it matters):
 *
 * | Order | Transform | NT targets | Rationale |
 * |-------|-----------|------------|-----------|
 * | 1 | `package` | external_declaration, function_definition, declaration, primary_expression,
 * typedef_name, struct/union/enum specifiers, enumeration_constant | **Must run first**:
 * rewrites identifiers that other transforms match on. | | 2 | `typeid` | synthetic_identifier
 * | Evaluates `typeid(...)` to SHA-based identifiers. No ordering deps. | | 3 | `typestr` |
 * synthetic_string_literal | Evaluates `typestr(T)` to string literals. No ordering deps. | | 4
 * | `once` | function_definition, declaration | Wraps `once` functions in `pthread_once`.
 * Independent of others. | | 5 | `vargs` | function_definition, declaration, postfix_expression
 * | Rewrites `+`-variadic signatures and call sites. Must run before `keyword` (which also
 * touches function_definition/declaration). | | 6 | `keyword` | function_definition,
 * declaration | Rewrites `_kargs` definitions. Runs after `vargs` so vargs parameters are
 * already rewritten. | | 7 | `kw_call` | postfix_expression | Rewrites keyword-argument call
 * sites (`f(.arg=val)`). Depends on `keyword` having processed definitions first. | | 8 |
 * `bang` | postfix_expression | Rewrites `expr!` error propagation. Depends on `kw_call` (the
 * `!` may appear on a kw-rewritten call). | | 9 | `rstr` | postfix_expression | Rewrites
 * `__ncc_rstr("...")` rich string calls to static compound literals. Pre-CPP scan rewrites
 * `r"..."` → `__ncc_rstr("...")`. | | 10 | `constexpr` | postfix_expression | Evaluates
 * `constexpr_eval()`, `constexpr_max()`, `constexpr_min()`. May appear in transform-generated
 * code. | | 11 | `constexpr_paste` | synthetic_identifier | Evaluates `constexpr_paste()`. May
 * appear in typeid-generated code. |
 *
 * Because all transforms are post-order and the tree walk is bottom-up,
 * child nodes are fully transformed before their parents.  For nodes
 * sharing the same NT (e.g., multiple transforms on `postfix_expression`),
 * registration order is the tiebreaker — earlier-registered transforms
 * fire first on each node.
 *
 * ## Separate Pipeline: --modernize
 *
 * The `--modernize` flag uses a completely different pipeline (modernize.c):
 * - **Phase A** (`mod_token_xforms.c`): Token-level C11/C17→C23 rewrites
 *   on the *original* (pre-CPP) source.
 * - **Phase B** (`mod_tree_xforms.c`): Tree-level rewrites using CPP output.
 * - **clang-format**: Optional post-formatting.
 *
 * The modernize pipeline shares the lexer and token types but does not
 * use the packrat parser or the semantic transforms listed above.
 */

#include <stdlib.h>
#include "base_alloc_shim.h"
#include "ncc_limits.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "compile.h"
#include "buf.h"
#include "parse.h"
#include "prefix_xform.h"
#include "rewrite.h"
#include "transform.h"
#include "st.h"

extern void final_output(compile_ctx_t *ctx);

void
ncc_register_extra_prefixes(prefix_registry_t *reg)
{
    (void)reg;
}

// ============================================================================
// Pre-CPP scan: r"..." → __ncc_rstr("...")
//
// Scans the raw source buffer *before* the C preprocessor runs and rewrites
// r"..." rich string literals into __ncc_rstr("...") calls so the
// preprocessor sees a plain function call.
//
// The scan is a simple byte-by-byte state machine that tracks comment and
// string context to avoid false matches.
// ============================================================================

static bool
is_id_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '_';
}

/**
 * @brief Rewrite r"..." to __ncc_rstr("...") in a source buffer.
 *
 * Returns a new buffer if any r"..." literals were found, or the
 * original buffer unchanged.
 */
static ncc_buf_t *
rstr_prescan(ncc_buf_t *input)
{
    const char *src = input->data;
    int64_t     len = input->len;

    // Quick check: does the buffer contain r" at all?
    bool has_rstr = false;
    for (int64_t i = 0; i < len - 1; i++) {
        if (src[i] == 'r' && src[i + 1] == '"') {
            if (i == 0 || !is_id_char(src[i - 1])) {
                has_rstr = true;
                break;
            }
        }
    }
    if (!has_rstr) {
        return input;
    }

    // Build output buffer with replacements.
    ncc_buf_t *out = ncc_buf_alloc(0);

    enum {
        ST_NORMAL,
        ST_LINE_COMMENT,
        ST_BLOCK_COMMENT,
        ST_STRING,
        ST_CHAR
    } state = ST_NORMAL;

    int64_t i          = 0;
    int64_t flush_from = 0; // start of pending un-flushed source

    while (i < len) {
        switch (state) {
        case ST_NORMAL:
            // Line comment?
            if (src[i] == '/' && i + 1 < len && src[i + 1] == '/') {
                state = ST_LINE_COMMENT;
                i += 2;
                continue;
            }
            // Block comment?
            if (src[i] == '/' && i + 1 < len && src[i + 1] == '*') {
                state = ST_BLOCK_COMMENT;
                i += 2;
                continue;
            }
            // String literal (not preceded by r)?
            if (src[i] == '"') {
                state = ST_STRING;
                i++;
                continue;
            }
            // Char literal?
            if (src[i] == '\'') {
                state = ST_CHAR;
                i++;
                continue;
            }
            // r"..." match?
            if (src[i] == 'r' && i + 1 < len && src[i + 1] == '"') {
                if (i == 0 || !is_id_char(src[i - 1])) {
                    // Flush everything before the 'r'
                    if (i > flush_from) {
                        out = ncc_buf_concat(out, (char *)src + flush_from, i - flush_from);
                    }
                    // Emit __ncc_rstr( instead of r
                    out = ncc_buf_concat_str(out, "__ncc_rstr(");
                    i++; // skip the 'r', keep the '"'

                    // Now scan forward to find the end of the string
                    // (including adjacent string concatenation)
                    // First: consume this string literal
                    int64_t str_start = i; // the '"'
                    i++;                   // skip opening "
                    while (i < len) {
                        if (src[i] == '\\' && i + 1 < len) {
                            i += 2; // skip escaped char
                        }
                        else if (src[i] == '"') {
                            i++; // skip closing "
                            break;
                        }
                        else {
                            i++;
                        }
                    }

                    // Check for adjacent string concatenation: whitespace then "
                    while (i < len) {
                        int64_t ws = i;
                        while (ws < len
                               && (src[ws] == ' ' || src[ws] == '\t' || src[ws] == '\n'
                                   || src[ws] == '\r')) {
                            ws++;
                        }
                        if (ws < len && src[ws] == '"') {
                            // Adjacent string: include whitespace + this string
                            i = ws + 1; // skip opening "
                            while (i < len) {
                                if (src[i] == '\\' && i + 1 < len) {
                                    i += 2;
                                }
                                else if (src[i] == '"') {
                                    i++;
                                    break;
                                }
                                else {
                                    i++;
                                }
                            }
                        }
                        else {
                            break;
                        }
                    }

                    // Emit the string content (from str_start to i)
                    out        = ncc_buf_concat(out, (char *)src + str_start, i - str_start);
                    // Emit closing )
                    out        = ncc_buf_concat_str(out, ")");
                    flush_from = i;
                    continue;
                }
            }
            i++;
            break;

        case ST_LINE_COMMENT:
            if (src[i] == '\n') {
                state = ST_NORMAL;
            }
            i++;
            break;

        case ST_BLOCK_COMMENT:
            if (src[i] == '*' && i + 1 < len && src[i + 1] == '/') {
                state = ST_NORMAL;
                i += 2;
            }
            else {
                i++;
            }
            break;

        case ST_STRING:
            if (src[i] == '\\' && i + 1 < len) {
                i += 2;
            }
            else if (src[i] == '"') {
                state = ST_NORMAL;
                i++;
            }
            else {
                i++;
            }
            break;

        case ST_CHAR:
            if (src[i] == '\\' && i + 1 < len) {
                i += 2;
            }
            else if (src[i] == '\'') {
                state = ST_NORMAL;
                i++;
            }
            else {
                i++;
            }
            break;
        }
    }

    // Flush remaining bytes
    if (flush_from < len) {
        out = ncc_buf_concat(out, (char *)src + flush_from, len - flush_from);
    }

    base_dealloc(input);
    return out;
}

// ============================================================================
// Token dump helpers (bootstrap only — main NCC uses dragon_slay's C lexer)
// ============================================================================

static const char *
token_type_name(ttype_t type)
{
    switch (type) {
    case TT_ERR:
        return "ERR";
    case TT_WS:
        return "WS";
    case TT_ID:
        return "ID";
    case TT_KEYWORD:
        return "KW";
    case TT_NUM:
        return "NUM";
    case TT_COMMENT:
        return "CMT";
    case TT_STR:
        return "STR";
    case TT_CHR:
        return "CHR";
    case TT_PUNCT:
        return "PUNCT";
    case TT_PREPROC:
        return "PP";
    case TT_UNKNOWN:
        return "UNK";
    default:
        return "???";
    }
}

// --dump-tokens: print token list to stdout
static void
dump_tokens(lex_t *lex)
{
    printf("# Tokens: %d\n", lex->num_toks);
    printf("# Format: index type line ncc_off text\n\n");

    for (int i = 0; i < lex->num_toks; i++) {
        tok_t *tok  = &lex->toks[i];
        int    tlen = tok->len > 60 ? 60 : tok->len;

        char *text;
        if (tok->replacement) {
            text = tok->replacement->data;
            tlen = tok->replacement->len > 60 ? 60 : tok->replacement->len;
        }
        else {
            text = lex->input->data + tok->offset;
        }

        if (tok->type == TT_WS) {
            printf("%5d %-5s %4d %d ",
                   i,
                   token_type_name(tok->type),
                   tok->line_no,
                   lex_is_ncc_off(lex, i));
            for (int j = 0; j < tlen; j++) {
                char c = text[j];
                if (c == '\n') {
                    printf("\\n");
                }
                else if (c == '\t') {
                    printf("\\t");
                }
                else if (c == ' ') {
                    printf("·");
                }
                else {
                    putchar(c);
                }
            }
            printf("\n");
        }
        else if (tok->type == TT_PREPROC || tok->type == TT_COMMENT) {
            int end = 0;
            while (end < tlen && text[end] != '\n') {
                end++;
            }
            printf("%5d %-5s %4d %d %.*s%s\n",
                   i,
                   token_type_name(tok->type),
                   tok->line_no,
                   lex_is_ncc_off(lex, i),
                   end,
                   text,
                   end < tok->len ? "..." : "");
        }
        else {
            printf("%5d %-5s %4d %d %.*s\n",
                   i,
                   token_type_name(tok->type),
                   tok->line_no,
                   lex_is_ncc_off(lex, i),
                   tlen,
                   text);
        }
    }

    fflush(stdout);
}

// ============================================================================
// Transform registration table
//
// Registration order matters — see the pipeline documentation at the top
// of this file for ordering rationale.
// ============================================================================

typedef void (*xform_register_fn)(xform_registry_t *);

// Forward declarations (one per xform_*.c file)
extern void register_flatten_xform(xform_registry_t *);
extern void register_package_xform(xform_registry_t *);
extern void register_typeid_xform(xform_registry_t *);
extern void register_typestr_xform(xform_registry_t *);
extern void register_once_xform(xform_registry_t *);
extern void register_vargs_xform(xform_registry_t *);
extern void register_keyword_xform(xform_registry_t *);
extern void register_kw_call_xform(xform_registry_t *);
extern void register_bang_xform(xform_registry_t *);
extern void register_rstr_xform(xform_registry_t *);
extern void register_constexpr_xform(xform_registry_t *);
extern void register_constexpr_paste_xform(xform_registry_t *);
extern void register_typehash_xform(xform_registry_t *);

// Semantic transforms in pipeline order (see file header for rationale).
static const xform_register_fn semantic_xforms[] = {
    register_package_xform,         // 1: namespace rewrites (must be first)
    register_typeid_xform,          // 2: typeid() → SHA identifiers
    register_typehash_xform,        // 3: typehash() → uint64 literals
    register_typestr_xform,         // 4: typestr() → string literals
    register_once_xform,            // 5: once functions → pthread_once
    register_vargs_xform,           // 6: variadic + → compound literals
    register_keyword_xform,         // 7: _kargs definitions (after vargs)
    register_kw_call_xform,         // 8: keyword call sites (after keyword)
    register_bang_xform,            // 9: expr! error propagation (after kw_call)
    register_rstr_xform,            // 10: r"..." rich string literals
    register_constexpr_xform,       // 11: constexpr_eval/max/min
    register_constexpr_paste_xform, // 12: constexpr_paste
};

#define NUM_SEMANTIC_XFORMS (sizeof(semantic_xforms) / sizeof(semantic_xforms[0]))

// ============================================================================
// Parse error helpers
// ============================================================================

// Find the source filename for a token position by scanning backward
// for the most recent #line directive. Returns in_file if none found.
static char *
find_source_file(lex_t *lex, int tok_pos)
{
    for (int i = tok_pos; i >= 0; i--) {
        tok_t *t = &lex->toks[i];
        if (t->type != TT_PREPROC) {
            continue;
        }
        char *text = lex->input->data + t->offset;
        char *p    = text + 1; // Skip '#'
        char *end  = text + t->len;

        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p + 4 <= end && strncmp(p, "line", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
            p += 4;
            while (p < end && (*p == ' ' || *p == '\t')) {
                p++;
            }
        }
        if (p >= end || *p < '0' || *p > '9') {
            continue;
        }
        while (p < end && *p >= '0' && *p <= '9') {
            p++;
        }
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p < end && *p == '"') {
            char *fname_start = ++p;
            while (p < end && *p != '"' && *p != '\n') {
                p++;
            }
            if (p > fname_start) {
                size_t len  = p - fname_start;
                char  *file = base_alloc(len + 1);
                if (!file) {
                    return lex->in_file;
                }
                memcpy(file, fname_start, len);
                file[len] = '\0';
                return file;
            }
        }
    }
    return lex->in_file;
}

// Show source context around a parse error token with caret marker and hints.
static void
show_error_context(lex_t *lex, int tok_pos)
{
    tok_t *tok = &lex->toks[tok_pos];

    int         toklen;
    const char *tok_text = tok_text_ptr(lex->input, tok, &toklen);

    if (tok->offset < 0) {
        int show_len = toklen > 200 ? 200 : toklen;
        fprintf(stderr, " %4d | %.*s\n", tok->line_no, show_len, tok_text);
        fprintf(stderr, "      | ^");
        int marker_len = show_len > 1 ? (show_len < 40 ? show_len - 1 : 39) : 0;
        for (int i = 0; i < marker_len; i++) {
            fputc('~', stderr);
        }
        fputc('\n', stderr);
    }
    else {
        char *data   = lex->input->data;
        int   len    = lex->input->len;
        int   offset = tok->offset;

        int line_start = offset;
        while (line_start > 0 && data[line_start - 1] != '\n') {
            line_start--;
        }

        int line_end = offset;
        while (line_end < len && data[line_end] != '\n') {
            line_end++;
        }

        int line_len = line_end - line_start;
        if (line_len > 200) {
            line_len = 200;
        }

        fprintf(stderr, " %4d | %.*s\n", tok->line_no, line_len, data + line_start);

        int col = offset - line_start;
        fprintf(stderr, "      | ");
        for (int i = 0; i < col; i++) {
            fputc(' ', stderr);
        }
        fputc('^', stderr);
        int marker_len = toklen > 1 ? (toklen < 40 ? toklen - 1 : 39) : 0;
        for (int i = 0; i < marker_len; i++) {
            fputc('~', stderr);
        }
        fputc('\n', stderr);
    }

    fprintf(stderr, "hint: There is probably an error in the following block.\n");
    fprintf(stderr, "hint: Use 'ncc -E file.c' to inspect NCC's preprocessed output.\n");
}

// ============================================================================
// Debug helpers (NCC_DEBUG_BUILD only)
// ============================================================================

typedef struct print_frame_t {
    tnode_t *node;
    int      depth;
    int      child_idx;
} print_frame_t;

static void
debug_print_tree_to(tnode_t *root, int depth, ncc_buf_t *buf, FILE *out)
{
    if (!root) {
        return;
    }

    int            stack_cap = NCC_CAP_XLARGE;
    int            stack_top = 0;
    print_frame_t *stack     = base_alloc(stack_cap * sizeof(print_frame_t));
    if (!stack) {
        return;
    }

    stack[stack_top++] = (print_frame_t){.node = root, .depth = depth, .child_idx = -1};

    while (stack_top > 0) {
        print_frame_t *f = &stack[stack_top - 1];

        if (f->child_idx == -1) {
            tnode_t *node = f->node;
            for (int i = 0; i < f->depth; i++) {
                fprintf(out, "  ");
            }
            fprintf(out, "%s", node->nt);
            if (node->tptr) {
                if (node->tptr->replacement) {
                    fprintf(out, " [%s]", node->tptr->replacement->data);
                }
                else if (buf) {
                    int   len  = node->tptr->len;
                    char *text = base_alloc(len + 1);
                    if (text) {
                        memcpy(text, buf->data + node->tptr->offset, len);
                        text[len] = '\0';
                        fprintf(out, " [%s]", text);
                        base_dealloc(text);
                    }
                }
            }
            fprintf(out, "\n");
            f->child_idx = 0;
        }

        if (f->child_idx >= f->node->num_kids) {
            stack_top--;
            continue;
        }

        int      ci    = f->child_idx++;
        tnode_t *child = tnode_get_kid(f->node, ci);
        if (!child) {
            continue;
        }

        if (stack_top >= stack_cap) {
            stack_cap *= 2;
            stack = base_realloc(stack, stack_cap * sizeof(print_frame_t));
            if (!stack) {
                return;
            }
            f = &stack[stack_top - 1];
        }

        stack[stack_top++]
            = (print_frame_t){.node = child, .depth = f->depth + 1, .child_idx = -1};
    }

    base_dealloc(stack);
}

void
debug_print_tree(tnode_t *node, int depth, ncc_buf_t *buf)
{
    debug_print_tree_to(node, depth, buf, stderr);
}

static void
dump_tree_if_requested(const char *env_var, tnode_t *tree, ncc_buf_t *buf)
{
    char *dump_tree = getenv(env_var);
    if (!dump_tree || !*dump_tree) {
        return;
    }

    FILE *dump_out;
    bool  close_it = false;

    if (strcmp(dump_tree, "1") == 0 || strcmp(dump_tree, "stderr") == 0) {
        dump_out = stderr;
    }
    else {
        dump_out = fopen(dump_tree, "w");
        if (!dump_out) {
            fprintf(stderr,
                    "[NCC] Error: Cannot open %s for tree dump: %s\n",
                    dump_tree,
                    strerror(errno));
            exit(1);
        }
        close_it = true;
    }

    debug_print_tree_to(tree, 0, buf, dump_out);
    fflush(dump_out);

    if (close_it) {
        fclose(dump_out);
        fprintf(stderr, "[NCC] Parse tree written to: %s\n", dump_tree);
    }
}

static int
get_ncc_debug_level(void)
{
    char *val = getenv("NCC_DEBUG");
    if (!val) {
        return -1;
    }
    char *end;
    long  level = strtol(val, &end, 10);
    if (end == val || *end != '\0') {
        return -1;
    }
    return (int)level;
}

// ============================================================================
// Bootstrap compile_file(): complete pipeline from source to compiler output
// ============================================================================

void
compile_file(ncc_argv_t *argv_ctx)
{
    compile_ctx_t ctx = {
        .argv     = argv_ctx,
        .compiler = ncc_find_compiler(),
    };

    // Step 1: Read input
    ncc_buf_t *input;

    if (argv_ctx->has_stdin) {
        input = ncc_buf_read_stream(stdin);
    }
    else {
        input = ncc_buf_read_file_by_name(argv_ctx->sources[0]);

        if (!input) {
            ncc_warning("%s: Cannot open file resorting to passthrough mode.\n",
                        argv_ctx->argv[0]);
            compiler_passthrough(argv_ctx);
        }
    }

    // Step 1.5: Rewrite r"..." → __ncc_rstr("...") before CPP
    input = rstr_prescan(input);

    // Step 2: Preprocess
    input = ncc_invoke_preprocessor(argv_ctx, ctx.compiler, input);

    if (!input) {
        ncc_error("%s: No output received from C preprocessor.\n", argv_ctx->argv[0]);
        abort();
    }

    // -E -E: stop after C preprocessor, before lex/parse/transform
    if (argv_ctx->e_count >= 2) {
        fwrite(input->data, 1, input->len, stdout);
        return;
    }

    // Step 3: Tokenize with NCC's lexer
    lex_init(&ctx.lex_state, input, argv_ctx->has_stdin ? "«stdin»" : argv_ctx->sources[0]);
    lex(&ctx.lex_state);

    // Step 4: Apply prefix (literal modifier) transformations
    prefix_registry_t prefix_reg;
    prefix_registry_init(&prefix_reg);
    prefix_register_builtins(&prefix_reg);

    if (!prefix_apply_transforms(&prefix_reg, &ctx.lex_state)) {
        ncc_error("%s: Prefix (literal modifier) transformation failed in '%s'\n",
                  argv_ctx->argv[0],
                  argv_ctx->has_stdin ? "«stdin»" : argv_ctx->sources[0]);
        exit(1);
    }
    prefix_registry_free(&prefix_reg);

    // --dump-tokens: print token list after lexing + prefix transforms, then exit
    if (argv_ctx->has_dump_tokens) {
        dump_tokens(&ctx.lex_state);
        return;
    }

    // Step 5: Check if file is effectively empty
    bool has_code = false;
    for (int i = 0; i < ctx.lex_state.num_toks; i++) {
        ttype_t type = ctx.lex_state.toks[i].type;
        if (type != TT_WS && type != TT_COMMENT && type != TT_PREPROC) {
            has_code = true;
            break;
        }
    }

    if (!has_code) {
        ctx.tree = nullptr;
        final_output(&ctx);
        return;
    }

    // Step 6: Initialize symbol table
    symtab_t st;
    st_init(&st);

    bool verbose = false;
    {
        char *v = getenv("NCC_VERBOSE");
        if (v && *v && strcmp(v, "0") != 0) {
            verbose = true;
        }
    }

    // Step 7: Parse with packrat parser
    int pos  = 0;
    ctx.tree = parse_translation_unit_st(&ctx.lex_state, &pos, &st);

    if (!ctx.tree) {
        tok_t      *err_tok  = &ctx.lex_state.toks[pos];
        char       *err_file = find_source_file(&ctx.lex_state, pos);
        int         tok_len;
        const char *tok_text = tok_text_ptr(ctx.lex_state.input, err_tok, &tok_len);
        int         show_len = tok_len > 30 ? 30 : tok_len;
        ncc_error("%s:%d: Parse error at token '%.*s'\n",
                  err_file,
                  err_tok->line_no,
                  show_len,
                  tok_text);
        show_error_context(&ctx.lex_state, pos);
        exit(1);
    }

    // Check if parser consumed all tokens
    if (pos < ctx.lex_state.num_toks) {
        int final_pos = pos;
        while (final_pos < ctx.lex_state.num_toks) {
            ttype_t type = ctx.lex_state.toks[final_pos].type;
            if (type != TT_WS && type != TT_COMMENT && type != TT_PREPROC) {
                break;
            }
            final_pos++;
        }
        if (final_pos < ctx.lex_state.num_toks) {
            tok_t      *stop_tok  = &ctx.lex_state.toks[final_pos];
            char       *stop_file = find_source_file(&ctx.lex_state, final_pos);
            int         tok_len;
            const char *tok_text = tok_text_ptr(ctx.lex_state.input, stop_tok, &tok_len);
            int         show_len = tok_len > 50 ? 50 : tok_len;
            ncc_error(
                "%s:%d: Parser stopped at token '%.*s' - "
                "cannot process this code.\n",
                stop_file,
                stop_tok->line_no,
                show_len,
                tok_text);
            show_error_context(&ctx.lex_state, final_pos);
            exit(1);
        }
    }

    int debug_level = get_ncc_debug_level();
    if (debug_level >= 0) {
        fprintf(stderr, "[NCC_DEBUG] Parse complete\n");
        fflush(stderr);
    }
    if (debug_level == 0) {
        fprintf(stderr, "=== NCC_DEBUG: Parse tree PRE-transformations ===\n");
        fflush(stderr);
        debug_print_tree(ctx.tree, 0, ctx.lex_state.input);
        fprintf(stderr, "=== End of parse tree ===\n");
        fflush(stderr);
    }

    dump_tree_if_requested("NCC_DUMP_TREE_PRE", ctx.tree, ctx.lex_state.input);

#ifdef NCC_PARSER_STATS
    {
        int    ntoks     = ctx.lex_state.num_toks;
        // Two-tier memo: fail bitmap (1 bit per pos*NT) + success dict.
        double bitmap_mb = (double)(ntoks + 1) * NT_COUNT / 8.0 / (1024.0 * 1024.0);
        fprintf(stderr,
                "[ncc-stats] Tokens: %d, fail bitmap: %.2f MB, success dict: dynamic\n",
                ntoks,
                bitmap_mb);
        extern void parser_dump_stats(void);
        parser_dump_stats();
    }
#endif
    if (verbose) {
        fprintf(stderr, "[ncc] Parse complete\n");
    }

    // Step 8: Flatten transform (must run before all other transforms)
    xform_registry_t flatten_reg;
    xform_registry_init(&flatten_reg);
    register_flatten_xform(&flatten_reg);

    tree_xform_t flatten_ctx;
    xform_ctx_init(&flatten_ctx, &ctx.lex_state, &st, ctx.tree);
#ifdef NCC_DEBUG_BUILD
    if (debug_level >= 0) {
        fprintf(stderr, "[NCC_DEBUG] Starting flatten pass\n");
        fflush(stderr);
    }
#endif
    ctx.tree = xform_apply(&flatten_reg, &flatten_ctx);
    xform_registry_free(&flatten_reg);

    // Step 9: Semantic transforms (operate on flattened tree)
    xform_registry_t xform_reg;
    xform_registry_init(&xform_reg);
    for (size_t i = 0; i < NUM_SEMANTIC_XFORMS; i++) {
        semantic_xforms[i](&xform_reg);
    }

    tree_xform_t xform_ctx;
    xform_ctx_init(&xform_ctx, &ctx.lex_state, &st, ctx.tree);
    xform_ctx.user_data = (void *)&ctx;
#ifdef NCC_DEBUG_BUILD
    if (debug_level >= 0) {
        fprintf(stderr, "[NCC_DEBUG] Starting semantic transformations\n");
        fflush(stderr);
    }
#endif
    ctx.tree = xform_apply(&xform_reg, &xform_ctx);

    ctx.has_transforms = (xform_ctx.nodes_replaced > 0);

    if (verbose) {
        fprintf(stderr, "[ncc] Transforms: %d nodes replaced\n", xform_ctx.nodes_replaced);
    }

    xform_registry_free(&xform_reg);

#ifdef NCC_DEBUG_BUILD
    if (debug_level >= 0) {
        fprintf(stderr,
                "[NCC_DEBUG] Transformations complete (%d nodes replaced)\n",
                xform_ctx.nodes_replaced);
        fflush(stderr);
    }

    if (debug_level == 1) {
        fprintf(stderr, "=== NCC_DEBUG: Parse tree POST-transformations ===\n");
        fflush(stderr);
        debug_print_tree(ctx.tree, 0, ctx.lex_state.input);
        fprintf(stderr, "=== End of parse tree ===\n");
        fflush(stderr);
    }

    dump_tree_if_requested("NCC_DUMP_TREE_POST", ctx.tree, ctx.lex_state.input);

    if (debug_level >= 0) {
        fprintf(stderr, "[NCC_DEBUG] Starting final_output\n");
        fflush(stderr);
    }
#endif

    // Step 10: Emit to compiler
    if (verbose) {
        fprintf(stderr, "[ncc] Emitting to compiler: %s\n", ctx.compiler);
    }
    final_output(&ctx);

    st_free(&st);
    synth_cleanup();
    parse_arena_destroy();
}
