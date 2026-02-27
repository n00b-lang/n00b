// annot_symtab.c - Phases 2-3: two-pass symbol registration + type binding.
//
// Pass A: symbol-creating annotations (DECLARES, TYPE_DECL, ADT, FIELD, METHOD)
//         → sets nc->last_sym.  Each symbol with a type_node gets type-bound
//         immediately, so multiple symbol-creators on the same rule (e.g.,
//         @type($1) @declares($1) on enum-stmt) all resolve correctly.
// Pass B: symbol-reading annotations (TYPE, LITERAL)
//         → reads nc->last_sym (guaranteed set by pass A).
//
// This two-pass design fixes the ordering bug where @type silently fails
// when it appears before @declares in the grammar rule.

#include "internal/slay/annot_phases.h"

// Translate type_node → concrete type and unify with the symbol's type_var.
static void
bind_sym_type(n00b_annot_walk_ctx_t *ctx, n00b_sym_entry_t *sym)
{
    if (sym && sym->type_node && ctx->tc_ctx && ctx->translate_type_spec) {
        n00b_tc_type_t *declared = ctx->translate_type_spec(
            ctx->tc_ctx, ctx->grammar, sym->type_node);

        if (declared) {
            n00b_tc_unify(ctx->tc_ctx, sym->type_var, declared);
        }
    }
}

