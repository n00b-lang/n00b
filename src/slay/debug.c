// debug.c — Human-readable debug output for grammars, parse trees,
// Earley state tables, and LR(0) tables.

#include "slay/debug.h"
#include "internal/slay/grammar_internal.h"
#include "internal/slay/earley_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Character class names
// ============================================================================

static const char *
cc_name(n00b_char_class_t cc)
{
    switch (cc) {
    case N00B_CC_ID_START:        return "IdStart";
    case N00B_CC_ID_CONTINUE:     return "IdContinue";
    case N00B_CC_ASCII_DIGIT:     return "Digit";
    case N00B_CC_UNICODE_DIGIT:   return "UDigit";
    case N00B_CC_ASCII_UPPER:     return "Upper";
    case N00B_CC_ASCII_LOWER:     return "Lower";
    case N00B_CC_ASCII_ALPHA:     return "Alpha";
    case N00B_CC_WHITESPACE:      return "WS";
    case N00B_CC_HEX_DIGIT:       return "Hex";
    case N00B_CC_NONZERO_DIGIT:   return "NZDigit";
    case N00B_CC_PRINTABLE:       return "Print";
    case N00B_CC_NON_WS_PRINTABLE:return "NonWsPrint";
    case N00B_CC_NON_NL_WS:       return "NonNlWs";
    case N00B_CC_NON_NL_PRINTABLE:return "NonNlPrint";
    case N00B_CC_JSON_STRING_CHAR:return "JsonStr";
    case N00B_CC_REGEX_BODY_CHAR: return "RegexStr";
    }

    return "?class?";
}

// ============================================================================
// Operation names
// ============================================================================

static const char *
op_name(n00b_earley_op_t op)
{
    switch (op) {
    case N00B_EO_PREDICT_NT:      return "Predict(N)";
    case N00B_EO_PREDICT_G:       return "Predict(G)";
    case N00B_EO_FIRST_GROUP_ITEM:return "GroupStart";
    case N00B_EO_SCAN_TOKEN:      return "Scan(T)";
    case N00B_EO_SCAN_ANY:        return "Scan(*)";
    case N00B_EO_SCAN_NULL:       return "Scan(e)";
    case N00B_EO_SCAN_CLASS:      return "Scan(C)";
    case N00B_EO_SCAN_SET:        return "Scan(S)";
    case N00B_EO_COMPLETE_N:      return "Complete";
    case N00B_EO_ITEM_END:        return "ItemEnd";
    }

    return "?op?";
}

// ============================================================================
// Subtree info symbols
// ============================================================================

static const char *
si_name(n00b_subtree_info_t si)
{
    switch (si) {
    case N00B_SI_NONE:             return "";
    case N00B_SI_NT_RULE_START:    return "T";
    case N00B_SI_NT_RULE_END:      return "B";
    case N00B_SI_GROUP_START:      return "gT";
    case N00B_SI_GROUP_END:        return "gB";
    case N00B_SI_GROUP_ITEM_START: return "iT";
    case N00B_SI_GROUP_ITEM_END:   return "iB";
    }

    return "";
}

// ============================================================================
// Format a terminal ID to a FILE
// ============================================================================

static void
fprint_terminal(FILE *out, n00b_grammar_t *g, int64_t id)
{
    if (n00b_token_id_is_fixed_text(id)) {
        // Hash-based fixed-text terminal.
        n00b_terminal_t *t = n00b_get_terminal(g, id);

        if (t && t->value.data) {
            fprintf(out, "'%.*s'",
                    (int)t->value.u8_bytes, t->value.data);
        }
        else {
            fprintf(out, "tok#%lld", (long long)id);
        }

        return;
    }

    if (id >= 0 && id < 0x7f) {
        // Small non-negative: could be a literal type ID or a codepoint.
        // Try grammar terminal lookup first.
        n00b_terminal_t *t = n00b_get_terminal(g, id);

        if (t && t->value.data) {
            fprintf(out, "'%.*s'",
                    (int)t->value.u8_bytes, t->value.data);
            return;
        }

        // Printable codepoint fallback.
        if (id >= 0x20) {
            fprintf(out, "'%c'", (char)id);
            return;
        }
    }

    fprintf(out, "tok#%lld", (long long)id);
}

