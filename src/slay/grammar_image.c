// grammar_image.c - Build-time grammar baking + runtime reconstruction.
//
// WP-018: pre-compile a parsed `n00b_grammar_t` into emitted C source
// that reconstructs an identical grammar at runtime, skipping the BNF
// metagrammar parse (~1.5s on c_ncc.bnf). See grammar_image.h for the
// design rationale (DF-EB: no LR0; DF-EC: first-sets recomputed by
// finalize, not baked; DF-ED: reconstructed on the GC heap exactly like
// a runtime-parsed grammar).
//
// The emitter intentionally uses plain `"..."` C string literals (each
// wrapped with `n00b_string_from_cstr`) for its code templates rather
// than `r"..."` rich literals: rich literals process `\` as markup at
// ncc-compile time, which would corrupt the backslash/quote/newline
// bytes the emitted C source must contain verbatim
// (n00b-api-guidelines § 2.11 — plain format strings, not raw).

#include <stdlib.h>

#include "slay/grammar_image.h"
#include "internal/slay/grammar_internal.h"
#include "core/string.h"
#include "text/strings/string_ops.h"
#include "text/strings/format.h"
#include "util/assert.h"

// ============================================================================
// Reconstruction primitives
// ============================================================================

n00b_grammar_t *
n00b_grammar_image_begin(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    // The baked image already contains every rule the source grammar's
    // finalize produced (including error-recovery rules). Disable
    // automatic error-rule generation so the finalize at the end of
    // reconstruction does not inject a second copy and shift rule ids.
    n00b_grammar_set_error_recovery(g, false);

    return g;
}

void
n00b_grammar_image_add_nt(n00b_grammar_t *g,
                          int64_t         expected_id,
                          n00b_string_t  *name,
                          bool            group_nt,
                          bool            start_nt)
{
    // Anonymous NTs are stored with a null name; the emitter passes the
    // empty string to mean "anonymous".
    n00b_string_t *use_name = (name && name->u8_bytes) ? name : nullptr;

    n00b_nonterm_t *nt = n00b_nonterm(g, use_name);

    n00b_require(nt->id == expected_id,
                 "grammar image: non-terminal id mismatch on rebuild");

    nt->group_nt = group_nt;

    if (start_nt) {
        n00b_grammar_set_start_id(g, nt->id);
    }
}

void
n00b_grammar_image_add_terminal(n00b_grammar_t *g, n00b_string_t *name)
{
    n00b_register_terminal(g, name);
}

void
n00b_grammar_image_add_literal_type(n00b_grammar_t *g, n00b_string_t *name)
{
    n00b_register_literal_type(g, name);
}

void
n00b_grammar_image_add_terminal_category(n00b_grammar_t *g,
                                         int64_t         terminal_id,
                                         n00b_string_t  *category)
{
    n00b_grammar_set_terminal_category(g, terminal_id, category);
}

void
n00b_grammar_image_add_rule(n00b_grammar_t *g,
                            n00b_nt_id_t    nt_id,
                            int32_t         cost,
                            bool            penalty,
                            int32_t         link_ix,
                            int             n,
                            n00b_match_t   *items)
{
    n00b_nonterm_t *nt = n00b_get_nonterm(g, nt_id);
    n00b_require(nt != nullptr,
                 "grammar image: rule references unknown non-terminal");

    n00b_list_t(n00b_match_t) contents = n00b_list_new_private(n00b_match_t);
    for (int i = 0; i < n; i++) {
        n00b_list_push(contents, items[i]);
    }

    n00b_parse_rule_t rule = {0};
    rule.nt_id        = nt_id;
    rule.contents     = contents;
    rule.cost         = cost;
    rule.penalty_rule = penalty;
    rule.link_ix      = penalty ? link_ix : -1;

    n00b_list_push(g->rules, rule);

    int32_t ix = (int32_t)(g->rules.len - 1);
    n00b_list_push(nt->rule_ids, ix);
}

