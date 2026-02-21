/**
 * @file st_register.c
 * @brief Symbol table registration from parse tree nodes.
 *
 * Extracts function signatures, keyword argument metadata, and
 * variadic parameter info from the parsed AST and registers them
 * in the symbol table for use by semantic transforms.  Separated
 * from st.c for clarity.
 */

#include <stdlib.h>
#include "base_alloc_shim.h"
#include "ncc_limits.h"
#include <string.h>

#include "branch_symbols.h"
#include "lex.h"
#include "rewrite.h"
#include "st.h"

// Check if a node is valid (not null and not elided)
static inline bool
is_valid_node(tnode_t *node)
{
    return node && node != (tnode_t *)&elided_node;
}

// Forward declarations
static kw_info_t    *extract_keyword_info(st_reg_ctx_t *ctx, tnode_t *kw_clause, tnode_t *declarator, const char *filename);
static tnode_t      *find_node(tnode_t *node, nt_type_t nt_id);
static vargs_info_t *detect_vargs(ncc_buf_t *input, lex_t *lex, tnode_t *declarator);

// Iterative DFS to find first token in a node tree
static tok_t *
find_first_token_recursive(tnode_t *node)
{
    if (!node || node == (tnode_t *)&elided_node) {
        return nullptr;
    }
    if (node->tptr) {
        return node->tptr;
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
        // Push children in reverse so leftmost is processed first
        for (int i = n->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (!kid || kid == (tnode_t *)&elided_node) {
                continue;
            }
            if (kid->tptr) {
                base_dealloc(stk);
                return kid->tptr;
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

// Check if ncc transformations are disabled at this node
// This includes system headers (detected via #line flag 3) and #pragma ncc off regions
static bool
is_ncc_disabled(lex_t *lex, tnode_t *node)
{
    if (!node) {
        return false;
    }

    tok_t *tok = find_first_token_recursive(node);

    if (!tok) {
        return false;
    }

    // Check if token is in an ncc_off range
    return lex_tok_is_ncc_off(lex, tok);
}

// Count positional parameters from a parameter_list node
static int
count_parameter_declarations(tnode_t *node)
{
    if (!is_valid_node(node)) {
        return 0;
    }
    if (node->nt_id == NT_parameter_declaration) {
        return 1;
    }
    if (node->nt_id == NT_parameter_list) {
        int count = 0;
        for (int i = 0; i < node->num_kids; i++) {
            count += count_parameter_declarations(tnode_get_kid(node, i));
        }
        return count;
    }
    return 0;
}

// Count positional parameters from a declarator
static int
count_positional_params(tnode_t *declarator)
{
    if (!declarator) {
        return 0;
    }
    tnode_t *func_decl = find_node(declarator, NT_function_declarator);
    if (!func_decl) {
        return 0;
    }
    tnode_t *param_list = find_node(func_decl, NT_parameter_list);
    if (!param_list) {
        return 0;
    }
    return count_parameter_declarations(param_list);
}

// Macro to iterate over valid children of a node
#define for_each_valid_kid(node, kid, i)       \
    for (int i = 0; i < (node)->num_kids; i++) \
        if ((kid = tnode_get_kid((node), i)), is_valid_node(kid))

// Detect variadic arguments in a function declarator
// Returns a newly allocated vargs_info_t, or nullptr if no vargs
// Uses grammar branches to detect style:
//   parameter_type_list branches:
// Build a type_name node from a parameter_declaration's components.
// parameter_declaration has: declaration_specifiers [declarator|abstract_declarator]
// type_name has: specifier_qualifier_list [abstract_declarator]
// We deep-copy the declaration_specifiers children into a new specifier_qualifier_list.
static tnode_t *
build_type_name_from_param_decl(tnode_t *param_decl)
{
    if (!param_decl) {
        return nullptr;
    }

    // Find declaration_specifiers in the ORIGINAL node (where nt_id is set).
    tnode_t *decl_specs = nullptr;
    tnode_t *abs_decl   = nullptr;

    for (int i = 0; i < param_decl->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(param_decl, i);
        if (!kid) {
            continue;
        }
        if (kid->nt_id == NT_declaration_specifiers) {
            decl_specs = kid;
        }
        else if (kid->nt_id == NT_abstract_declarator) {
            abs_decl = kid;
        }
    }
    if (!decl_specs) {
        return nullptr;
    }

    // Deep-copy declaration_specifiers and build a specifier_qualifier_list
    tnode_t *ds_copy = copy_tree(decl_specs);
    if (!ds_copy) {
        return nullptr;
    }

    tnode_t *sql = synth_nonterminal("specifier_qualifier_list");
    sql->nt_id   = NT_specifier_qualifier_list;
    sql->branch  = 0;

    for (int i = 0; i < ds_copy->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(ds_copy, i);
        if (kid) {
            add_child(sql, kid);
        }
    }

    // Build type_name_0: specifier_qualifier_list [abstract_declarator]
    tnode_t *type_name = synth_nonterminal("type_name_0");
    type_name->nt_id   = NT_type_name;
    type_name->branch  = 0;
    add_child(type_name, sql);

    // Copy abstract_declarator if present (e.g., for pointer types like int *)
    if (abs_decl) {
        tnode_t *abs_copy = copy_tree(abs_decl);
        if (abs_copy) {
            add_child(type_name, abs_copy);
        }
    }

    return type_name;
}

//     0: parameter_list '+'               -> n00b varargs (typed: last param = type)
//     1: parameter_list ',' '+'           -> untyped n00b with positional
//     2: '+'                              -> untyped n00b only
//     3: parameter_list ',' '...'         -> C varargs with positional
//     4: '...'                            -> C varargs only
//     5: parameter_list                   -> no vargs
static vargs_info_t *
detect_vargs(ncc_buf_t *input, lex_t *lex, tnode_t *declarator)
{
    (void)input;
    if (!declarator) {
        return nullptr;
    }

    // Find function_declarator in the declarator tree
    tnode_t *func_decl = find_node(declarator, NT_function_declarator);
    if (!func_decl) {
        return nullptr;
    }

    // Find parameter_type_list in function_declarator
    tnode_t *param_type_list = find_node(func_decl, NT_parameter_type_list);
    if (!param_type_list) {
        return nullptr;
    }

    // Check branch to determine vargs style
    vargs_info_t *info = nullptr;

    switch (param_type_list->branch) {
    case BRANCH_REF(parameter_type_list, N00B_VA_PLUS):
        // parameter_list '+' -> n00b varargs (typed: last param_decl is the type)
        if (is_ncc_disabled(lex, declarator)) {
            return nullptr;
        }
        {
            tnode_t *param_list = find_node(param_type_list, NT_parameter_list);
            int      total      = count_parameter_declarations(param_list);

            info = base_calloc(1, sizeof(vargs_info_t));
            if (!info) {
                return nullptr;
            }
            info->style = VARGS_N00B;

            // The last parameter_declaration is the vararg type.
            // Extract it and build a type_name for the type check.
            // positional_before = total - 1 (last is the type, not a real param)
            info->positional_before = total > 0 ? total - 1 : 0;

            // Find the last parameter_declaration to extract the type
            tnode_t *last_pd = nullptr;
            if (param_list) {
                for (int i = param_list->num_kids - 1; i >= 0; i--) {
                    tnode_t *kid = tnode_get_kid(param_list, i);
                    if (kid && kid->nt_id == NT_parameter_declaration) {
                        last_pd = kid;
                        break;
                    }
                }
            }
            if (last_pd) {
                // Build a type_name from the parameter_declaration's components:
                // type_name_0: specifier_qualifier_list [abstract_declarator]
                // We extract declaration_specifiers and convert to
                // specifier_qualifier_list, plus any abstract_declarator.
                tnode_t *type_name = build_type_name_from_param_decl(last_pd);
                if (type_name) {
                    info->type_node = type_name;
                }
            }
        }
        break;

    case BRANCH_REF(parameter_type_list, N00B_VA_COMMA_PLUS):
        // parameter_list ',' '+' -> untyped n00b with positional
        if (is_ncc_disabled(lex, declarator)) {
            return nullptr;
        }
        info = base_calloc(1, sizeof(vargs_info_t));
        if (!info) {
            return nullptr;
        }
        info->style             = VARGS_N00B;
        info->positional_before = count_parameter_declarations(
            find_node(param_type_list, NT_parameter_list));
        break;

    case BRANCH_REF(parameter_type_list, N00B_VA_ONLY):
        // '+' -> untyped n00b only
        if (is_ncc_disabled(lex, declarator)) {
            return nullptr;
        }
        info = base_calloc(1, sizeof(vargs_info_t));
        if (!info) {
            return nullptr;
        }
        info->style             = VARGS_N00B;
        info->positional_before = 0;
        break;

    case BRANCH_REF(parameter_type_list, C_VA_WITH_PARAMS):
        // parameter_list ',' '...' -> C varargs with positional
        info = base_calloc(1, sizeof(vargs_info_t));
        if (!info) {
            return nullptr;
        }
        info->style             = VARGS_CSTD;
        info->positional_before = count_parameter_declarations(
            find_node(param_type_list, NT_parameter_list));
        break;

    case BRANCH_REF(parameter_type_list, C_VA_ONLY):
        // '...' -> C varargs only
        info = base_calloc(1, sizeof(vargs_info_t));
        if (!info) {
            return nullptr;
        }
        info->style             = VARGS_CSTD;
        info->positional_before = 0;
        break;

    case BRANCH_REF(parameter_type_list, PARAM_LIST_ONLY):
        // Plain parameter list — no varargs.
        return nullptr;

    default:
        return nullptr;
    }

    return info;
}

// Iterative DFS tree search: find a node with a specific nt_id
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

        for (int i = n->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (!is_valid_node(kid)) {
                continue;
            }
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

// Extract the identifier name from a declarator subtree (iterative)
static char *
extract_declarator_name(st_reg_ctx_t *ctx, tnode_t *node)
{
    if (!node) {
        return nullptr;
    }

    int       cap = NCC_CAP_SMALL;
    int       top = 0;
    tnode_t **stk = base_alloc(cap * sizeof(tnode_t *));
    if (!stk) {
        return nullptr;
    }

    stk[top++] = node;

    while (top > 0) {
        tnode_t *n = stk[--top];
        if (!is_valid_node(n)) {
            continue;
        }
        if (n->nt_id == NT_identifier) {
            tok_t *tok = identifier_tok(n);
            if (tok) {
                base_dealloc(stk);
                return extract(ctx->input, tok);
            }
        }
        if (NT_IN_SET(n->nt_id, NT_SET_DECLARATORS)) {
            for (int i = n->num_kids - 1; i >= 0; i--) {
                tnode_t *kid = tnode_get_kid(n, i);
                if (is_valid_node(kid)) {
                    if (top >= cap) {
                        cap *= 2;
                        stk = base_realloc(stk, cap * sizeof(tnode_t *));
                    }
                    stk[top++] = kid;
                }
            }
        }
    }

    base_dealloc(stk);
    return nullptr;
}

// Check if declaration_specifiers contains a specific storage class keyword (iterative)
static bool
has_storage_class(st_reg_ctx_t *ctx, tnode_t *node, const char *keyword)
{
    if (!node) {
        return false;
    }

    int       cap = NCC_CAP_SMALL;
    int       top = 0;
    tnode_t **stk = base_alloc(cap * sizeof(tnode_t *));
    if (!stk) {
        return false;
    }

    stk[top++] = node;

    while (top > 0) {
        tnode_t *n = stk[--top];
        if (n->tptr) {
            char *text = extract(ctx->input, n->tptr);
            if (text && strcmp(text, keyword) == 0) {
                base_dealloc(stk);
                return true;
            }
            continue;
        }
        for (int i = n->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (is_valid_node(kid)) {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(tnode_t *));
                }
                stk[top++] = kid;
            }
        }
    }

    base_dealloc(stk);
    return false;
}

// Check if an init_declarator has an initializer (making it a definition)
static bool
has_initializer(tnode_t *init_decl)
{
    return init_decl && init_decl->nt_id == NT_init_declarator
        && init_decl->branch == BRANCH(init_declarator, WITH_INIT);
}

// Extract struct/union/enum tag name from a type specifier (iterative)
static char *
extract_tag_name(st_reg_ctx_t *ctx, tnode_t *node)
{
    if (!node) {
        return nullptr;
    }

    int       cap = NCC_CAP_SMALL;
    int       top = 0;
    tnode_t **stk = base_alloc(cap * sizeof(tnode_t *));
    if (!stk) {
        return nullptr;
    }

    stk[top++] = node;

    while (top > 0) {
        tnode_t *n = stk[--top];
        if (NT_IN_SET(n->nt_id, NT_SET_TAG_SPECIFIERS)) {
            for (int i = 0; i < n->num_kids; i++) {
                tnode_t *kid = tnode_get_kid(n, i);
                if (kid && kid->nt_id == NT_identifier) {
                    tok_t *tok = identifier_tok(kid);
                    if (tok) {
                        base_dealloc(stk);
                        return extract(ctx->input, tok);
                    }
                }
            }
        }
        for (int i = n->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (is_valid_node(kid)) {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(tnode_t *));
                }
                stk[top++] = kid;
            }
        }
    }

    base_dealloc(stk);
    return nullptr;
}

// Check if a struct/union/enum specifier has a body (iterative)
static bool
has_tag_body(tnode_t *node)
{
    if (!node) {
        return false;
    }

    int       cap = NCC_CAP_SMALL;
    int       top = 0;
    tnode_t **stk = base_alloc(cap * sizeof(tnode_t *));
    if (!stk) {
        return false;
    }

    stk[top++] = node;

    while (top > 0) {
        tnode_t *n = stk[--top];
        if (n->nt_id == NT_struct_or_union_specifier) {
            if (n->branch == BRANCH(struct_or_union_specifier, WITH_BODY)) {
                base_dealloc(stk);
                return true;
            }
        }
        if (n->nt_id == NT_enum_specifier) {
            if (n->branch == BRANCH(enum_specifier, WITH_BODY)
                || n->branch == BRANCH(enum_specifier, TRAILING_COMMA)) {
                base_dealloc(stk);
                return true;
            }
        }
        for (int i = n->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (is_valid_node(kid)) {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(tnode_t *));
                }
                stk[top++] = kid;
            }
        }
    }

    base_dealloc(stk);
    return false;
}

