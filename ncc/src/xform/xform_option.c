// xform_option.c — Transform: first-class _option(T) type.
//
// For pointer types, _option(T*) = T* (NULL is none).
// For value types, _option(T) = struct { bool has_value; T value; }
// with a unique name via typeid-style mangling.
//
// Also transforms: _some(T, val), _none(T), _is_some(x), _is_none(x),
// _unwrap(x).
//
// Two registrations:
//   - "synthetic_identifier" (post-order) for _option(T)
//   - "postfix_expression"  (post-order) for _some/_none/_is_some/_is_none/_unwrap

#include "xform/xform_helpers.h"
#include "core/dict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Access to shared user_data dictionaries
// ============================================================================

// Layout-compatible with ncc_xform_data_t in ncc.c.
// Fields before our dicts: compiler, constexpr_headers, func_meta (opaque).
// We only access option_meta and option_decls by their actual struct offsets.
// ncc_xform_data_t layout:
//   const char              *compiler;
//   const char              *constexpr_headers;
//   ncc_meta_table_t         func_meta;          // opaque to us
//   n00b_dict_t      option_meta;
//   n00b_dict_t      option_decls;
//
// We replicate the func_meta layout to skip over it correctly.

#define _NCC_META_TABLE_SIZE 256

typedef struct {
    char *key;
    void *value;
} _ncc_meta_entry_t;

typedef struct {
    _ncc_meta_entry_t entries[_NCC_META_TABLE_SIZE];
} _ncc_meta_table_t;

typedef struct {
    const char          *compiler;
    const char          *constexpr_headers;
    _ncc_meta_table_t    func_meta;
    n00b_dict_t  option_meta;
    n00b_dict_t  option_decls;
} ncc_opt_xform_data_t;

static n00b_dict_t *
get_option_meta(n00b_xform_ctx_t *ctx)
{
    ncc_opt_xform_data_t *d = (ncc_opt_xform_data_t *)ctx->user_data;
    return &d->option_meta;
}

static n00b_dict_t *
get_option_decls(n00b_xform_ctx_t *ctx)
{
    ncc_opt_xform_data_t *d = (ncc_opt_xform_data_t *)ctx->user_data;
    return &d->option_decls;
}

// ============================================================================
// Helpers
// ============================================================================

// Check whether a type string represents a pointer type.
static bool
type_string_is_pointer(const char *type_str)
{
    return type_str && strchr(type_str, '*') != NULL;
}

// Template parsing helper.
static n00b_parse_tree_t *
parse_template(n00b_grammar_t *g, const char *nt_name, const char *src)
{
    n00b_result_t(n00b_parse_tree_ptr_t) r =
        n00b_xform_parse_template(g, nt_name, src, NULL);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "xform_option: template parse failed for '%s':\n  %s\n",
                nt_name, src);
        return NULL;
    }
    return n00b_result_get(r);
}

// Walk up to find the variable name being declared.
// Looks for ancestor "declaration" → "init_declarator_list" →
// "init_declarator" → "declarator" → leaf identifier.
static const char *
find_declared_var_name(n00b_parse_tree_t *node)
{
    n00b_parse_tree_t *decl = n00b_xform_find_ancestor(node, "declaration");
    if (!decl) {
        return NULL;
    }

    n00b_parse_tree_t *init_list = n00b_xform_find_child_nt(
        decl, "init_declarator_list");
    if (!init_list) {
        return NULL;
    }

    n00b_parse_tree_t *init_decl = n00b_xform_find_child_nt(
        init_list, "init_declarator");
    if (!init_decl) {
        init_decl = init_list;
    }

    n00b_parse_tree_t *declarator = n00b_xform_find_child_nt(
        init_decl, "declarator");
    if (!declarator) {
        return NULL;
    }

    n00b_parse_tree_t *dd = n00b_xform_find_child_nt(
        declarator, "direct_declarator");
    if (!dd) {
        dd = declarator;
    }

    n00b_parse_tree_t *ident = n00b_xform_find_child_nt(dd, "identifier");
    if (!ident) {
        // direct_declarator might contain identifier directly or
        // through provided_identifier.
        ident = n00b_xform_find_child_nt(dd, "provided_identifier");
    }
    if (ident) {
        // Walk down to find the first leaf token.
        n00b_parse_tree_t *cur = ident;
        while (cur && !n00b_tree_is_leaf(cur)) {
            size_t cnc = n00b_tree_num_children(cur);
            n00b_parse_tree_t *next = NULL;
            for (size_t i = 0; i < cnc; i++) {
                n00b_parse_tree_t *c = n00b_tree_child(cur, i);
                if (c) {
                    if (n00b_tree_is_leaf(c)) {
                        return n00b_xform_leaf_text(c);
                    }
                    if (!next) {
                        next = c;
                    }
                }
            }
            cur = next;
        }
        if (cur && n00b_tree_is_leaf(cur)) {
            return n00b_xform_leaf_text(cur);
        }
    }

    // Fallback: first non-punctuation leaf child.
    size_t nc = n00b_tree_num_children(dd);
    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *c = n00b_tree_child(dd, i);
        if (c && n00b_tree_is_leaf(c)) {
            const char *text = n00b_xform_leaf_text(c);
            if (text && text[0] != '(' && text[0] != ')') {
                return text;
            }
        }
    }

    return NULL;
}

