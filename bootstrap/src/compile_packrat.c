/**
 * @file compile_packrat.c
 * @brief Bootstrap compiler: compile_file() using the packrat parser.
 *
 * Provides the complete bootstrap compilation pipeline:
 *   1. Read input + preprocess
 *   2. Tokenize with NCC's lex.c
 *   3. Apply prefix transforms (builtins only)
 *   4. Parse with packrat parser
 *   5. Flatten + semantic transforms
 *   6. Emit via final_output()
 */

#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "compile.h"
#include "buf.h"
#include "parse.h"
#include "prefix_xform.h"
#include "transform.h"
#include "st.h"

extern void final_output(compile_ctx_t *ctx);

void
ncc_register_extra_prefixes(prefix_registry_t *reg)
{
    (void)reg;
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
            printf("%5d %-5s %4d %d ", i, token_type_name(tok->type), tok->line_no, lex_is_ncc_off(lex, i));
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
// Parse error helpers
// ============================================================================

extern void register_typeid_xform(xform_registry_t *reg);
extern void register_typestr_xform(xform_registry_t *reg);
extern void register_once_xform(xform_registry_t *reg);
extern void register_keyword_xform(xform_registry_t *reg);
extern void register_kw_call_xform(xform_registry_t *reg);
extern void register_bang_xform(xform_registry_t *reg);
extern void register_flatten_xform(xform_registry_t *reg);
extern void register_vargs_xform(xform_registry_t *reg);
extern void register_package_xform(xform_registry_t *reg);
extern void register_constexpr_xform(xform_registry_t *reg);
extern void register_constexpr_paste_xform(xform_registry_t *reg);

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

    fprintf(stderr,
            "hint: There is probably an error in the following block.\n");
    fprintf(stderr,
            "hint: Use 'ncc -E file.c' to inspect NCC's preprocessed output.\n");
}

// ============================================================================
// Debug helpers (NCC_DEBUG_BUILD only)
// ============================================================================

#ifdef NCC_DEBUG_BUILD
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

    int            stack_cap = 128;
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

        stack[stack_top++] = (print_frame_t){.node = child, .depth = f->depth + 1, .child_idx = -1};
    }

    base_dealloc(stack);
}

static void
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
#endif // NCC_DEBUG_BUILD

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

    // Step 2: Preprocess
    input = ncc_invoke_preprocessor(argv_ctx, ctx.compiler, input);

    if (!input) {
        ncc_error("%s: No output received from C preprocessor.\n",
                  argv_ctx->argv[0]);
        abort();
    }

    // -E -E: stop after C preprocessor, before lex/parse/transform
    if (argv_ctx->e_count >= 2) {
        fwrite(input->data, 1, input->len, stdout);
        return;
    }

    // Step 3: Tokenize with NCC's lexer
    lex_init(&ctx.lex_state,
             input,
             argv_ctx->has_stdin ? "«stdin»" : argv_ctx->sources[0]);
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
        const char *tok_text = tok_text_ptr(ctx.lex_state.input,
                                            err_tok,
                                            &tok_len);
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
            const char *tok_text = tok_text_ptr(ctx.lex_state.input,
                                                stop_tok,
                                                &tok_len);
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

#ifdef NCC_DEBUG_BUILD
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
#endif

    if (verbose) {
        fprintf(stderr, "[ncc] Parse complete\n");
    }

    // Step 8: Flatten transform (must run before all other transforms)
    xform_registry_t flatten_reg;
    xform_registry_init(&flatten_reg);
    register_flatten_xform(&flatten_reg);

    xform_ctx_t flatten_ctx;
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
    register_package_xform(&xform_reg);
    register_typeid_xform(&xform_reg);
    register_typestr_xform(&xform_reg);
    register_once_xform(&xform_reg);
    register_vargs_xform(&xform_reg);
    register_keyword_xform(&xform_reg);
    register_kw_call_xform(&xform_reg);
    register_bang_xform(&xform_reg);
    register_constexpr_xform(&xform_reg);
    register_constexpr_paste_xform(&xform_reg);

    xform_ctx_t xform_ctx;
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
        fprintf(stderr, "[NCC_DEBUG] Transformations complete (%d nodes replaced)\n", xform_ctx.nodes_replaced);
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

    parse_arena_destroy();
}