// Extract enum constant name from an enumerator node
static char *
extract_enum_const_name(st_reg_ctx_t *ctx, tnode_t *enumerator)
{
    if (!enumerator) {
        return nullptr;
    }
    for (int i = 0; i < enumerator->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(enumerator, i);
        if (kid->nt_id == NT_enumeration_constant) {
            for (int j = 0; j < kid->num_kids; j++) {
                tnode_t *id = tnode_get_kid(kid, j);
                if (id->nt_id == NT_identifier) {
                    tok_t *tok = identifier_tok(id);
                    if (tok) {
                        return extract(ctx->input, tok);
                    }
                }
            }
        }
    }
    return nullptr;
}

// Mark a symbol as package-local if we're in a package context at file scope.
// Skips static symbols, main, and extern declarations without prior package-local decl.
static void
maybe_mark_package_local(st_reg_ctx_t *ctx, const char *name, tnode_t *decl_specs)
{
    if (!ctx->st || !ctx->st->package_name || !name) {
        return;
    }
    // Only mark file-scope symbols
    if (ctx->st->depth != 0) {
        return;
    }
    // Skip 'main'
    if (strcmp(name, "main") == 0) {
        return;
    }
    // Skip static symbols
    if (decl_specs && has_storage_class(ctx, decl_specs, "static")) {
        return;
    }
    // Skip pure extern declarations (no prior package-local decl)
    if (decl_specs && has_storage_class(ctx, decl_specs, "extern")) {
        sym_entry_t *existing = st_get_entry(ctx->st, (char *)name);
        if (!existing || !existing->is_package_local) {
            return;
        }
    }
    sym_entry_t *entry = st_get_entry(ctx->st, (char *)name);
    if (entry) {
        entry->is_package_local = true;
    }
}