// Build "option, T" type string for mangling (produces unique struct name).
static char *
option_type_string(const char *type_str)
{
    size_t len = strlen("option, ") + strlen(type_str) + 1;
    char  *buf = malloc(len);
    snprintf(buf, len, "option, %s", type_str);
    return buf;
}

// Find the last leaf token in a subtree.
static n00b_token_info_t *
find_last_leaf_token(n00b_parse_tree_t *node)
{
    if (!node) {
        return NULL;
    }
    if (n00b_tree_is_leaf(node)) {
        return n00b_tree_leaf_value(node);
    }
    size_t nc = n00b_tree_num_children(node);
    for (size_t i = nc; i > 0; i--) {
        n00b_token_info_t *tok = find_last_leaf_token(
            n00b_tree_child(node, i - 1));
        if (tok) {
            return tok;
        }
    }
    return NULL;
}

// Emit a struct declaration at the translation_unit level.
// Inserts: struct <mangled> { bool has_value; <type_str> value; };
// as the first child of the root translation_unit node.
static void
emit_struct_decl(n00b_xform_ctx_t *ctx, const char *mangled,
                 const char *type_str)
{
    n00b_dict_t *decls = get_option_decls(ctx);
    if (n00b_dict_contains(decls, (void *)mangled)) {
        return;
    }
    n00b_dict_put(decls, strdup(mangled), (void *)(uintptr_t)1);

    char src[1024];
    snprintf(src, sizeof(src),
             "struct %s { _Bool has_value; %s value; };",
             mangled, type_str);

    n00b_parse_tree_t *decl_tree = parse_template(
        ctx->grammar, "external_declaration", src);
    if (!decl_tree) {
        fprintf(stderr, "ncc: error: failed to emit option struct '%s'\n",
                mangled);
        exit(1);
    }

    // Add trailing newline trivia to the last token of the struct decl
    // so it doesn't run into the next line's #line directive.
    n00b_token_info_t *last_tok = find_last_leaf_token(decl_tree);
    if (last_tok) {
        n00b_trivia_t *nl = n00b_alloc(n00b_trivia_t);
        nl->text = n00b_string_from_cstr("\n");
        nl->next = last_tok->trailing_trivia;
        last_tok->trailing_trivia = nl;
    }

    n00b_xform_insert_child(ctx->root, 0, decl_tree);
}

// ============================================================================
// Transform: _option(T) on synthetic_identifier
// ============================================================================