// ============================================================================
// Format a match item to a FILE
// ============================================================================

static void
fprint_match(FILE *out, n00b_grammar_t *g, n00b_match_t *m)
{
    switch (m->kind) {
    case N00B_MATCH_EMPTY:
        fprintf(out, "e");
        break;

    case N00B_MATCH_NT: {
        n00b_nonterm_t *nt = n00b_get_nonterm(g, m->nt_id);

        if (nt && nt->name.data) {
            fprintf(out, "%.*s", (int)nt->name.u8_bytes, nt->name.data);
        }
        else {
            fprintf(out, "?");
        }

        break;
    }

    case N00B_MATCH_TERMINAL:
        fprint_terminal(out, g, m->terminal_id);
        break;

    case N00B_MATCH_ANY:
        fprintf(out, "<Any>");
        break;

    case N00B_MATCH_CLASS:
        fprintf(out, "<%s>", cc_name(m->char_class));
        break;

    case N00B_MATCH_SET:
        fprintf(out, "<Set>");
        break;

    case N00B_MATCH_GROUP: {
        n00b_rule_group_t *grp = (n00b_rule_group_t *)m->group;

        if (!grp) {
            fprintf(out, "<group?>");
            break;
        }

        fprintf(out, "(");

        if (grp->contents_id >= 0) {
            n00b_nonterm_t *gnt = n00b_get_nonterm(g, grp->contents_id);

            if (gnt && n00b_list_len(gnt->rule_ids) > 0) {
                int32_t            rix = n00b_list_get(gnt->rule_ids, 0);
                n00b_parse_rule_t *r   = n00b_get_rule(g, rix);

                if (r) {
                    size_t len = n00b_list_len(r->contents);

                    for (size_t i = 0; i < len; i++) {
                        if (i) {
                            fprintf(out, " ");
                        }

                        n00b_match_t *mi = &r->contents.data[i];
                        fprint_match(out, g, mi);
                    }
                }
            }
        }

        if (grp->min == 0 && grp->max == 1) {
            fprintf(out, ")?");
        }
        else if (grp->min == 0 && grp->max == 0) {
            fprintf(out, ")*");
        }
        else if (grp->min == 1 && grp->max == 0) {
            fprintf(out, ")+");
        }
        else {
            fprintf(out, ")[%d,%d]", grp->min, grp->max);
        }

        break;
    }
    }
}

// ============================================================================
// Format a rule RHS with optional dot position
// ============================================================================

static void
fprint_rule_rhs(FILE *out, n00b_grammar_t *g, n00b_parse_rule_t *r, int dot_pos)
{
    size_t n = n00b_list_len(r->contents);

    for (size_t i = 0; i <= n; i++) {
        if ((int)i == dot_pos) {
            fprintf(out, ". ");
        }

        if (i < n) {
            fprint_match(out, g, &r->contents.data[i]);

            if (i + 1 < n || (int)(i + 1) == dot_pos) {
                fprintf(out, " ");
            }
        }
    }
}

// ============================================================================
// Public API: n00b_grammar_print
// ============================================================================

void
n00b_grammar_print(n00b_grammar_t *g, FILE *out)
{
    if (!g) {
        fprintf(out, "(null grammar)\n");
        return;
    }

    size_t nr = n00b_list_len(g->rules);

    fprintf(out, "Grammar: %zu rules, %zu non-terminals\n",
            nr, n00b_list_len(g->nt_list));
    fprintf(out, "----------------------------------------\n");

    for (size_t i = 0; i < nr; i++) {
        n00b_parse_rule_t *r = &g->rules.data[i];

        if (n00b_hide_penalties(g) && r->penalty_rule) {
            continue;
        }

        n00b_nonterm_t *nt = n00b_get_nonterm(g, r->nt_id);

        if (n00b_hide_groups(g) && nt && nt->group_nt) {
            continue;
        }

        fprintf(out, "  [%3zu] ", i);

        if (nt && nt->name.data) {
            fprintf(out, "%-20.*s -> ",
                    (int)nt->name.u8_bytes, nt->name.data);
        }
        else {
            fprintf(out, "%-20s -> ", "?");
        }

        fprint_rule_rhs(out, g, r, -1);

        if (r->cost) {
            fprintf(out, "  (cost %d)", r->cost);
        }

        if (r->penalty_rule) {
            fprintf(out, "  (error)");
        }

        fprintf(out, "\n");
    }

    fprintf(out, "----------------------------------------\n");
}

