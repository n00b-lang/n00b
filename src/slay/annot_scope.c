// annot_scope.c - Phase 1 (scope open) and Phase 7 (scope close).

#include "internal/slay/annot_phases.h"

// ============================================================================
// Phase 1: scope open
// ============================================================================

void
annot_phase_scope_open(n00b_annot_walk_ctx_t *ctx, annot_node_ctx_t *nc)
{
    for (size_t ai = 0; ai < nc->annot_count; ai++) {
        n00b_annotation_t *a = nc->annots[ai];

        if (a->kind == N00B_ANNOT_SCOPE_OPEN) {
            nc->scope_ns = n00b_string_empty();

            n00b_string_t scope_name = a->scope_tag;

            if (a->name_ref.kind == N00B_ROLE_BY_INDEX
                    ? a->name_ref.index >= 0
                    : a->name_ref.name.data != NULL) {
                n00b_parse_tree_t *name_node
                    = n00b_tree_resolve_child_ref(
                        ctx->grammar, nc->node, a->name_ref);

                if (name_node) {
                    n00b_string_t resolved
                        = n00b_tree_extract_first_identifier(name_node);

                    if (resolved.u8_bytes > 0) {
                        scope_name = resolved;
                    }
                }
            }

            n00b_symtab_push_scope(ctx->symtab, nc->scope_ns, scope_name);
            nc->pn->scope    = n00b_symtab_current_scope(ctx->symtab,
                                                          nc->scope_ns);
            nc->pn->scope->scope_tag = a->scope_tag;
            nc->opened_scope = true;

            // For function scopes, add a "$return" symbol so that
            // @infer("$return") can find the return type variable.
            if (ctx->tc_ctx && a->scope_tag.u8_bytes > 0
                && n00b_unicode_str_eq(a->scope_tag, *r"function")) {
                n00b_sym_entry_t *ret_sym = n00b_symtab_add(
                    ctx->symtab, n00b_string_empty(),
                    *r"$return", N00B_SYM_VARIABLE, nc->node);
                ret_sym->type_var = n00b_tc_fresh_var(ctx->tc_ctx);
            }
        }
        else if (a->kind == N00B_ANNOT_ADT) {
            nc->scope_ns = n00b_string_empty();

            n00b_parse_tree_t *name_node
                = n00b_tree_resolve_child_ref(
                    ctx->grammar, nc->node, a->name_ref);
            n00b_string_t scope_name
                = name_node
                    ? n00b_tree_extract_first_identifier(name_node)
                    : n00b_string_empty();

            if (scope_name.u8_bytes == 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "anon_%d", ctx->anon_counter++);
                scope_name = n00b_string_from_cstr(buf);
            }

            n00b_symtab_push_scope(ctx->symtab, nc->scope_ns, scope_name);

            n00b_scope_t *scope
                = n00b_symtab_current_scope(ctx->symtab, nc->scope_ns);

            if (scope) {
                nc->pn->scope = scope;

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

                scope->adt_kind  = kind;
                scope->scope_tag = a->scope_tag.data ? a->scope_tag
                                                     : a->adt_kind;
            }

            nc->opened_scope = true;
        }
    }
}

// ============================================================================
// Phase 7: scope close
// ============================================================================

void
annot_phase_scope_close(n00b_annot_walk_ctx_t *ctx, annot_node_ctx_t *nc)
{
    if (nc->opened_scope) {
        n00b_symtab_pop_scope(ctx->symtab, nc->scope_ns);
    }
}
