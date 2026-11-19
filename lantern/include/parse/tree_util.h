#pragma once

#include "parse/types.h"
#include "parse/tree.h"
#include "parse/grammar.h"
#include "parse/annotation.h"

#include <stdatomic.h>

bool parse_alt_leads_with(parse_grammar_t *g, core_string_t nt_name,
                        int32_t rule_ix, core_string_t word);

typedef bool (*parse_leaf_fn)(parse_tree_t *leaf, void *arg);

parse_tree_t *parse_tree_scan_leaves(parse_tree_t *root, bool reverse,
                                       parse_leaf_fn fn, void *arg);

parse_token_info_t *parse_tree_find_first_terminal(parse_tree_t *node);

parse_tree_t *parse_tree_find_direct_child_by_nt_name(parse_grammar_t *g,
                                                      parse_tree_t *parent,
                                                      core_string_t name);
parse_tree_t *parse_tree_find_flat_child_by_nt_name(parse_grammar_t *g,
                                                    parse_tree_t *parent,
                                                    core_string_t name);

parse_tree_t *parse_tree_get_nth_nt_child(parse_tree_t *node, int32_t n);

parse_tree_t *parse_tree_resolve_child_ref(parse_grammar_t    *g,
                                             parse_tree_t *parent,
                                             parse_child_ref_t   ref);

parse_nonterm_t *parse_tree_nonterm(parse_grammar_t *g, parse_tree_t *n);
core_string_t parse_tree_nt_name(parse_grammar_t *g, parse_tree_t *n);
bool parse_tree_is_nt(parse_grammar_t *g, parse_tree_t *n, core_string_t name);

// A nonterminal name fixed at one call site, resolved to a grammar's id once
// rather than compared as a string at every check. `resolved` packs the
// grammar version (parse_grammar_version) above PARSE_NT_SITE_ID_BITS bits of
// id + 1; 0 means nothing is cached. Names are unique within a grammar, so a
// node is that nonterminal exactly when its id is the resolved one.
typedef struct {
    _Atomic uint64_t resolved;
} parse_nt_site_t;

#define PARSE_NT_SITE_ID_BITS 24

int64_t parse_nt_site_resolve(parse_grammar_t *g, parse_nt_site_t *site,
                              core_string_t name);

// The id `name` has in `g`, or -1 when `g` has no such nonterminal.
static inline int64_t
parse_nt_site_id(parse_grammar_t *g, parse_nt_site_t *site, core_string_t name)
{
    uint64_t resolved = atomic_load_explicit(&site->resolved,
                                             memory_order_relaxed);
    if (g && resolved
        && resolved >> PARSE_NT_SITE_ID_BITS == parse_grammar_version(g))
        return (int64_t)(resolved
                         & ((UINT64_C(1) << PARSE_NT_SITE_ID_BITS) - 1)) - 1;
    return parse_nt_site_resolve(g, site, name);
}

// The id of the nonterminal named by string literal `lit` in grammar `g`,
// cached at the expansion site.
#define PARSE_NT_ID(g, lit)                                                    \
    ({                                                                         \
        static_assert(sizeof(lit) > 1, "a nonterminal name is never empty");   \
        static parse_nt_site_t parse_nt_site_;                                 \
        parse_nt_site_id((g), &parse_nt_site_, CORE_STRING_STATIC(lit));       \
    })

// Whether `n` is a node of the nonterminal with id `id` (as
// parse_tree_nt_name would name it: group tops and leaves have no name).
static inline bool
parse_tree_has_nt_id(parse_tree_t *n, int64_t id)
{
    if (id < 0 || !n || core_tree_is_leaf(n)) return false;
    const parse_nt_node_t *pn = &core_tree_node_value(n);
    return !pn->group_top && pn->id == id;
}

#define PARSE_TREE_IS_NT(g, n, lit) parse_tree_has_nt_id((n), PARSE_NT_ID((g), lit))

parse_tree_t *parse_tree_find_direct_child_by_nt_id(parse_tree_t *parent,
                                                    int64_t id);
parse_tree_t *parse_tree_find_flat_child_by_nt_id(parse_tree_t *parent,
                                                  int64_t id);
const parse_rule_t *parse_tree_rule(parse_grammar_t *g, parse_tree_t *n);

core_string_t parse_tree_leaf_text(parse_tree_t *n);
int64_t      parse_tree_leaf_tid(parse_tree_t *n);
bool         parse_tree_leaf_is(parse_tree_t *n, core_string_t text);