static n00b_parse_tree_t *
xform_option_type(n00b_xform_ctx_t *ctx, n00b_parse_tree_t *node)
{
    size_t nc = n00b_tree_num_children(node);
    if (nc < 4) {
        return NULL;
    }

    // Check first child is the _named_id_kw_option NT containing "_option".
    n00b_parse_tree_t *kw_nt = n00b_tree_child(node, 0);
    if (!kw_nt) {
        return NULL;
    }

    const char *kw_text = NULL;
    if (n00b_tree_is_leaf(kw_nt)) {
        kw_text = n00b_xform_leaf_text(kw_nt);
    }
    else if (n00b_tree_num_children(kw_nt) > 0) {
        kw_text = n00b_xform_leaf_text(n00b_tree_child(kw_nt, 0));
    }

    if (!kw_text || strcmp(kw_text, "_option") != 0) {
        return NULL;
    }

    // Extract type string from typeid_atom and optional typeid_continuation.
    n00b_parse_tree_t *atom = n00b_xform_find_child_nt(node, "typeid_atom");
    n00b_parse_tree_t *cont = n00b_xform_find_child_nt(node,
                                                        "typeid_continuation");
    if (!atom) {
        return NULL;
    }

    char *type_str   = n00b_xform_extract_type_string(ctx, atom, cont);
    bool  is_pointer = type_string_is_pointer(type_str);

    uint32_t line, col;
    n00b_xform_first_leaf_pos(node, &line, &col);

    // Record in metadata dict for _is_some/_unwrap etc.
    const char *var_name = find_declared_var_name(node);
    if (var_name) {
        n00b_dict_t *meta = get_option_meta(ctx);
        n00b_dict_put(meta, strdup(var_name),
                               (void *)(uintptr_t)is_pointer);
    }

    n00b_parse_tree_t *replacement;

    if (is_pointer) {
        // Pointer type: _option(T*) = T* (same ABI, NULL = none).
        replacement = n00b_xform_make_token_node(
            N00B_TOK_IDENTIFIER, type_str, line, col);
    }
    else {
        // Value type: _option(T) = struct <mangled>.
        char *opt_str = option_type_string(type_str);
        char *mangled = n00b_type_mangle(opt_str);
        free(opt_str);

        emit_struct_decl(ctx, mangled, type_str);

        char buf[256];
        snprintf(buf, sizeof(buf), "struct %s", mangled);
        replacement = parse_template(ctx->grammar, "type_specifier", buf);
        if (!replacement) {
            replacement = n00b_xform_make_token_node(
                N00B_TOK_IDENTIFIER, mangled, line, col);
        }

        free(mangled);
    }

    free(type_str);
    return replacement;
}

// ============================================================================
// Transform: _some/_none/_is_some/_is_none/_unwrap on postfix_expression
// ============================================================================

// Extract callee name from a postfix_expression that looks like a call.
// Returns the name if it's one of our keywords, else NULL.
static const char *
get_option_callee(n00b_parse_tree_t *node, size_t nc)
{
    if (nc < 3) {
        return NULL;
    }

    n00b_parse_tree_t *callee = n00b_tree_child(node, 0);
    if (!callee) {
        return NULL;
    }

    const char *name = NULL;
    if (n00b_tree_is_leaf(callee)) {
        name = n00b_xform_leaf_text(callee);
    }
    else {
        size_t cnc = n00b_tree_num_children(callee);
        if (cnc == 1) {
            n00b_parse_tree_t *inner = n00b_tree_child(callee, 0);
            if (inner && n00b_tree_is_leaf(inner)) {
                name = n00b_xform_leaf_text(inner);
            }
        }
    }

    if (!name) {
        return NULL;
    }

    if (strcmp(name, "_some") == 0 || strcmp(name, "_none") == 0
        || strcmp(name, "_is_some") == 0 || strcmp(name, "_is_none") == 0
        || strcmp(name, "_unwrap") == 0) {
        return name;
    }

    return NULL;
}

// Collect arguments from an argument_expression_list.
static n00b_parse_tree_t **
option_collect_args(n00b_parse_tree_t *arglist, int *nargs)
{
    *nargs = 0;
    if (!arglist) {
        return NULL;
    }

    // Count by walking the left-recursive structure.
    int count = 0;
    n00b_parse_tree_t *cur = arglist;
    while (cur && !n00b_tree_is_leaf(cur)) {
        count++;
        n00b_parse_tree_t *sub = n00b_xform_find_child_nt(
            cur, "argument_expression_list");
        if (!sub) {
            break;
        }
        cur = sub;
    }

    n00b_parse_tree_t **args = calloc((size_t)count, sizeof(*args));
    int idx = count - 1;
    cur     = arglist;
    while (cur && !n00b_tree_is_leaf(cur) && idx >= 0) {
        size_t nc = n00b_tree_num_children(cur);
        for (size_t i = nc; i > 0; i--) {
            n00b_parse_tree_t *c = n00b_tree_child(cur, i - 1);
            if (c && !n00b_tree_is_leaf(c)
                && !n00b_xform_nt_name_is(c, "argument_expression_list")) {
                args[idx--] = c;
                break;
            }
        }
        n00b_parse_tree_t *sub = n00b_xform_find_child_nt(
            cur, "argument_expression_list");
        if (!sub) {
            break;
        }
        cur = sub;
    }

    *nargs = count;
    return args;
}