// Forward declarations for mutual recursion
static void register_declarators(st_reg_ctx_t *ctx, tnode_t *node, tnode_t *decl_specs, tnode_t *decl_node);
static void register_enum_constants(st_reg_ctx_t *ctx, tnode_t *node, tnode_t *enum_node);

// Register all declarators from an init_declarator_list
static void
register_declarators(st_reg_ctx_t *ctx, tnode_t *node, tnode_t *decl_specs, tnode_t *decl_node)
{
    if (!ctx->st || !node) {
        return;
    }
    if (node->nt_id == NT_init_declarator_list) {
        tnode_t *kid;
        for_each_valid_kid(node, kid, i)
        {
            register_declarators(ctx, kid, decl_specs, decl_node);
        }
        return;
    }
    if (node->nt_id == NT_init_declarator) {
        char *name = extract_declarator_name(ctx, node);
        if (!name) {
            return;
        }
        norm_node_t *norm_type = normalize_tokens_to_type_tree(ctx->input, decl_node);
        if (has_storage_class(ctx, decl_specs, "typedef")) {
            st_add_typedef(ctx->st, name, decl_node, norm_type);
            maybe_mark_package_local(ctx, name, decl_specs);
        }
        else {
            bool is_def = has_initializer(node) && !has_storage_class(ctx, decl_specs, "extern");
            st_add_variable(ctx->st, name, is_def ? SYM_DEFINITION : SYM_DECLARATION, decl_node, norm_type);
            maybe_mark_package_local(ctx, name, decl_specs);

            // Detect and store variadic argument info (for function declarations)
            tnode_t *declarator = find_node(node, NT_declarator);
            if (declarator) {
                vargs_info_t *vargs_info = detect_vargs(ctx->input, ctx->lex, declarator);
                if (vargs_info) {
                    st_set_vargs_info(ctx->st, name, vargs_info);
                }
            }
        }
    }
}