// ============================================================================
// Public API: n00b_parser_print_states
// ============================================================================

void
n00b_parser_print_states(n00b_earley_parser_t *p, FILE *out, bool show_all)
{
    if (!p) {
        fprintf(out, "(null parser)\n");
        return;
    }

    size_t ns = n00b_list_len(p->states);
    fprintf(out, "Parser: %zu states\n", ns);

    for (size_t si = 0; si < ns; si++) {
        n00b_earley_state_t *s = p->states.data[si];

        fprintf(out, "\n=== State %d", s->id);

        if (s->token) {
            if (s->token->tid == N00B_TOK_EOF) {
                fprintf(out, " [EOF]");
            }
            else if (n00b_option_is_set(s->token->value)) {
                n00b_string_t v = n00b_option_get(s->token->value);
                fprintf(out, " [%.*s]", (int)v.u8_bytes, v.data);
            }
            else {
                fprintf(out, " [tid=%lld]", (long long)s->token->tid);
            }
        }

        fprintf(out, " ===\n");

        size_t ni = n00b_list_len(s->items);

        for (size_t ii = 0; ii < ni; ii++) {
            n00b_earley_item_t *ei    = s->items.data[ii];
            n00b_earley_item_t *start = ei->start_item;

            if (!start) {
                start = ei;
            }

            if (!show_all
                && ei->cursor
                       != (int32_t)n00b_list_len(start->rule->contents)) {
                continue;
            }

            fprintf(out, "  [%zu] ", ii);

            n00b_nonterm_t *nt = n00b_get_nonterm(
                p->grammar, start->ruleset_id);

            if (nt && nt->name.data) {
                fprintf(out, "%.*s -> ",
                        (int)nt->name.u8_bytes, nt->name.data);
            }
            else {
                fprintf(out, "?%d -> ", start->ruleset_id);
            }

            fprint_rule_rhs(out, p->grammar, start->rule, ei->cursor);

            fprintf(out, " (s%d)", start->estate_id);

            if (ei->penalty) {
                fprintf(out, " pen=%u", ei->penalty);
            }

            if (ei->cost) {
                fprintf(out, " cost=%u", ei->cost);
            }

            const char *opn = op_name(ei->op);
            const char *sin = si_name(ei->subtree_info);

            if (*opn) {
                fprintf(out, " [%s", opn);

                if (*sin) {
                    fprintf(out, ",%s", sin);
                }

                fprintf(out, "]");
            }
            else if (*sin) {
                fprintf(out, " [%s]", sin);
            }

            fprintf(out, "\n");
        }
    }
}

// ============================================================================
// Tree printing (recursive)
// ============================================================================

