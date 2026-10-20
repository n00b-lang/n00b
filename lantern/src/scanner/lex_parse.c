#include "private/scanner/lex_decl.h"

#include "private/parse/annot_grammar.h"
#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "scanner/scanner.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

#include <stdio.h>
#include <string.h>

static _Thread_local core_string_t lex_parse_err;

static void
lex_parse_err_set(core_string_t message)
{
    core_free(lex_parse_err.data);
    lex_parse_err = message;
}

__attribute__((format(printf, 1, 2))) static void
lex_parse_err_printf(const char *fmt, ...)
{
    core_buffer_t *buf = core_buffer_empty();
    va_list        ap;

    va_start(ap, fmt);
    core_buffer_vprintf(buf, fmt, ap);
    va_end(ap);
    lex_parse_err_set(core_buffer_take_string(buf));
}

core_string_t
scan_lex_parse_error(void)
{
    return lex_parse_err;
}

core_string_t
scan_lex_kind_name(scan_lex_kind_t kind)
{
    switch (kind) {
#define SCAN_LEX_NAME(name, spelling) \
    case SCAN_LEX_##name: return CORE_STRING_STATIC(spelling);
        SCAN_LEX_DIRECTIVES(SCAN_LEX_NAME)
#undef SCAN_LEX_NAME
    default: return CORE_STRING_STATIC("lex-unknown");
    }
}

static bool
lex_kind(core_string_t name, scan_lex_kind_t *out)
{
#define SCAN_LEX_MATCH(sym, spelling) \
    if (core_string_eq(name, CORE_STRING_STATIC(spelling))) { \
        *out = SCAN_LEX_##sym; \
        return true; \
    }
    SCAN_LEX_DIRECTIVES(SCAN_LEX_MATCH)
#undef SCAN_LEX_MATCH
    return false;
}

static core_string_t
copy_string(core_string_t value)
{
    return value.data ? core_string_from_raw(value.data, value.u8_bytes)
                      : (core_string_t){0};
}

void
scan_lex_decls_free(core_list_t(scan_lex_decl_t) *decls)
{
    if (!decls) {
        return;
    }
    for (size_t i = 0; i < decls->len; i++) {
        scan_lex_decl_t *decl = &decls->data[i];
        core_free(decl->source_origin.data);
        for (size_t j = 0; j < decl->lex_opts.len; j++) {
            scan_lex_opt_t *opt = &decl->lex_opts.data[j];
            core_free(opt->name.data);
            for (size_t k = 0; k < opt->args.len; k++) {
                core_free(opt->args.data[k].data);
            }
            core_list_free(opt->args);
        }
        core_list_free(decl->lex_opts);
    }
    core_list_free(*decls);
}

typedef struct {
    scan_lex_opt_t *option;
    core_string_t first_identifier;
} leaf_ctx_t;

static bool
collect_leaf(parse_tree_t *leaf, void *arg)
{
    leaf_ctx_t *ctx = arg;
    int64_t tid = parse_tree_leaf_tid(leaf);
    core_string_t value = parse_tree_leaf_text(leaf);

    if (tid == PARSE_TOK_IDENTIFIER && !ctx->first_identifier.data) {
        ctx->first_identifier = value;
    }
    if (ctx->option) {
        if (tid == PARSE_TOK_IDENTIFIER && !ctx->option->name.data) {
            ctx->option->name = copy_string(value);
        }
        else if (tid == PARSE_TOK_STRING_LIT) {
            core_list_push(ctx->option->args, copy_string(value));
        }
    }
    return false;
}

static void
collect_options(parse_grammar_t *grammar, parse_tree_t *node,
                scan_lex_decl_t *decl)
{
    if (parse_tree_is_nt(grammar, node, CORE_STRING_STATIC("lex-option"))) {
        scan_lex_opt_t opt = {.args = core_list_new(core_string_t)};
        leaf_ctx_t ctx = {.option = &opt};
        parse_tree_scan_leaves(node, false, collect_leaf, &ctx);
        core_list_push(decl->lex_opts, opt);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        collect_options(grammar, core_tree_child(node, i), decl);
    }
}

typedef struct {
    parse_grammar_t *grammar;
    core_string_t origin;
    core_list_t(scan_lex_decl_t) *decls;
    bool failed;
} decl_ctx_t;

static void
collect_declarations(decl_ctx_t *ctx, parse_tree_t *node)
{
    if (ctx->failed) {
        return;
    }
    if (parse_tree_is_nt(ctx->grammar, node,
                         CORE_STRING_STATIC("lex-declaration"))) {
        leaf_ctx_t leaves = {0};
        parse_tree_scan_leaves(node, false, collect_leaf, &leaves);
        scan_lex_decl_t decl = {
            .source_origin = copy_string(ctx->origin),
            .lex_opts = core_list_new(scan_lex_opt_t),
        };
        if (!lex_kind(leaves.first_identifier, &decl.kind)) {
            lex_parse_err_printf(
                     "lexer '%.*s': unknown directive '%.*s'",
                     (int)ctx->origin.u8_bytes, ctx->origin.data,
                     (int)leaves.first_identifier.u8_bytes,
                     leaves.first_identifier.data);
            core_list_t(scan_lex_decl_t) temporary
                = core_list_new(scan_lex_decl_t);
            core_list_push(temporary, decl);
            scan_lex_decls_free(&temporary);
            ctx->failed = true;
            return;
        }
        collect_options(ctx->grammar, node, &decl);
        core_list_push(*ctx->decls, decl);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        collect_declarations(ctx, core_tree_child(node, i));
    }
}

bool
scan_lex_parse_file(core_string_t text, core_string_t origin,
                    core_list_t(scan_lex_decl_t) *decls)
{
    lex_parse_err_set((core_string_t){0});
    if (!decls || !text.data) {
        return false;
    }
    parse_grammar_t *grammar = parse_annot_grammar();
    parse_nonterm_t *start
        = grammar ? parse_find_nonterm(grammar, CORE_STRING_STATIC("lex-file"))
                  : nullptr;
    if (!start) {
        lex_parse_err_printf(
                 "lexer grammar is unavailable");
        return false;
    }

    core_buffer_t *buffer = nullptr;
    parse_scanner_t *scanner = nullptr;
    parse_token_stream_t *tokens = parse_annot_lex(text, &buffer, &scanner);
    if (!tokens) {
        lex_parse_err_printf(
                 "cannot tokenize lexer '%.*s'",
                 (int)origin.u8_bytes, origin.data);
        return false;
    }

    parse_pwz_parser_t *parser
        = parse_pwz_new_for_start(grammar, parse_nonterm_id(start));
    bool ok = parser && parse_pwz_parse(parser, tokens);
    if (ok) {
        decl_ctx_t ctx = {
            .grammar = grammar,
            .origin = origin,
            .decls = decls,
        };
        collect_declarations(&ctx, parse_pwz_get_tree(parser));
        ok = !ctx.failed;
    }
    else {
        lex_parse_err_printf(
                 "lexer '%.*s': syntax error near token %d",
                 (int)origin.u8_bytes, origin.data,
                 parser ? parse_pwz_error_pos(parser) : 0);
    }
    if (parser) {
        parse_pwz_free(parser);
    }
    parse_annot_lex_free(tokens, buffer, scanner);
    return ok;
}