// Register enumeration constants from an enumerator_list
static void
register_enum_constants(st_reg_ctx_t *ctx, tnode_t *node, tnode_t *enum_node)
{
    if (!ctx->st || !node) {
        return;
    }
    if (node->nt_id == NT_enumerator_list) {
        tnode_t *kid;
        for_each_valid_kid(node, kid, i)
        {
            register_enum_constants(ctx, kid, enum_node);
        }
        return;
    }
    if (node->nt_id == NT_enumerator) {
        char *name = extract_enum_const_name(ctx, node);
        if (name) {
            norm_node_t *norm_type = normalize_tokens_to_type_tree(ctx->input, enum_node);
            st_add_enum_const(ctx->st, name, node, norm_type);
            maybe_mark_package_local(ctx, name, nullptr);
        }
    }
}

// Find and register enum constants from an enum_specifier with a body
static void
register_enum_constants_from_specifier(st_reg_ctx_t *ctx, tnode_t *enum_spec)
{
    if (!ctx->st || !enum_spec) {
        return;
    }
    for (int i = 0; i < enum_spec->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(enum_spec, i);
        if (kid->nt_id == NT_enumerator_list) {
            register_enum_constants(ctx, kid, enum_spec);
            return;
        }
    }
}

// Iteratively find and register enum constants from enum_specifiers within a node
static void
find_and_register_enum_constants(st_reg_ctx_t *ctx, tnode_t *node)
{
    if (!node || IS_ELIDED(node)) {
        return;
    }
    if (node->nt_id == NT_enum_specifier) {
        register_enum_constants_from_specifier(ctx, node);
        return;
    }

    int       cap = NCC_CAP_SMALL;
    int       top = 0;
    tnode_t **stk = base_alloc(cap * sizeof(tnode_t *));
    if (!stk) {
        return;
    }

    stk[top++] = node;

    while (top > 0) {
        tnode_t *n = stk[--top];
        for (int i = n->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (!kid || IS_ELIDED(kid)) {
                continue;
            }
            if (kid->nt_id == NT_enum_specifier) {
                register_enum_constants_from_specifier(ctx, kid);
            } else {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(tnode_t *));
                }
                stk[top++] = kid;
            }
        }
    }

    base_dealloc(stk);
}