// Resolve whether an expression refers to a pointer-type option.
static bool
resolve_is_pointer(n00b_xform_ctx_t *ctx, n00b_parse_tree_t *expr_node,
                   bool *out_is_pointer)
{
    char *expr_text = n00b_xform_node_to_text(expr_node);
    if (!expr_text) {
        return false;
    }

    // Trim whitespace.
    char *p = expr_text;
    while (*p == ' ' || *p == '\t') p++;
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
    char name[256];
    if (len >= sizeof(name)) len = sizeof(name) - 1;
    memcpy(name, p, len);
    name[len] = '\0';
    free(expr_text);

    n00b_dict_t *meta = get_option_meta(ctx);
    bool found;
    void *val = n00b_dict_get(meta, name, &found);
    if (found) {
        *out_is_pointer = (val != NULL);
        return true;
    }

    // Fallback: compile-time check using _Generic.
    extern char *compile_and_run(const char *, const char *, char **);
    extern char *collect_file_scope_declarations(n00b_xform_ctx_t *,
                                                 n00b_parse_tree_t *);

    ncc_opt_xform_data_t *xdata = (ncc_opt_xform_data_t *)ctx->user_data;
    const char *compiler = xdata ? xdata->compiler : NULL;
    if (!compiler) {
        compiler = "cc";
    }

    char *decls = collect_file_scope_declarations(ctx, expr_node);

    char src[4096];
    snprintf(src, sizeof(src),
             "#include <stdio.h>\n"
             "#include <stdbool.h>\n"
             "%s\n"
             "int main(void) {\n"
             "    printf(\"%%d\\n\", _Generic(&(typeof((%s))){0},\n"
             "        typeof(*(%s)) **: 1,\n"
             "        default: 0));\n"
             "}\n",
             decls ? decls : "",
             name, name);

    free(decls);

    char *err    = NULL;
    char *output = compile_and_run(compiler, src, &err);
    free(err);

    if (output) {
        *out_is_pointer = (atoi(output) != 0);
        free(output);
        return true;
    }

    return false;
}