static void
tree_print_recursive(n00b_parse_tree_t *t, n00b_grammar_t *g, FILE *out,
                     int depth)
{
    if (!t) {
        return;
    }

    for (int i = 0; i < depth; i++) {
        fprintf(out, "  ");
    }

    if (n00b_tree_is_leaf(t)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(t);

        if (tok) {
            if (n00b_option_is_set(tok->value)) {
                n00b_string_t v = n00b_option_get(tok->value);
                fprintf(out, "'%.*s'", (int)v.u8_bytes, v.data);
            }
            else {
                fprintf(out, "tok(%lld)", (long long)tok->tid);
            }

            fprintf(out, " [%u:%u-%u]", tok->line, tok->column, tok->endcol);
        }
        else {
            fprintf(out, "(null token)");
        }

        fprintf(out, "\n");
        return;
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(t);

    if (pn->id == N00B_EMPTY_STRING) {
        fprintf(out, "e [%d-%d]\n", pn->start, pn->end);
        return;
    }

    if (pn->name.data) {
        fprintf(out, "%.*s", (int)pn->name.u8_bytes, pn->name.data);
    }
    else {
        // Try grammar lookup.
        n00b_nonterm_t *nt = g ? n00b_get_nonterm(g, pn->id) : NULL;

        if (nt && nt->name.data) {
            fprintf(out, "%.*s", (int)nt->name.u8_bytes, nt->name.data);
        }
        else {
            fprintf(out, "?");
        }
    }

    fprintf(out, " [%d-%d]", pn->start, pn->end);

    if (pn->penalty) {
        fprintf(out, " pen=%u", pn->penalty);
    }

    if (pn->cost) {
        fprintf(out, " cost=%u", pn->cost);
    }

    fprintf(out, "\n");

    size_t nc = n00b_tree_num_children(t);

    for (size_t i = 0; i < nc; i++) {
        tree_print_recursive(n00b_tree_child(t, i), g, out, depth + 1);
    }
}

// ============================================================================
// Public API: n00b_tree_print
// ============================================================================

void
n00b_tree_print(n00b_parse_tree_t *tree, n00b_grammar_t *g, FILE *out)
{
    if (!tree) {
        fprintf(out, "(null tree)\n");
        return;
    }

    tree_print_recursive(tree, g, out, 0);
}

// ============================================================================
// Public API: n00b_forest_print
// ============================================================================

void
n00b_forest_print(n00b_parse_forest_t *forest, n00b_grammar_t *g, FILE *out)
{
    if (!forest) {
        fprintf(out, "No valid parses.\n");
        return;
    }

    int32_t count = n00b_parse_forest_count(forest);

    if (count == 0) {
        fprintf(out, "No valid parses.\n");
        return;
    }

    if (count == 1) {
        n00b_tree_print(n00b_parse_forest_tree(forest, 0), g, out);
        return;
    }

    fprintf(out, "%d parse trees:\n", count);

    for (int32_t i = 0; i < count; i++) {
        fprintf(out, "\n--- Parse %d ---\n", i + 1);
        n00b_tree_print(n00b_parse_forest_tree(forest, i), g, out);
    }
}

// ============================================================================
// Public API: n00b_parse_node_repr
// ============================================================================

n00b_string_t
n00b_parse_node_repr(n00b_parse_tree_t *node)
{
    if (!node) {
        return n00b_string_from_cstr("(null)");
    }

    char buf[256];

    if (n00b_tree_is_leaf(node)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);

        if (tok && n00b_option_is_set(tok->value)) {
            n00b_string_t v = n00b_option_get(tok->value);
            snprintf(buf, sizeof(buf), "'%.*s' (%u:%u-%u) tid=%lld",
                     (int)v.u8_bytes, v.data,
                     tok->line, tok->column, tok->endcol,
                     (long long)tok->tid);
        }
        else if (tok) {
            snprintf(buf, sizeof(buf), "token (%u:%u-%u) tid=%lld",
                     tok->line, tok->column, tok->endcol,
                     (long long)tok->tid);
        }
        else {
            snprintf(buf, sizeof(buf), "(null token)");
        }
    }
    else {
        n00b_nt_node_t *pn = &n00b_tree_node_value(node);

        if (pn->id == N00B_EMPTY_STRING) {
            snprintf(buf, sizeof(buf), "e (%d-%d)", pn->start, pn->end);
        }
        else if (pn->name.data) {
            snprintf(buf, sizeof(buf), "%.*s (%d-%d) id#%lld",
                     (int)pn->name.u8_bytes, pn->name.data,
                     pn->start, pn->end,
                     (long long)pn->id);
        }
        else {
            snprintf(buf, sizeof(buf), "? (%d-%d) id#%lld",
                     pn->start, pn->end,
                     (long long)pn->id);
        }

        if (pn->penalty) {
            size_t len = strlen(buf);
            snprintf(buf + len, sizeof(buf) - len, " pen=%u", pn->penalty);
        }

        if (pn->cost) {
            size_t len = strlen(buf);
            snprintf(buf + len, sizeof(buf) - len, " cost=%u", pn->cost);
        }
    }

    return n00b_string_from_cstr(buf);
}