// Register struct/union/enum tags from declaration specifiers
static void
register_tags(st_reg_ctx_t *ctx, tnode_t *decl_specs, tnode_t *decl_node)
{
    if (!ctx->st || !decl_specs) {
        return;
    }
    char *tag_name = extract_tag_name(ctx, decl_specs);
    if (tag_name) {
        bool         is_def    = has_tag_body(decl_specs);
        norm_node_t *norm_type = normalize_tokens_to_type_tree(ctx->input, decl_specs);
        st_add_tag(ctx->st, tag_name, is_def ? SYM_DEFINITION : SYM_DECLARATION, decl_node, norm_type);
        maybe_mark_package_local(ctx, tag_name, decl_specs);
    }
    if (has_tag_body(decl_specs)) {
        find_and_register_enum_constants(ctx, decl_specs);
    }
}

void
st_register_declaration(st_reg_ctx_t *ctx)
{
    if (!ctx->st || !ctx->node) {
        return;
    }
    tnode_t *decl_specs     = nullptr;
    tnode_t *init_decl_list = nullptr;
    tnode_t *declarator     = nullptr;
    tnode_t *kw_clause      = nullptr;
    tnode_t *kid;

    for_each_valid_kid(ctx->node, kid, i)
    {
        if (kid->nt_id == NT_declaration_specifiers) {
            decl_specs = kid;
        }
        else if (kid->nt_id == NT_init_declarator_list) {
            init_decl_list = kid;
        }
        else if (kid->nt_id == NT_declarator) {
            declarator = kid;
        }
        else if (kid->nt_id == NT_keyword_clause) {
            kw_clause = kid;
        }
    }

    if (decl_specs) {
        register_tags(ctx, decl_specs, ctx->node);
    }
    if (init_decl_list && decl_specs) {
        register_declarators(ctx, init_decl_list, decl_specs, ctx->node);
    }

    // Handle function declaration with keyword_clause (branch 4)
    // declaration_specifiers declarator keyword_clause ';'
    if (declarator && kw_clause && decl_specs) {
        char *name = extract_declarator_name(ctx, declarator);
        if (name) {
            norm_node_t *norm_type = normalize_tokens_to_type_tree(ctx->input, ctx->node);
            st_add_variable(ctx->st, name, SYM_DECLARATION, ctx->node, norm_type);
            maybe_mark_package_local(ctx, name, decl_specs);

            // Extract and store keyword info
            kw_info_t *kw_info = extract_keyword_info(ctx, kw_clause, declarator, nullptr);
            if (kw_info) {
                st_set_kw_info(ctx->st, name, kw_info);
            }

            // Detect and store variadic argument info
            vargs_info_t *vargs_info = detect_vargs(ctx->input, ctx->lex, declarator);
            if (vargs_info) {
                st_set_vargs_info(ctx->st, name, vargs_info);
            }
        }
    }
}