n00b_match_t
n00b_grammar_image_group(int32_t min, int32_t max, int32_t gid,
                         int64_t contents_id)
{
    n00b_rule_group_t *group = n00b_alloc(n00b_rule_group_t);

    group->gid         = gid;
    group->min         = min;
    group->max         = max;
    group->contents_id = contents_id;

    return (n00b_match_t){.kind = N00B_MATCH_GROUP, .group = group};
}

void
n00b_grammar_image_finish(n00b_grammar_t *g,
                          n00b_string_t  *tokenizer_name,
                          uint32_t        max_penalty)
{
    if (tokenizer_name && tokenizer_name->u8_bytes) {
        g->tokenizer_name = tokenizer_name;
    }

    g->max_penalty = max_penalty;

    // A baked grammar is captured WITHOUT first-set / left-corner / LR0
    // analysis (it was finalized under N00B_SLAY_SKIP_FINALIZE_ANALYSIS
    // at bake time, and PWZ — the only consumer — does not need that
    // derived state). Force the same gate here so reconstruction stays
    // fast and structurally identical regardless of the consumer's
    // environment, then finalize to rebuild nullability + valid_tokens.
    setenv("N00B_SLAY_SKIP_FINALIZE_ANALYSIS", "1", 1);

    n00b_grammar_finalize(g);
}

// ============================================================================
// Emitter
// ============================================================================
//
// The emitted source for c_ncc.bnf is ~760 KB across ~19 000 lines, so
// the emitter accumulates pieces into a list and joins once at the end
// rather than repeatedly `n00b_unicode_str_cat`-ing onto a growing
// string (which would be O(n²) — copying the whole buffer per append).

static inline void
emit(n00b_list_t(n00b_string_t *) *parts, const char *s)
{
    n00b_list_push(*parts, n00b_string_from_cstr(s));
}

static inline void
emit_str(n00b_list_t(n00b_string_t *) *parts, n00b_string_t *s)
{
    n00b_list_push(*parts, s);
}

// Build a quoted C string literal (with surrounding double quotes) for
// the bytes of `s` (or "" for null), escaping whatever the C lexer cares
// about so the result is valid regardless of the name's contents.
static n00b_string_t *
c_quoted(n00b_string_t *s)
{
    const char *data  = (s && s->data) ? s->data : "";
    int64_t     bytes = (s && s->data) ? s->u8_bytes : 0;

    // Worst case is 4 output bytes per input byte (\\ooo), plus the two
    // surrounding quotes and a terminator.
    char  *buf = n00b_alloc_array(char, (size_t)bytes * 4 + 3);
    size_t k   = 0;

    buf[k++] = '"';
    for (int64_t i = 0; i < bytes; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
        case '\\': buf[k++] = '\\'; buf[k++] = '\\'; break;
        case '"':  buf[k++] = '\\'; buf[k++] = '"';  break;
        case '\n': buf[k++] = '\\'; buf[k++] = 'n';  break;
        case '\r': buf[k++] = '\\'; buf[k++] = 'r';  break;
        case '\t': buf[k++] = '\\'; buf[k++] = 't';  break;
        default:
            if (c >= 0x20 && c < 0x7f) {
                buf[k++] = (char)c;
            }
            else {
                buf[k++] = '\\';
                buf[k++] = (char)('0' + ((c >> 6) & 0x7));
                buf[k++] = (char)('0' + ((c >> 3) & 0x7));
                buf[k++] = (char)('0' + (c & 0x7));
            }
            break;
        }
    }
    buf[k++] = '"';
    buf[k]   = '\0';

    return n00b_string_from_raw(buf, (int64_t)k);
}

// Push an `n00b_string_t *`-producing expression for `s`:
// `n00b_string_from_cstr("...")`. The reconstruction primitives all take
// `n00b_string_t *`, so a bare C string literal would be a type error
// (and a hash-of-garbage crash at runtime); the wrapper materializes a
// real n00b string at reconstruction time.
static void
emit_n00b_string(n00b_list_t(n00b_string_t *) *parts, n00b_string_t *s)
{
    n00b_list_push(*parts, n00b_string_from_cstr("n00b_string_from_cstr("));
    n00b_list_push(*parts, c_quoted(s));
    n00b_list_push(*parts, n00b_string_from_cstr(")"));
}

