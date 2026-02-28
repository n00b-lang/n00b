// annot_cf.c - Phase 4: control flow label creation.

#include "internal/slay/annot_phases.h"

// Allocate a CF label, set its kind and self node, and register it.
static n00b_cf_label_t *
make_cf_label(n00b_annot_walk_ctx_t *ctx, n00b_parse_tree_t *node,
              n00b_cf_kind_t kind)
{
    n00b_cf_label_t *label = n00b_alloc(n00b_cf_label_t);
    label->kind = kind;
    label->self = node;
    n00b_dict_put(ctx->cf_labels, node, label);
    return label;
}

void
annot_phase_cf(n00b_annot_walk_ctx_t *ctx, annot_node_ctx_t *nc)
{
    for (size_t ai = 0; ai < nc->annot_count; ai++) {
        n00b_annotation_t *a = nc->annots[ai];

        switch (a->kind) {
        case N00B_ANNOT_BRANCH: {
            n00b_cf_label_t *label = make_cf_label(ctx, nc->node,
                                                    N00B_CF_BRANCH);
            label->cond      = n00b_tree_resolve_child_ref(
                                   ctx->grammar, nc->node, a->name_ref);
            label->then_body = n00b_tree_resolve_child_ref(
                                   ctx->grammar, nc->node, a->type_ref);
            label->else_body = n00b_tree_resolve_child_ref(
                                   ctx->grammar, nc->node, a->value_ref);
            break;
        }

        case N00B_ANNOT_LOOP: {
            n00b_cf_label_t *label = make_cf_label(ctx, nc->node,
                                                    N00B_CF_LOOP);
            label->cond      = n00b_tree_resolve_child_ref(
                                   ctx->grammar, nc->node, a->name_ref);
            label->then_body = n00b_tree_resolve_child_ref(
                                   ctx->grammar, nc->node, a->type_ref);
            break;
        }

        case N00B_ANNOT_SWITCH: {
            n00b_cf_label_t *label = make_cf_label(ctx, nc->node,
                                                    N00B_CF_SWITCH);
            label->cond      = n00b_tree_resolve_child_ref(
                                   ctx->grammar, nc->node, a->name_ref);
            label->then_body = n00b_tree_resolve_child_ref(
                                   ctx->grammar, nc->node, a->type_ref);
            break;
        }

        case N00B_ANNOT_JUMP: {
            n00b_cf_label_t *label = make_cf_label(ctx, nc->node,
                                                    N00B_CF_JUMP);
            label->jump_kind = a->scope_tag;

            if (!label->jump_kind || !label->jump_kind->data) {
                label->jump_kind
                    = n00b_tree_extract_first_identifier(nc->node);
            }

            break;
        }

        case N00B_ANNOT_CAPTURE: {
            n00b_cf_label_t *label = make_cf_label(ctx, nc->node,
                                                    N00B_CF_CAPTURE);
            label->tag            = a->scope_tag;
            label->capture_by_tag = a->capture_by_tag;
            break;
        }

        case N00B_ANNOT_ASSIGNS: {
            n00b_cf_label_t *label = make_cf_label(ctx, nc->node,
                                                    N00B_CF_ASSIGNS);
            label->cond      = n00b_tree_resolve_child_ref(
                                   ctx->grammar, nc->node, a->name_ref);
            label->then_body = n00b_tree_resolve_child_ref(
                                   ctx->grammar, nc->node, a->value_ref);
            break;
        }

        case N00B_ANNOT_VARREF: {
            n00b_cf_label_t *label = make_cf_label(ctx, nc->node,
                                                    N00B_CF_VARREF);
            label->cond = n00b_tree_resolve_child_ref(
                              ctx->grammar, nc->node, a->name_ref);
            break;
        }

        case N00B_ANNOT_OPERATOR: {
            if (a->op_kind->u8_bytes > 0
                && n00b_unicode_str_eq(a->op_kind, r"unwrap_result")) {
                make_cf_label(ctx, nc->node, N00B_CF_UNWRAP_RESULT);
            }
            break;
        }

        case N00B_ANNOT_CALL: {
            n00b_cf_label_t *label = make_cf_label(ctx, nc->node,
                                                    N00B_CF_CALL);
            label->cond      = n00b_tree_resolve_child_ref(
                                   ctx->grammar, nc->node, a->name_ref);
            label->then_body = n00b_tree_resolve_child_ref(
                                   ctx->grammar, nc->node, a->type_ref);
            break;
        }

        case N00B_ANNOT_NOTRIVIA: {
            int32_t child_ix = a->notrivia_ref.index;

            if (child_ix >= 0
                && (size_t)child_ix < n00b_tree_num_children(nc->node)) {
                n00b_parse_tree_t *target
                    = n00b_tree_child(nc->node, child_ix);
                n00b_token_info_t *tok
                    = n00b_tree_find_first_terminal(target);

                if (tok && tok->leading_trivia) {
                    fprintf(stderr,
                            "warning: whitespace before modifier tick "
                            "at line %u col %u\n",
                            tok->line, tok->column);
                }
            }

            break;
        }

        default:
            break;
        }
    }
}