// Register function parameters from a parameter_list
static void
register_function_parameters(st_reg_ctx_t *ctx, tnode_t *node, tnode_t *func_def_node)
{
    if (!ctx->st || !node) {
        return;
    }
    if (node->nt_id == NT_parameter_list) {
        tnode_t *kid;
        for_each_valid_kid(node, kid, i)
        {
            register_function_parameters(ctx, kid, func_def_node);
        }
        return;
    }
    if (node->nt_id == NT_parameter_declaration
        && node->branch == BRANCH(parameter_declaration, WITH_DECLARATOR)) {
        for (int i = 0; i < node->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(node, i);
            if (kid->nt_id == NT_declarator) {
                char *name = extract_declarator_name(ctx, kid);
                if (name) {
                    norm_node_t *norm_type = normalize_tokens_to_type_tree(ctx->input, node);
                    st_add_variable(ctx->st, name, SYM_DEFINITION, node, norm_type);
                }
                break;
            }
        }
    }
}

// Recursively collect keyword_param nodes from a keyword_param_list into a dynamic list
static ncc_list_t *
collect_keyword_params(tnode_t *node, ncc_list_t *params)
{
    if (!is_valid_node(node)) {
        return params;
    }
    if (node->nt_id == NT_keyword_param) {
        return ncc_list_append(params, node);
    }
    if (node->nt_id == NT_keyword_param_list) {
        for (int i = 0; i < node->num_kids; i++) {
            params = collect_keyword_params(tnode_get_kid(node, i), params);
        }
    }
    return params;
}

