/**
 * @file xform_typestr.c
 * @brief Transforms `typestr(type)` into a string literal containing encoded type ID.
 */

#include <ctype.h>
#include "base_alloc_shim.h"

#include "branch_symbols.h"
#include "transform.h"
#include "rewrite.h"
#include "types.h"
#include "token.h"
#include "nt_types.h"
#include "assert.h"

// Transform:
// typestr(type_name|string [, typename|string_literal]*) -> "encoded_type_id"

static void
extract_atom(xform_ctx_t *ctx, tnode_t *node, list_t **l)
{
    char *text;

    if (!node) {
        return;
    }

    assert(node->nt_id == NT_typeid_atom);
    node = tnode_get_kid(node, 0);
    if (node->nt_id == NT_typeof_specifier_argument) {
        text = normalize_type_node(node, ctx->input);
    }
    else {
        // string_literal node — may span multiple adjacent tokens
        int num_toks = node->num_toks;
        while (node && !node->tptr) {
            node = tnode_get_kid(node, 0);
        }
        assert(node && node->tptr);
        if (num_toks < 1) {
            num_toks = 1;
        }

        list_t *parts   = list_alloc(0);
        tok_t  *cur_tok = node->tptr;
        int     found   = 0;
        while (found < num_toks) {
            if (cur_tok->type == TT_STR) {
                char *part = extract(ctx->input, cur_tok);
                int   slen = strlen(part);
                if (slen >= 2 && part[0] == '"' && part[slen - 1] == '"') {
                    memmove(part, part + 1, slen - 2);
                    part[slen - 2] = '\0';
                }
                parts = list_append(parts, part);
                found++;
            }
            cur_tok++;
        }
        text = join(parts, "");
        base_dealloc(parts);
    }
    *l = list_append(*l, text);
}

static tnode_t *
xform_typestr(xform_ctx_t *ctx, tnode_t *node)
{
    if (node->branch != BRANCH(synthetic_string_literal, TYPESTR)) {
        return nullptr;
    }
    // kids: [0]=typestr [1]=( [2]=typeid_atom [3]=opt(continuation) [4]=)
    list_t *flattened = list_alloc(0);

    extract_atom(ctx, tnode_get_kid(node, 2), &flattened);

    tnode_t *next = tnode_get_kid(node, 3);

    while (next && next->nt_id == NT_typeid_continuation) {
        extract_atom(ctx, tnode_get_kid(next, 1), &flattened);
        next = tnode_get_kid(next, 2);
    }

    char *type_id = join(flattened, "");

    // Wrap in quotes to make it a string literal token
    char *quoted = base_alloc(strlen(type_id) + 3);
    sprintf(quoted, "\"%s\"", type_id);
    base_dealloc(type_id);

    tnode_t *str_node = synth_terminal(quoted, TT_STR, get_node_line(node));
    base_dealloc(quoted);

    return replace_node(node, str_node, "typestr");
}

/**
 * @brief Register the typestr transformation.
 *
 * @param reg Registry to add the transformation to
 */
void
register_typestr_xform(xform_registry_t *reg)
{
    xform_register_post(reg, NT_synthetic_string_literal, xform_typestr, "typestr");
}
