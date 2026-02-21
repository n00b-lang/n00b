/**
 * @file xform_package.c
 * @brief Tree-based transformation for the `package` namespace declaration.
 *
 * Transforms `package name;` declarations:
 * - Suppresses the package statement in output
 * - Prefixes all package-local identifiers (functions, variables, typedefs,
 *   struct/union/enum tags, enum constants) with the package prefix
 * - Resolves prefix via NCC_PACKAGE_MAP environment variable
 *
 * Uses proper tree node operations - NO token replacement.
 */

#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include "ncc_limits.h"
#include <string.h>

#include "branch_symbols.h"
#include "transform.h"
#include "xform_helpers.h"
#include "nt_types.h"

// ---------------------------------------------------------------------------
// Helper: Node Validation
// ---------------------------------------------------------------------------

static inline bool
is_valid_node(tnode_t *node)
{
    extern const tnode_t elided_node;
    return node && node != (tnode_t *)&elided_node;
}

#define for_each_valid_kid(node, kid, i)       \
    for (int i = 0; i < (node)->num_kids; i++) \
        if ((kid = tnode_get_kid((node), i)), is_valid_node(kid))

// ---------------------------------------------------------------------------
// Helper: Build a prefixed name
// ---------------------------------------------------------------------------

static char *
build_prefixed_name(const char *prefix, const char *name)
{
    size_t len = strlen(prefix) + 1 + strlen(name) + 1;
    char  *buf = base_alloc(len);
    if (!buf) {
        return nullptr;
    }
    snprintf(buf, len, "%s_%s", prefix, name);
    return buf;
}

// ---------------------------------------------------------------------------
// Helper: Extract identifier name from declarator subtree
// ---------------------------------------------------------------------------