// Extract keyword info from a keyword_clause node
// declarator is used to count positional parameters
static kw_info_t *
extract_keyword_info(st_reg_ctx_t *ctx, tnode_t *kw_clause, tnode_t *declarator, const char *filename)
{
    if (!kw_clause || kw_clause->nt_id != NT_keyword_clause) {
        return nullptr;
    }

    kw_info_t *info = base_calloc(1, sizeof(kw_info_t));
    if (!info) {
        return nullptr;
    }

    // Count positional parameters from the declarator
    info->num_positional_params = count_positional_params(declarator);

    // Check for opaque keyword passthrough (keywords: opaque)
    if (kw_clause->branch == BRANCH(keyword_clause, OPAQUE)) {
        info->is_opaque = true;
        info->params    = nullptr;
        return info;
    }

    // Find keyword_param_list child (branch 0: keywords { ... })
    tnode_t *param_list = find_node(kw_clause, NT_keyword_param_list);
    if (!param_list) {
        base_dealloc(info);
        return nullptr;
    }

    // Collect all keyword_param nodes into a temporary list
    ncc_list_t *param_nodes = nullptr;
    param_nodes         = collect_keyword_params(param_list, param_nodes);

    int num_params = ncc_list_len(param_nodes);
    if (num_params == 0) {
        base_dealloc(param_nodes);
        base_dealloc(info);
        return nullptr;
    }

    // Initialize params list (will be built via ncc_list_append)
    info->params = nullptr;

    // Extract info from each keyword_param
    for (int i = 0; i < num_params; i++) {
        tnode_t *param = ncc_list_get(param_nodes, i);

        kw_param_info_t *kw_param = base_calloc(1, sizeof(kw_param_info_t));
        if (!kw_param) {
            continue;
        }

        // Find declaration_specifiers, declarator, and initializer
        tnode_t *decl_specs    = nullptr;
        tnode_t *kw_declarator = nullptr;
        tnode_t *initializer   = nullptr;

        for (int j = 0; j < param->num_kids; j++) {
            tnode_t *kid = tnode_get_kid(param, j);
            if (!is_valid_node(kid)) {
                continue;
            }
            if (kid->nt_id == NT_declaration_specifiers) {
                decl_specs = kid;
            }
            else if (kid->nt_id == NT_declarator) {
                kw_declarator = kid;
            }
            else if (kid->nt_id == NT_initializer) {
                initializer = kid;
            }
        }

        // Extract name from declarator
        if (kw_declarator) {
            kw_param->name = extract_declarator_name(ctx, kw_declarator);
        }

        // Store tree nodes for emit
        kw_param->decl_specs = decl_specs;
        kw_param->declarator = kw_declarator;

        // Store default value node (if present)
        kw_param->default_val = initializer;
        if (initializer) {
            kw_param->default_file  = (char *)filename;
            // Get line number from first terminal in initializer
            tnode_t *first_terminal = initializer;
            while (first_terminal && !first_terminal->tptr && first_terminal->num_kids > 0) {
                first_terminal = tnode_get_kid(first_terminal, 0);
            }
            if (first_terminal && first_terminal->tptr) {
                kw_param->default_line = first_terminal->tptr->line_no;
            }
            info->defaults_set = true;
        }

        info->params = ncc_list_append(info->params, kw_param);
    }

    base_dealloc(param_nodes);
    return info;
}

void
st_register_function_definition(st_reg_ctx_t *ctx)
{
    if (!ctx->st || !ctx->node) {
        return;
    }
    for (int i = 0; i < ctx->node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(ctx->node, i);
        if (kid->nt_id == NT_declarator) {
            tnode_t *func_decl = find_node(kid, NT_function_declarator);
            if (func_decl) {
                tnode_t *param_list = find_node(func_decl, NT_parameter_list);
                if (param_list) {
                    register_function_parameters(ctx, param_list, ctx->node);
                }
            }
            return;
        }
    }
}