static n00b_parse_tree_t *
xform_option_ops(n00b_xform_ctx_t *ctx, n00b_parse_tree_t *node)
{
    size_t nc = n00b_tree_num_children(node);

    const char *callee = get_option_callee(node, nc);
    if (!callee) {
        return NULL;
    }

    uint32_t line, col;
    n00b_xform_first_leaf_pos(node, &line, &col);

    // ========================================================================
    // _some(T, val) — grammar: <typeid_atom> <typeid_continuation>? "," <assignment_expression>
    // ========================================================================
    if (strcmp(callee, "_some") == 0) {
        n00b_parse_tree_t *atom = n00b_xform_find_child_nt(node, "typeid_atom");
        n00b_parse_tree_t *cont = n00b_xform_find_child_nt(
            node, "typeid_continuation");
        n00b_parse_tree_t *val_expr = n00b_xform_find_child_nt(
            node, "assignment_expression");

        if (!atom || !val_expr) {
            fprintf(stderr,
                    "ncc: error: _some requires (type, value) "
                    "(line %u, col %u)\n",
                    line, col);
            exit(1);
        }

        char *type_text = n00b_xform_extract_type_string(ctx, atom, cont);
        char *val_text  = n00b_xform_node_to_text(val_expr);
        bool  is_ptr    = type_string_is_pointer(type_text);

        char src[2048];
        if (is_ptr) {
            snprintf(src, sizeof(src), "(%s)", val_text);
        }
        else {
            char *opt_str = option_type_string(type_text);
            char *mangled = n00b_type_mangle(opt_str);
            free(opt_str);
            emit_struct_decl(ctx, mangled, type_text);

            snprintf(src, sizeof(src),
                     "((struct %s){.has_value = 1, .value = (%s)})",
                     mangled, val_text);
            free(mangled);
        }

        free(type_text);
        free(val_text);

        return parse_template(ctx->grammar, "primary_expression", src);
    }

    // ========================================================================
    // _none(T) — grammar: <typeid_atom> <typeid_continuation>?
    // ========================================================================
    if (strcmp(callee, "_none") == 0) {
        n00b_parse_tree_t *atom = n00b_xform_find_child_nt(node, "typeid_atom");
        n00b_parse_tree_t *cont = n00b_xform_find_child_nt(
            node, "typeid_continuation");

        if (!atom) {
            fprintf(stderr,
                    "ncc: error: _none requires 1 argument (type) "
                    "(line %u, col %u)\n",
                    line, col);
            exit(1);
        }

        char *type_text = n00b_xform_extract_type_string(ctx, atom, cont);
        bool  is_ptr    = type_string_is_pointer(type_text);

        char src[2048];
        if (is_ptr) {
            snprintf(src, sizeof(src), "((%s)nullptr)", type_text);
        }
        else {
            char *opt_str = option_type_string(type_text);
            char *mangled = n00b_type_mangle(opt_str);
            free(opt_str);
            emit_struct_decl(ctx, mangled, type_text);

            snprintf(src, sizeof(src),
                     "((struct %s){.has_value = 0, .value = {}})",
                     mangled);
            free(mangled);
        }

        free(type_text);

        return parse_template(ctx->grammar, "primary_expression", src);
    }

    // For _is_some/_is_none/_unwrap, use argument_expression_list as before.
    n00b_parse_tree_t *arglist = n00b_xform_find_child_nt(
        node, "argument_expression_list");
    int                nargs = 0;
    n00b_parse_tree_t **args = option_collect_args(arglist, &nargs);

    // ========================================================================
    // _is_some(x) / _is_none(x)
    // ========================================================================
    if (strcmp(callee, "_is_some") == 0 || strcmp(callee, "_is_none") == 0) {
        bool is_some = (strcmp(callee, "_is_some") == 0);

        if (nargs != 1) {
            fprintf(stderr,
                    "ncc: error: %s requires 1 argument "
                    "(line %u, col %u)\n",
                    callee, line, col);
            exit(1);
        }

        char *expr_text = n00b_xform_node_to_text(args[0]);
        bool  is_ptr    = false;

        if (!resolve_is_pointer(ctx, args[0], &is_ptr)) {
            fprintf(stderr,
                    "ncc: error: cannot determine type of '%s' for %s "
                    "(line %u, col %u)\n",
                    expr_text, callee, line, col);
            exit(1);
        }

        char src[2048];
        if (is_ptr) {
            snprintf(src, sizeof(src), "((%s) %s nullptr)",
                     expr_text, is_some ? "!=" : "==");
        }
        else {
            snprintf(src, sizeof(src), "(%s(%s).has_value)",
                     is_some ? "" : "!", expr_text);
        }

        free(expr_text);
        free(args);

        return parse_template(ctx->grammar, "primary_expression", src);
    }

    // ========================================================================
    // _unwrap(x)
    // ========================================================================
    if (strcmp(callee, "_unwrap") == 0) {
        if (nargs != 1) {
            fprintf(stderr,
                    "ncc: error: _unwrap requires 1 argument "
                    "(line %u, col %u)\n",
                    line, col);
            exit(1);
        }

        char *expr_text = n00b_xform_node_to_text(args[0]);
        bool  is_ptr    = false;

        if (!resolve_is_pointer(ctx, args[0], &is_ptr)) {
            fprintf(stderr,
                    "ncc: error: cannot determine type of '%s' for _unwrap "
                    "(line %u, col %u)\n",
                    expr_text, line, col);
            exit(1);
        }

        int  id = ctx->unique_id++;
        char var_name[64];
        snprintf(var_name, sizeof(var_name), "_ncc_opt_%d", id);

        char src[4096];
        if (is_ptr) {
            snprintf(src, sizeof(src),
                     "({ __auto_type %s = (%s);"
                     " if (%s == nullptr) { __builtin_trap(); }"
                     " %s; })",
                     var_name, expr_text,
                     var_name,
                     var_name);
        }
        else {
            snprintf(src, sizeof(src),
                     "({ __auto_type %s = (%s);"
                     " if (!%s.has_value) { __builtin_trap(); }"
                     " %s.value; })",
                     var_name, expr_text,
                     var_name,
                     var_name);
        }

        free(expr_text);
        free(args);

        return parse_template(ctx->grammar, "primary_expression", src);
    }

    free(args);
    return NULL;
}

// ============================================================================
// Registration
// ============================================================================

void
n00b_register_option_xform(n00b_xform_registry_t *reg)
{
    n00b_xform_register(reg, "synthetic_identifier", xform_option_type,
                         "option_type");
    n00b_xform_register(reg, "postfix_expression", xform_option_ops,
                         "option_ops");
}