// ============================================================================
// Public API: n00b_lr0_print
// ============================================================================

void
n00b_lr0_print(n00b_grammar_t *g, FILE *out)
{
    if (!g || !g->lr0_states) {
        fprintf(out, "(no LR(0) table)\n");
        return;
    }

    fprintf(out, "LR(0) table: %d states, %d items, %d gotos\n",
            g->lr0_state_count, g->lr0_item_count, g->lr0_goto_count);
    fprintf(out, "Start state: %d\n", g->lr0_start_state);
    fprintf(out, "----------------------------------------\n");

    for (int32_t si = 0; si < g->lr0_state_count; si++) {
        n00b_lr0_state_t *st = &g->lr0_states[si];

        fprintf(out, "\nState %d (%d items, %d gotos):\n",
                si, st->items_count, st->gotos_count);

        for (int32_t j = 0; j < st->items_count; j++) {
            int32_t            item_id = g->lr0_state_items[st->items_start + j];
            n00b_lr0_item_t   *item    = &g->lr0_items[item_id];
            n00b_parse_rule_t *rule    = n00b_get_rule(g, item->rule_ix);

            if (!rule) {
                fprintf(out, "  (bad rule ix %d)\n", item->rule_ix);
                continue;
            }

            n00b_nonterm_t *nt = n00b_get_nonterm(g, rule->nt_id);

            fprintf(out, "  ");

            if (nt && nt->name.data) {
                fprintf(out, "%.*s -> ",
                        (int)nt->name.u8_bytes, nt->name.data);
            }
            else {
                fprintf(out, "? -> ");
            }

            fprint_rule_rhs(out, g, rule, item->dot);
            fprintf(out, "\n");
        }

        for (int32_t j = 0; j < st->gotos_count; j++) {
            n00b_lr0_goto_t *gt  = &g->lr0_gotos[st->gotos_start + j];
            int64_t          sym = gt->symbol;

            fprintf(out, "  GOTO(");

            if (n00b_token_id_is_fixed_text(sym)) {
                // Hash-based terminal (bit 63 set).
                fprint_terminal(out, g, sym);
            }
            else if (sym >= 0 && (sym & (1LL << 62))) {
                // Small terminal with bit 62 flag.
                int64_t tid = sym & ~(1LL << 62);
                fprint_terminal(out, g, tid);
            }
            else if (sym >= 0) {
                // Non-terminal.
                n00b_nonterm_t *nt = n00b_get_nonterm(g, sym);

                if (nt && nt->name.data) {
                    fprintf(out, "%.*s",
                            (int)nt->name.u8_bytes, nt->name.data);
                }
                else {
                    fprintf(out, "?");
                }
            }
            else if (sym == INT64_MIN) {
                fprintf(out, "<Any>");
            }
            else if (sym == INT64_MIN + 1) {
                fprintf(out, "<Empty>");
            }
            else if (sym <= INT64_MIN + 1000 + 100) {
                fprintf(out, "class#%lld", (long long)(-(sym - INT64_MIN - 100)));
            }
            else if (sym <= INT64_MIN + 1000 + 100 + 1000) {
                fprintf(out, "group#%lld", (long long)(-(sym - INT64_MIN - 1000)));
            }
            else {
                fprintf(out, "sym#%lld", (long long)sym);
            }

            fprintf(out, ") = %d\n", gt->dest_state);
        }
    }

    fprintf(out, "----------------------------------------\n");

    // Print prediction states.
    if (g->lr0_predict_state) {
        size_t n_nts = n00b_list_len(g->nt_list);

        fprintf(out, "\nPrediction states:\n");

        for (size_t i = 0; i < n_nts; i++) {
            n00b_nonterm_t *nt = n00b_get_nonterm(g, (int64_t)i);

            if (g->lr0_predict_state[i] >= 0) {
                if (nt && nt->name.data) {
                    fprintf(out, "  %.*s -> state %d\n",
                            (int)nt->name.u8_bytes, nt->name.data,
                            g->lr0_predict_state[i]);
                }
                else {
                    fprintf(out, "  ? -> state %d\n",
                            g->lr0_predict_state[i]);
                }
            }
        }
    }
}