void
st_register_function_name(st_reg_ctx_t *ctx)
{
    if (!ctx->st || !ctx->node) {
        return;
    }

    char    *name       = nullptr;
    tnode_t *declarator = nullptr;
    tnode_t *kw_clause  = nullptr;
    tnode_t *decl_specs = nullptr;

    // Find declarator, keyword_clause, and declaration_specifiers in function_definition
    for (int i = 0; i < ctx->node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(ctx->node, i);
        if (!is_valid_node(kid)) {
            continue;
        }
        if (kid->nt_id == NT_declarator) {
            declarator = kid;
            name       = extract_declarator_name(ctx, kid);
        }
        else if (kid->nt_id == NT_keyword_clause) {
            kw_clause = kid;
        }
        else if (kid->nt_id == NT_declaration_specifiers) {
            decl_specs = kid;
        }
    }

    if (name) {
        norm_node_t *norm_type = normalize_tokens_to_type_tree(ctx->input, ctx->node);
        st_add_variable(ctx->st, name, SYM_DEFINITION, ctx->node, norm_type);
        maybe_mark_package_local(ctx, name, decl_specs);

        // If there's a keyword clause, extract and store keyword info
        if (kw_clause) {
            // TODO: Get filename from context if available
            kw_info_t *kw_info = extract_keyword_info(ctx, kw_clause, declarator, nullptr);
            if (kw_info) {
                st_set_kw_info(ctx->st, name, kw_info);
            }
        }

        // Detect and store variadic argument info
        vargs_info_t *vargs_info = detect_vargs(ctx->input, ctx->lex, declarator);
        if (vargs_info) {
            st_set_vargs_info(ctx->st, name, vargs_info);
        }
    }
}

// Find identifier token in node's children
static tok_t *
find_identifier_tok(tnode_t *node)
{
    for (int i = node->num_kids - 1; i >= 0; i--) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (is_valid_node(kid) && kid->nt_id == NT_identifier) {
            return identifier_tok(kid);
        }
    }
    return nullptr;
}

void
st_set_package(symtab_t *st, const char *name)
{
    if (!st || !name) {
        return;
    }

    if (st->package_name) {
        fprintf(stderr, "error: multiple package declarations (already '%s')\n", st->package_name);
        exit(1);
    }

    st->package_name = base_strdup(name);

    // Resolve prefix via NCC_PACKAGE_MAP
    const char *map = getenv("NCC_PACKAGE_MAP");
    if (map) {
        // Parse comma-separated key=value pairs
        char *map_copy = base_strdup(map);
        char *saveptr  = nullptr;
        char *pair     = strtok_r(map_copy, ",", &saveptr);

        while (pair) {
            char *eq = strchr(pair, '=');
            if (eq) {
                *eq = '\0';
                if (strcmp(pair, name) == 0) {
                    st->package_prefix = base_strdup(eq + 1);
                    base_dealloc(map_copy);
                    return;
                }
            }
            pair = strtok_r(nullptr, ",", &saveptr);
        }
        base_dealloc(map_copy);
    }

    // Default: use the package name as prefix
    st->package_prefix = base_strdup(name);
}

void
st_register_package(st_reg_ctx_t *ctx)
{
    if (!ctx->st || !ctx->node) {
        return;
    }

    // external_declaration_3: "package" identifier ";"
    // Find the identifier child
    tnode_t *id_node = find_node(ctx->node, NT_identifier);
    if (!id_node) {
        return;
    }

    tok_t *tok = identifier_tok(id_node);
    if (!tok) {
        return;
    }

    char *name = extract(ctx->input, tok);
    if (!name) {
        return;
    }

    st_set_package(ctx->st, name);
    base_dealloc(name);
}

void
st_register_label_def(st_reg_ctx_t *ctx)
{
    if (!ctx->lt || !ctx->node) {
        return;
    }
    tok_t *tok = find_identifier_tok(ctx->node);
    if (tok) {
        lt_add_definition(ctx->lt, extract(ctx->input, tok), ctx->node);
    }
}

void
st_register_label_ref(st_reg_ctx_t *ctx)
{
    if (!ctx->lt || !ctx->node) {
        return;
    }
    tok_t *tok = find_identifier_tok(ctx->node);
    if (tok) {
        lt_add_reference(ctx->lt, extract(ctx->input, tok), ctx->node);
    }
}
