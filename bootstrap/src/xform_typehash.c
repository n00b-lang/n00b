/**
 * @file xform_typehash.c
 * @brief Transform typehash(type) into a uint64 numeric literal.
 *
 * Computes a SHA256 hash of the normalized type string and emits the
 * first 64 bits as an unsigned integer literal (ULL suffix).
 *
 *   typehash(int)       → 12345678901234ULL
 *   typehash(char *)    → 98765432109876ULL
 */

#include <ctype.h>
#include <inttypes.h>
#include "base_alloc_shim.h"

#include "branch_symbols.h"
#include "ncc_limits.h"
#include "transform.h"
#include "rewrite.h"
#include "types.h"
#include "token.h"
#include "nt_types.h"
#include "xform_helpers.h"
#include "assert.h"

// Atom extraction — identical to xform_typeid.c.
static void
extract_atom(tree_xform_t *ctx, tnode_t *node, ncc_list_t **l)
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
        int num_toks = node->num_toks;
        while (node && !node->tptr) {
            node = tnode_get_kid(node, 0);
        }
        assert(node && node->tptr);
        if (num_toks < 1) {
            num_toks = 1;
        }

        ncc_list_t *parts   = ncc_list_alloc(0);
        tok_t      *cur_tok = node->tptr;
        int         found   = 0;
        while (found < num_toks) {
            if (cur_tok->type == TT_STR) {
                char *part = extract(ctx->input, cur_tok);
                int   slen = strlen(part);
                if (slen >= 2 && part[0] == '"' && part[slen - 1] == '"') {
                    memmove(part, part + 1, slen - 2);
                    part[slen - 2] = '\0';
                }
                parts = ncc_list_append(parts, part);
                found++;
            }
            cur_tok++;
        }
        text = join(parts, "");
        base_dealloc(parts);

        for (char *p = text; *p; p++) {
            if (!isalnum((unsigned char)*p) && *p != '_') {
                ncc_error(
                    "%s:%d: typehash() string literal contains "
                    "invalid identifier character '%c'\n",
                    ctx->lex->in_file,
                    get_node_line(node),
                    *p);
                exit(1);
            }
        }
    }
    *l = ncc_list_append(*l, text);
}

static tnode_t *
xform_typehash(tree_xform_t *ctx, tnode_t *node)
{
    if (node->branch != BRANCH(primary_expression, TYPEHASH)) {
        return nullptr;
    }

    // kids: [0]=typehash [1]=( [2]=typeid_atom [3]=opt(continuation) [4]=)
    ncc_list_t *flattened = ncc_list_alloc(0);

    extract_atom(ctx, tnode_get_kid(node, 2), &flattened);

    tnode_t *next = tnode_get_kid(node, 3);

    while (next && next->nt_id == NT_typeid_continuation) {
        extract_atom(ctx, tnode_get_kid(next, 1), &flattened);
        next = tnode_get_kid(next, 2);
    }

    char    *type_str = join(flattened, "");
    uint64_t hash     = get_type_hash_u64(type_str);
    base_dealloc(type_str);

    int  line = get_node_line(node);
    char result_str[NCC_INTSTR_BUF];
    snprintf(result_str, sizeof(result_str), "%" PRIu64 "ULL", hash);

    tnode_t *replacement = build_numeric_literal(result_str, line);
    replace_node(node, replacement, "typehash");
    return replacement;
}

void
register_typehash_xform(xform_registry_t *reg)
{
    xform_register_post(reg,
                        NT_primary_expression,
                        xform_typehash,
                        "typehash");
}