// Push a bare quoted C string literal (a `const char *`), used where the
// callee wants a plain C string — notably `n00b_static_grammar_register`,
// which runs in a `[[gnu::constructor]]` BEFORE the n00b runtime is up
// and therefore must NOT call `n00b_string_from_cstr`.
static void
emit_c_quoted(n00b_list_t(n00b_string_t *) *parts, n00b_string_t *s)
{
    n00b_list_push(*parts, c_quoted(s));
}

// Push a single match item as a C compound-literal `n00b_match_t`.
static void
emit_match_item(n00b_list_t(n00b_string_t *) *parts, n00b_match_t *m)
{
    switch (m->kind) {
    case N00B_MATCH_EMPTY:
        emit(parts, "(n00b_match_t){.kind = N00B_MATCH_EMPTY}");
        return;
    case N00B_MATCH_NT:
        emit_str(parts,
                 n00b_cformat("(n00b_match_t){.kind = N00B_MATCH_NT, "
                              ".nt_id = «#»}",
                              (int64_t)m->nt_id));
        return;
    case N00B_MATCH_TERMINAL:
        emit_str(parts,
                 n00b_cformat("(n00b_match_t){.kind = N00B_MATCH_TERMINAL, "
                              ".terminal_id = «#»LL}",
                              (int64_t)m->terminal_id));
        return;
    case N00B_MATCH_ANY:
        emit(parts, "(n00b_match_t){.kind = N00B_MATCH_ANY}");
        return;
    case N00B_MATCH_CLASS:
        emit_str(parts,
                 n00b_cformat("(n00b_match_t){.kind = N00B_MATCH_CLASS, "
                              ".char_class = «#»}",
                              (int64_t)m->char_class));
        return;
    case N00B_MATCH_SET:
        // PWZ treats N00B_MATCH_SET as a no-op (it never reads
        // set_items); the BNF loader does not emit it for c_ncc.bnf.
        // Bake it as an EMPTY item to preserve item count without
        // attempting to serialize the unused set_items pointer graph.
        emit(parts, "(n00b_match_t){.kind = N00B_MATCH_EMPTY}");
        return;
    case N00B_MATCH_GROUP: {
        n00b_rule_group_t *grp = (n00b_rule_group_t *)m->group;
        emit_str(parts,
                 n00b_cformat("n00b_grammar_image_group(«#», «#», «#», «#»)",
                              (int64_t)grp->min, (int64_t)grp->max,
                              (int64_t)grp->gid, (int64_t)grp->contents_id));
        return;
    }
    }

    emit(parts, "(n00b_match_t){.kind = N00B_MATCH_EMPTY}");
}