static char *
extract_declarator_name(ncc_buf_t *input, tnode_t *node)
{
    if (!is_valid_node(node)) {
        return nullptr;
    }

    if (node->nt_id == NT_identifier) {
        tok_t *tok = identifier_tok(node);
        if (tok) {
            return extract(input, tok);
        }
    }

    if (NT_IN_SET(node->nt_id, NT_SET_DECLARATORS) || node->nt_id == NT_direct_declarator) {
        tnode_t *kid;
        for_each_valid_kid(node, kid, i)
        {
            char *name = extract_declarator_name(input, kid);
            if (name) {
                return name;
            }
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Helper: Iterative DFS find
// ---------------------------------------------------------------------------

static tnode_t *
find_node(tnode_t *node, nt_type_t nt_id)
{
    if (!is_valid_node(node)) {
        return nullptr;
    }
    if (node->nt_id == nt_id) {
        return node;
    }

    int       cap = NCC_CAP_MEDIUM;
    int       top = 0;
    tnode_t **stk = base_alloc(cap * sizeof(tnode_t *));
    if (!stk) {
        return nullptr;
    }

    stk[top++] = node;

    while (top > 0) {
        tnode_t *n = stk[--top];
        tnode_t *kid;
        for_each_valid_kid(n, kid, i)
        {
            if (kid->nt_id == nt_id) {
                base_dealloc(stk);
                return kid;
            }
            if (top >= cap) {
                cap *= 2;
                stk = base_realloc(stk, cap * sizeof(tnode_t *));
            }
            stk[top++] = kid;
        }
    }

    base_dealloc(stk);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Core: Replace an identifier node with a prefixed version
// Returns true if replacement was made.
// ---------------------------------------------------------------------------

static bool
replace_identifier_with_prefix(ncc_buf_t *input, tnode_t *parent, int child_idx, const char *prefix, int line)
{
    tnode_t *id_node = tnode_get_kid(parent, child_idx);
    if (!id_node || id_node->nt_id != NT_identifier) {
        return false;
    }

    tok_t *tok = identifier_tok(id_node);
    if (!tok) {
        return false;
    }

    char *old_name = extract(input, tok);
    if (!old_name) {
        return false;
    }

    char *new_name = build_prefixed_name(prefix, old_name);
    base_dealloc(old_name);
    if (!new_name) {
        return false;
    }

    tnode_t *new_id = build_identifier(new_name, line);
    replace_child(parent, child_idx, new_id, "package");
    base_dealloc(new_name);
    return true;
}

// ---------------------------------------------------------------------------
// Helper: Find and prefix the identifier in a declarator subtree
// ---------------------------------------------------------------------------

static bool
prefix_declarator_name(ncc_buf_t *input, tnode_t *node, const char *prefix, int line)
{
    if (!is_valid_node(node)) {
        return false;
    }

    // direct_declarator can contain the identifier as a child
    if (node->nt_id == NT_direct_declarator) {
        for (int i = 0; i < node->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(node, i);
            if (kid && kid->nt_id == NT_identifier) {
                return replace_identifier_with_prefix(input, node, i, prefix, line);
            }
        }
    }

    // Recurse through declarator structure
    if (NT_IN_SET(node->nt_id, NT_SET_DECLARATORS) || node->nt_id == NT_direct_declarator) {
        tnode_t *kid;
        for_each_valid_kid(node, kid, i)
        {
            if (prefix_declarator_name(input, kid, prefix, line)) {
                return true;
            }
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Helper: Check if declaration specifiers contain a storage class keyword
// ---------------------------------------------------------------------------

static bool
has_storage_class(ncc_buf_t *input, tnode_t *node, const char *keyword)
{
    if (!node) {
        return false;
    }
    if (node->tptr) {
        char *text = extract(input, node->tptr);
        if (text && strcmp(text, keyword) == 0) {
            base_dealloc(text);
            return true;
        }
        base_dealloc(text);
    }
    tnode_t *kid;
    for_each_valid_kid(node, kid, i)
    {
        if (has_storage_class(input, kid, keyword)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Helper: Prefix a struct/union/enum tag name in a type specifier
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Transform: Suppress the package statement itself
// ---------------------------------------------------------------------------

static tnode_t *
xform_package_statement(tree_xform_t *ctx [[maybe_unused]], tnode_t *node)
{
    // Only match external_declaration branch 3 (package statement)
    if (node->branch != BRANCH(external_declaration, PACKAGE)) {
        return nullptr;
    }

    // Suppress the entire package statement from output
    mark_skip_emit(node);

    return nullptr;
}

// ---------------------------------------------------------------------------
// Transform: Prefix function definitions
// ---------------------------------------------------------------------------

static tnode_t *
xform_package_func_def(tree_xform_t *ctx, tnode_t *node)
{
    if (!ctx->symtab || !ctx->symtab->package_prefix) {
        return nullptr;
    }

    const char *prefix = ctx->symtab->package_prefix;

    // Find children
    tnode_t *decl_specs = nullptr;
    tnode_t *declarator = nullptr;

    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (!is_valid_node(kid)) {
            continue;
        }
        if (kid->nt_id == NT_declaration_specifiers) {
            decl_specs = kid;
        }
        else if (kid->nt_id == NT_declarator) {
            declarator = kid;
        }
    }

    if (!declarator) {
        return nullptr;
    }

    // Skip static functions
    if (decl_specs && has_storage_class(ctx->input, decl_specs, "static")) {
        return nullptr;
    }

    // Extract name to check if it's package-local
    char *name = extract_declarator_name(ctx->input, declarator);
    if (!name) {
        return nullptr;
    }

    // Skip main
    if (strcmp(name, "main") == 0) {
        base_dealloc(name);
        return nullptr;
    }

    sym_entry_t *entry = st_get_entry(ctx->symtab, name);
    if (!entry || !entry->is_package_local) {
        base_dealloc(name);
        return nullptr;
    }

    int line = get_node_line(node);

    // Prefix the function name in the declarator
    prefix_declarator_name(ctx->input, declarator, prefix, line);

    base_dealloc(name);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Transform: Prefix declarations (variables, typedefs, function decls)
// ---------------------------------------------------------------------------

static tnode_t *
xform_package_declaration(tree_xform_t *ctx, tnode_t *node)
{
    if (!ctx->symtab || !ctx->symtab->package_prefix) {
        return nullptr;
    }

    const char *prefix = ctx->symtab->package_prefix;

    tnode_t *decl_specs     = nullptr;
    tnode_t *init_decl_list = nullptr;
    tnode_t *declarator     = nullptr;

    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (!is_valid_node(kid)) {
            continue;
        }
        if (kid->nt_id == NT_declaration_specifiers) {
            decl_specs = kid;
        }
        else if (kid->nt_id == NT_init_declarator_list) {
            init_decl_list = kid;
        }
        else if (kid->nt_id == NT_declarator) {
            declarator = kid;
        }
    }

    // Skip static declarations
    if (decl_specs && has_storage_class(ctx->input, decl_specs, "static")) {
        return nullptr;
    }

    int line = get_node_line(node);

    // Prefix declarators in init_declarator_list
    if (init_decl_list) {
        // Walk init_declarator nodes
        for (int i = 0; i < init_decl_list->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(init_decl_list, i);
            if (!is_valid_node(kid)) {
                continue;
            }
            if (kid->nt_id == NT_init_declarator) {
                tnode_t *decl = find_node(kid, NT_declarator);
                if (decl) {
                    char *name = extract_declarator_name(ctx->input, decl);
                    if (name) {
                        // Skip main
                        if (strcmp(name, "main") != 0) {
                            sym_entry_t *entry = st_get_entry(ctx->symtab, name);
                            if (entry && entry->is_package_local) {
                                prefix_declarator_name(ctx->input, decl, prefix, line);
                            }
                        }
                        base_dealloc(name);
                    }
                }
            }
        }
    }

    // Prefix standalone declarator (function decl with keywords)
    if (declarator) {
        char *name = extract_declarator_name(ctx->input, declarator);
        if (name) {
            if (strcmp(name, "main") != 0) {
                sym_entry_t *entry = st_get_entry(ctx->symtab, name);
                if (entry && entry->is_package_local) {
                    prefix_declarator_name(ctx->input, declarator, prefix, line);
                }
            }
            base_dealloc(name);
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Transform: Prefix identifier references in expressions
// Handles:
//   - primary_expression -> identifier (variable references)
//   - postfix_expression -> function calls (the function name identifier)
// ---------------------------------------------------------------------------

static tnode_t *
xform_package_primary_expr(tree_xform_t *ctx, tnode_t *node)
{
    if (!ctx->symtab || !ctx->symtab->package_prefix) {
        return nullptr;
    }

    // primary_expression: identifier
    if (node->branch != BRANCH(primary_expression, IDENTIFIER)) {
        return nullptr;
    }

    tnode_t *id_node = find_child(node, NT_identifier);
    if (!id_node) {
        return nullptr;
    }

    tok_t *tok = identifier_tok(id_node);
    if (!tok) {
        return nullptr;
    }

    char *name = extract(ctx->input, tok);
    if (!name) {
        return nullptr;
    }

    sym_entry_t *entry = st_get_entry(ctx->symtab, name);
    if (!entry || !entry->is_package_local) {
        base_dealloc(name);
        return nullptr;
    }

    const char *prefix   = ctx->symtab->package_prefix;
    char       *new_name = build_prefixed_name(prefix, name);
    base_dealloc(name);
    if (!new_name) {
        return nullptr;
    }

    int line = get_node_line(node);

    // Replace the identifier child of primary_expression
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (kid && kid->nt_id == NT_identifier) {
            tnode_t *new_id = build_identifier(new_name, line);
            replace_child(node, i, new_id, "package");
            break;
        }
    }

    base_dealloc(new_name);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Transform: Prefix struct/union/enum tag names everywhere
// ---------------------------------------------------------------------------

static tnode_t *
xform_package_tag_specifier(tree_xform_t *ctx, tnode_t *node)
{
    if (!ctx->symtab || !ctx->symtab->package_prefix) {
        return nullptr;
    }

    // Find the identifier child (tag name)
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (!kid || kid->nt_id != NT_identifier) {
            continue;
        }

        tok_t *tok = identifier_tok(kid);
        if (!tok) {
            continue;
        }

        char *name = extract(ctx->input, tok);
        if (!name) {
            continue;
        }

        sym_entry_t *entry = st_get_entry(ctx->symtab, name);
        if (!entry || !entry->is_package_local) {
            base_dealloc(name);
            continue;
        }

        const char *prefix = ctx->symtab->package_prefix;
        int         line   = get_node_line(node);
        replace_identifier_with_prefix(ctx->input, node, i, prefix, line);
        base_dealloc(name);
        return nullptr;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Transform: Prefix typedef name references
// ---------------------------------------------------------------------------

static tnode_t *
xform_package_typedef_name(tree_xform_t *ctx, tnode_t *node)
{
    if (!ctx->symtab || !ctx->symtab->package_prefix) {
        return nullptr;
    }

    // typedef_name has a typedef_name_terminal child that holds the token
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (!kid || !kid->tptr) {
            continue;
        }

        char *name = extract(ctx->input, kid->tptr);
        if (!name) {
            continue;
        }

        sym_entry_t *entry = st_get_entry(ctx->symtab, name);
        if (!entry || !entry->is_package_local) {
            base_dealloc(name);
            continue;
        }

        const char *prefix   = ctx->symtab->package_prefix;
        char       *new_name = build_prefixed_name(prefix, name);
        base_dealloc(name);
        if (!new_name) {
            continue;
        }

        int      line    = get_node_line(node);
        tnode_t *new_tok = synth_terminal(new_name, TT_ID, line);
        new_tok->nt_id   = kid->nt_id;
        replace_child(node, i, new_tok, "package");
        base_dealloc(new_name);
        return nullptr;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Transform: Prefix enum constants in their definitions
// ---------------------------------------------------------------------------

static tnode_t *
xform_package_enum_const(tree_xform_t *ctx, tnode_t *node)
{
    if (!ctx->symtab || !ctx->symtab->package_prefix) {
        return nullptr;
    }

    // enumeration_constant has an identifier child
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (!kid || kid->nt_id != NT_identifier) {
            continue;
        }

        tok_t *tok = identifier_tok(kid);
        if (!tok) {
            continue;
        }

        char *name = extract(ctx->input, tok);
        if (!name) {
            continue;
        }

        sym_entry_t *entry = st_get_entry(ctx->symtab, name);
        if (!entry || !entry->is_package_local) {
            base_dealloc(name);
            continue;
        }

        const char *prefix = ctx->symtab->package_prefix;
        int         line   = get_node_line(node);
        replace_identifier_with_prefix(ctx->input, node, i, prefix, line);
        base_dealloc(name);
        return nullptr;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void
register_package_xform(xform_registry_t *reg)
{
    xform_register_post(reg, NT_external_declaration, xform_package_statement, "package_stmt");
    xform_register_post(reg, NT_function_definition, xform_package_func_def, "package_func");
    xform_register_post(reg, NT_declaration, xform_package_declaration, "package_decl");
    xform_register_post(reg, NT_primary_expression, xform_package_primary_expr, "package_ref");
    xform_register_post(reg, NT_typedef_name, xform_package_typedef_name, "package_typedef");
    xform_register_post(reg, NT_struct_or_union_specifier, xform_package_tag_specifier, "package_tag");
    xform_register_post(reg, NT_enum_specifier, xform_package_tag_specifier, "package_enum");
    xform_register_post(reg, NT_enumeration_constant, xform_package_enum_const, "package_enum_const");
}