void
annot_phase_symtab(n00b_annot_walk_ctx_t *ctx, annot_node_ctx_t *nc)
{
    // ---- Pass A: symbol-creating annotations ----

    for (size_t ai = 0; ai < nc->annot_count; ai++) {
        n00b_annotation_t *a = nc->annots[ai];

        switch (a->kind) {
        case N00B_ANNOT_DECLARES: {
            n00b_parse_tree_t *name_node
                = n00b_tree_resolve_child_ref(
                    ctx->grammar, nc->node, a->name_ref);
            n00b_string_t sym_name
                = name_node
                    ? n00b_tree_extract_first_identifier(name_node)
                    : n00b_string_empty();

            if (sym_name.u8_bytes > 0) {
                n00b_sym_kind_t sym_kind = N00B_SYM_VARIABLE;

                if (a->sym_kind.u8_bytes > 0) {
                    if (n00b_unicode_str_eq(a->sym_kind, *r"param")) {
                        sym_kind = N00B_SYM_PARAM;
                    }
                    else if (n00b_unicode_str_eq(a->sym_kind, *r"function")) {
                        sym_kind = N00B_SYM_FUNCTION;
                    }
                }

                nc->last_sym = n00b_symtab_add(ctx->symtab,
                                                n00b_string_empty(),
                                                sym_name,
                                                sym_kind,
                                                nc->node);

                if (nc->last_sym->shadowed && ctx->shadowed_entries) {
                    n00b_list_push(*ctx->shadowed_entries, nc->last_sym);
                }

                if (ctx->tc_ctx) {
                    nc->last_sym->type_var = n00b_tc_fresh_var(ctx->tc_ctx);
                }

                // If @declares($name, $type) has a type_ref, set type_node
                // so type binding can bind the explicit annotation.
                if (a->type_ref.kind == N00B_ROLE_BY_INDEX
                        ? a->type_ref.index >= 0
                        : a->type_ref.name.data != NULL) {
                    n00b_parse_tree_t *tnode
                        = n00b_tree_resolve_child_ref(
                            ctx->grammar, nc->node, a->type_ref);

                    if (tnode) {
                        nc->last_sym->type_node = tnode;
                    }
                }

                bind_sym_type(ctx, nc->last_sym);

                if (sym_kind == N00B_SYM_PARAM && ctx->params) {
                    n00b_list_push(*ctx->params, nc->last_sym);
                }
            }

            break;
        }

        case N00B_ANNOT_TYPE_DECL: {
            n00b_parse_tree_t *name_node
                = n00b_tree_resolve_child_ref(
                    ctx->grammar, nc->node, a->name_ref);
            n00b_string_t sym_name
                = name_node
                    ? n00b_tree_extract_first_identifier(name_node)
                    : n00b_string_empty();

            if (sym_name.u8_bytes > 0) {
                nc->last_sym = n00b_symtab_add(ctx->symtab,
                                                n00b_string_empty(),
                                                sym_name,
                                                N00B_SYM_TYPEDEF,
                                                nc->node);

                if (ctx->tc_ctx) {
                    nc->last_sym->type_var = n00b_tc_fresh_var(ctx->tc_ctx);
                }

                // Set type_node so type binding translates the name child
                // into a named (prim) type and unifies it with type_var.
                nc->last_sym->type_node = name_node;
                bind_sym_type(ctx, nc->last_sym);
            }

            break;
        }

        case N00B_ANNOT_ADT: {
            n00b_parse_tree_t *name_node
                = n00b_tree_resolve_child_ref(
                    ctx->grammar, nc->node, a->name_ref);
            n00b_string_t tag_name
                = name_node
                    ? n00b_tree_extract_first_identifier(name_node)
                    : n00b_string_empty();

            if (tag_name.u8_bytes > 0) {
                n00b_sym_entry_t *sym = n00b_symtab_add(
                    ctx->symtab,
                    *r"tag",
                    tag_name,
                    N00B_SYM_TAG,
                    nc->node);

                n00b_string_t kind = a->adt_kind;

                if (a->adt_keyword_ref.kind == N00B_ROLE_BY_INDEX
                        ? a->adt_keyword_ref.index >= 0
                        : a->adt_keyword_ref.name.data != NULL) {
                    n00b_parse_tree_t *kw_node
                        = n00b_tree_resolve_child_ref(
                            ctx->grammar, nc->node, a->adt_keyword_ref);

                    if (kw_node) {
                        n00b_string_t kw
                            = n00b_tree_extract_first_identifier(kw_node);

                        if (kw.u8_bytes > 0) {
                            kind = kw;
                        }
                    }
                }

                sym->adt_kind = kind;
                nc->last_sym  = sym;

                if (ctx->tc_ctx) {
                    nc->last_sym->type_var = n00b_tc_fresh_var(ctx->tc_ctx);
                }
            }

            break;
        }

        case N00B_ANNOT_FIELD: {
            n00b_parse_tree_t *name_node
                = n00b_tree_resolve_child_ref(
                    ctx->grammar, nc->node, a->name_ref);
            n00b_string_t field_name
                = name_node
                    ? n00b_tree_extract_first_identifier(name_node)
                    : n00b_string_empty();

            if (field_name.u8_bytes > 0) {
                n00b_parse_tree_t *type_node
                    = n00b_tree_resolve_child_ref(
                        ctx->grammar, nc->node, a->type_ref);

                n00b_sym_entry_t *sym = n00b_symtab_add(
                    ctx->symtab,
                    n00b_string_empty(),
                    field_name,
                    N00B_SYM_VARIABLE,
                    nc->node);

                sym->is_field  = true;
                sym->type_node = type_node;
                nc->last_sym   = sym;

                if (ctx->tc_ctx) {
                    nc->last_sym->type_var = n00b_tc_fresh_var(ctx->tc_ctx);
                }

                bind_sym_type(ctx, nc->last_sym);
            }

            break;
        }

        case N00B_ANNOT_METHOD: {
            n00b_parse_tree_t *name_node
                = n00b_tree_resolve_child_ref(
                    ctx->grammar, nc->node, a->name_ref);
            n00b_string_t method_name
                = name_node
                    ? n00b_tree_extract_first_identifier(name_node)
                    : n00b_string_empty();

            if (method_name.u8_bytes > 0) {
                n00b_parse_tree_t *type_node
                    = n00b_tree_resolve_child_ref(
                        ctx->grammar, nc->node, a->type_ref);

                n00b_sym_entry_t *sym = n00b_symtab_add(
                    ctx->symtab,
                    n00b_string_empty(),
                    method_name,
                    N00B_SYM_FUNCTION,
                    nc->node);

                sym->is_method = true;
                sym->type_node = type_node;
                nc->last_sym   = sym;

                if (ctx->tc_ctx) {
                    nc->last_sym->type_var = n00b_tc_fresh_var(ctx->tc_ctx);
                }

                bind_sym_type(ctx, nc->last_sym);
            }

            break;
        }

        default:
            break;
        }
    }

    // ---- Pass B: symbol-reading annotations (last_sym guaranteed set) ----

    for (size_t ai = 0; ai < nc->annot_count; ai++) {
        n00b_annotation_t *a = nc->annots[ai];

        switch (a->kind) {
        case N00B_ANNOT_TYPE: {
            if (nc->last_sym) {
                n00b_parse_tree_t *type_node
                    = n00b_tree_resolve_child_ref(
                        ctx->grammar, nc->node, a->name_ref);

                if (type_node) {
                    nc->last_sym->type_node = type_node;
                    bind_sym_type(ctx, nc->last_sym);
                }
            }

            break;
        }

        case N00B_ANNOT_EXPOSES: {
            if (nc->last_sym && nc->pn->scope) {
                nc->last_sym->exposed_scope = nc->pn->scope;
            }

            break;
        }

        case N00B_ANNOT_LITERAL: {
            n00b_string_t type_name = a->op_kind;
            n00b_tc_type_t *lit_type = NULL;

            if (type_name.u8_bytes > 0 && ctx->tc_ctx && ctx->node_types) {
                lit_type = n00b_tc_lookup_prim(ctx->tc_ctx, type_name);
            }

            if (ctx->tc_ctx && ctx->translate_type_spec
                && (a->type_ref.kind == N00B_ROLE_BY_INDEX
                        ? a->type_ref.index >= 0
                        : a->type_ref.name.data != NULL)) {
                n00b_parse_tree_t *tspec
                    = n00b_tree_resolve_child_ref(
                        ctx->grammar, nc->node, a->type_ref);

                if (tspec) {
                    n00b_tc_type_t *mod_type = ctx->translate_type_spec(
                        ctx->tc_ctx, ctx->grammar, tspec);

                    if (mod_type) {
                        lit_type = mod_type;
                    }
                }
            }

            if (lit_type && ctx->node_types) {
                uintptr_t key = (uintptr_t)nc->node;
                n00b_dict_put(ctx->node_types, key, lit_type);
            }

            break;
        }

        default:
            break;
        }
    }
}