n00b_string_t *
n00b_grammar_image_emit(n00b_grammar_t *g,
                        n00b_string_t  *symbol_prefix,
                        n00b_string_t  *grammar_name)
{
    if (!g || !g->finalized || !symbol_prefix || !grammar_name) {
        return nullptr;
    }

    n00b_list_t(n00b_string_t *) parts = n00b_list_new_private(n00b_string_t *);

    emit(&parts,
         "/* Generated by n00b_grammar_image_emit (WP-018). "
         "Do not edit. */\n");
    emit(&parts, "#include \"n00b.h\"\n");
    emit(&parts, "#include \"slay/grammar_image.h\"\n");
    emit(&parts, "#include \"core/static_image.h\"\n\n");

    emit_str(&parts,
             n00b_cformat("static n00b_grammar_t *\n«#»_build(void)\n{\n",
                          symbol_prefix));
    emit(&parts, "    n00b_grammar_t *g = n00b_grammar_image_begin();\n\n");

    // --- Non-terminals (ascending id order; nt_list is already in id
    //     order because n00b_nonterm assigns id == nt_list.len). ---
    size_t nnt = g->nt_list.len;
    for (size_t i = 0; i < nnt; i++) {
        n00b_nonterm_t *nt = &g->nt_list.data[i];

        emit_str(&parts,
                 n00b_cformat("    n00b_grammar_image_add_nt(g, «#», ",
                              (int64_t)nt->id));
        emit_n00b_string(&parts, nt->name);
        emit_str(&parts,
                 n00b_cformat(", «#», «#»);\n",
                              n00b_string_from_cstr(nt->group_nt ? "true"
                                                                 : "false"),
                              n00b_string_from_cstr(nt->start_nt ? "true"
                                                                 : "false")));
    }
    emit(&parts, "\n");

    // --- Terminals (hash-derived ids; order-independent). ---
    n00b_dict_foreach(g->terminal_map, tname, tid, {
        (void)tid;
        emit(&parts, "    n00b_grammar_image_add_terminal(g, ");
        emit_n00b_string(&parts, tname);
        emit(&parts, ");\n");
    });
    emit(&parts, "\n");

    // --- Literal types (dense ids: must be replayed in id order). ---
    if (g->next_literal_type_id > 0) {
        n00b_list_t(n00b_string_t *) lit_by_id
            = n00b_list_new_private(n00b_string_t *);
        for (int64_t i = 0; i < g->next_literal_type_id; i++) {
            n00b_list_push(lit_by_id, n00b_string_empty());
        }
        n00b_dict_foreach(g->literal_type_map, lname, lid, {
            if (lid >= 0 && lid < g->next_literal_type_id) {
                lit_by_id.data[lid] = lname;
            }
        });
        for (int64_t i = 0; i < g->next_literal_type_id; i++) {
            emit(&parts, "    n00b_grammar_image_add_literal_type(g, ");
            emit_n00b_string(&parts, n00b_list_get(lit_by_id, i));
            emit(&parts, ");\n");
        }
        emit(&parts, "\n");
    }

    // --- Terminal categories (keyed by id; order-independent). ---
    if (g->has_terminal_categories && g->terminal_categories) {
        n00b_dict_foreach(g->terminal_categories, cat_id, cat_name, {
            emit_str(&parts,
                     n00b_cformat(
                         "    n00b_grammar_image_add_terminal_category(g, «#»LL, ",
                         (int64_t)cat_id));
            emit_n00b_string(&parts, cat_name);
            emit(&parts, ");\n");
        });
        emit(&parts, "\n");
    }

    // --- Rules (global order). ---
    size_t nrules = g->rules.len;
    for (size_t i = 0; i < nrules; i++) {
        n00b_parse_rule_t *r      = &g->rules.data[i];
        size_t             nitems = r->contents.len;

        // add_rule_internal forbids empty productions, so the source
        // grammar should never contain one; skip defensively.
        if (nitems == 0) {
            continue;
        }

        emit(&parts, "    {\n        n00b_match_t items[] = {\n");
        for (size_t j = 0; j < nitems; j++) {
            emit(&parts, "            ");
            emit_match_item(&parts, &r->contents.data[j]);
            emit(&parts, ",\n");
        }
        emit(&parts, "        };\n");
        emit_str(&parts,
                 n00b_cformat("        n00b_grammar_image_add_rule(g, «#», «#», "
                              "«#», «#», «#», items);\n    }\n",
                              (int64_t)r->nt_id, (int64_t)r->cost,
                              n00b_string_from_cstr(r->penalty_rule ? "true"
                                                                    : "false"),
                              (int64_t)r->link_ix, (int64_t)nitems));
    }
    emit(&parts, "\n");

    // --- Finalize. ---
    emit(&parts, "    n00b_grammar_image_finish(g, ");
    emit_n00b_string(&parts, g->tokenizer_name);
    emit_str(&parts,
             n00b_cformat(", «#»u);\n    return g;\n}\n\n",
                          (int64_t)g->max_penalty));

    // --- Registration constructor. ---
    emit_str(&parts,
             n00b_cformat("[[gnu::constructor]]\nstatic void\n"
                          "«#»_register(void)\n"
                          "{\n    n00b_static_grammar_register(",
                          symbol_prefix));
    emit_c_quoted(&parts, grammar_name);
    emit_str(&parts,
             n00b_cformat(", «#»_build);\n}\n", symbol_prefix));

    return n00b_unicode_str_join(n00b_string_empty(),
                                 n00b_list_to_array(n00b_string_t *, parts));
}
