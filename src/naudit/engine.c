/*
 * WP-001 Phase 3 — audit engine core.
 *
 * Loads the vendored ncc C grammar (`c_ncc.bnf`) at the path baked
 * into `audit_paths.h` (N00B_AUDIT_GRAMMAR_PATH, emitted at configure
 * time by the top-level meson build — no path literals in C source),
 * merges every loaded rule's `bnf_fragment` into that grammar via a
 * single combined `n00b_bnf_load` call, then parses target source
 * files via slay's unified `n00b_parse` entry point. For every match
 * of any rule's `violation_nt` nonterminal in the resulting parse
 * tree the engine emits one `n00b_audit_violation_t`, carrying the
 * source location pulled from the matched node's leftmost token
 * leaf.
 *
 * ## Notes — DF-C and DF-D resolutions (Phase 3 settlement)
 *
 * The preflight's leading candidate for the NULL rule was attaching
 * at `<provided_identifier>` (c_ncc.bnf line 3). Reading the grammar
 * in full confirmed that:
 *
 *   - `nullptr` is a registered keyword in the C-ncc grammar
 *     (`<constant> ::= ... | %"nullptr"` at line 21). The tokenizer
 *     emits it as a keyword token, NOT through `%IDENTIFIER`, so
 *     `nullptr` will never appear under `<provided_identifier>`.
 *   - `NULL` is NOT a registered keyword anywhere in c_ncc.bnf, so
 *     the C tokenizer hashes its text and — finding no match in the
 *     grammar's keyword set — emits it as `%IDENTIFIER`, which
 *     flows up through `<provided_identifier>` (line 3) into
 *     `<identifier>` (line 447) and from there into expression
 *     positions via `<primary_expression> ::= ... | <identifier>`
 *     (line 571).
 *
 * **DF-C resolution.** The fragment registers `NULL` as a keyword
 * terminal (the `%"NULL"` token-literal causes `n00b_bnf_load` to
 * call `n00b_register_terminal(grammar, "NULL")`), then introduces a
 * new alternative on `<provided_identifier>` whose RHS is exactly
 * that keyword. The composed fragment is:
 *
 *     <n00b_audit_v_null> ::= %"NULL"
 *     <provided_identifier> ::= <n00b_audit_v_null>
 *
 * Effect: when the c_tokenizer sees `NULL`, it hashes the text and
 * finds the now-registered keyword id, emitting `NULL` as the
 * `%"NULL"` terminal rather than `%IDENTIFIER`. The parse tree then
 * contains a `<n00b_audit_v_null>` node wrapping that token at
 * every `NULL` site in expression position. `nullptr` is unaffected
 * — it tokenizes via the existing `%"nullptr"` keyword and lands
 * under `<constant>`, never under `<provided_identifier>`. The
 * approach is therefore **pure-BNF**; no post-walk text discriminator
 * is needed in the engine.
 *
 * Attachment point cited: c_ncc.bnf line 3,
 * `<provided_identifier> ::= %IDENTIFIER`. The preflight's leading
 * candidate proved sufficient once `nullptr`'s keyword status (line
 * 21) was confirmed.
 *
 * Phase 6 will copy the fragment verbatim into the reference
 * guidance JSON. The fragment is a single line (followed by a
 * second line for the attachment), uses no characters that need
 * additional escaping when round-tripped through a JSON string
 * value (newline becomes `\n`, quote becomes `\"`), and contains
 * no rich-string template substitutions or adjacent string
 * concatenations — both are libn00b footguns flagged in STATUS.md.
 *
 * **DF-D resolution.** The per-rule violation-nonterminal naming
 * convention is `n00b_audit_v_<short>`, matching the preflight's
 * proposal. For the `n00b.s2_1.null` rule the specific spelling is
 * `n00b_audit_v_null`. The `<short>` segment is the last dotted
 * component of the rule id with non-identifier characters stripped
 * (here, `null` from `n00b.s2_1.null`). This convention scales
 * cleanly to twenty rules — every rule's violation-nt is unique
 * because every rule id is unique, and the names are short enough
 * that any reasonable BNF-parser identifier-length limit is unlikely
 * to bite. When a second rule lands in WP-002+ this convention
 * becomes the durable contract.
 *
 * ## Notes — chosen libn00b APIs (verified against vendored headers)
 *
 * Source-of-truth headers under
 * `/Users/viega/n00b-audit/subprojects/n00b/include/`:
 *
 *   - `slay/bnf.h::n00b_bnf_load(bnf_text, start_symbol, grammar)`
 *     — composes the BNF text into the supplied grammar; returns
 *     `bool`. The combined-text approach (base + fragments
 *     concatenated with newline separators) is used here rather
 *     than calling `n00b_bnf_load` twice, because the loader calls
 *     `n00b_grammar_finalize` at the end of every successful load
 *     (bnf.c line 2359) and a second call against an already-
 *     finalized grammar is not part of the documented surface.
 *   - `slay/grammar.h::n00b_grammar_new()` / `n00b_grammar_free()`
 *     — grammar object lifecycle.
 *   - `slay/c_tokenizer.h::n00b_c_tokenize` (a `n00b_scan_cb_t`
 *     that handles the full C23 + ncc keyword set including
 *     `nullptr`, plus `#`-directive trivia and string literals).
 *     Created with `n00b_c_tokenizer_state_new()` and
 *     `n00b_c_tokenizer_reset`.
 *   - `parsers/scanner.h::n00b_scanner_new(buf, cb, g, .state, .reset_cb)`
 *     — wraps the input buffer in a scanner.
 *   - `parsers/token_stream.h::n00b_token_stream_new(scanner)` —
 *     wraps the scanner in a token stream consumed by the parser.
 *   - `slay/n00b_parse.h::n00b_grammar_parse(g, ts, mode, ...)`
 *     (macro over `n00b_parse`) — runs the unified parser.
 *     `N00B_PARSE_MODE_DEFAULT` tries PWZ first, falls back to
 *     Earley.
 *   - `slay/n00b_parse.h::n00b_parse_result_ok` /
 *     `n00b_parse_result_tree` / `n00b_parse_result_free` — outcome
 *     handling.
 *   - `slay/parse_tree.h::n00b_pt_search_by_nt(node, nt_name, out, max)`
 *     — recursive DFS that collects every node whose NT name matches.
 *     This is the rule-match finder.
 *   - `slay/parse_tree.h::n00b_pt_first_token(node)` — leftmost
 *     token-leaf in a subtree; used to pull line/column from the
 *     matched construct.
 *   - `slay/parse_tree.h` defines `n00b_parse_tree_t` as
 *     `n00b_tree_t(n00b_nt_node_t, n00b_token_info_t *)`; the leaf's
 *     `n00b_token_info_t *` carries `.line` and `.column`
 *     (uint32_t, 1-based, per `slay/token.h::n00b_token_info_t`).
 *     **These are the parse-tree source-location accessors used
 *     here — no invented numbers.**
 *   - `core/file.h::n00b_file_open(.kind = MMAP)` /
 *     `n00b_file_as_buffer` / `n00b_file_close` — same MMAP
 *     substrate the Phase 2 guidance loader uses.
 *   - `core/buffer.h::n00b_buffer_copy` / `n00b_buffer_concat` —
 *     used to assemble the base+fragments BNF text in-place. The
 *     resulting buffer is converted to `n00b_string_t *` via
 *     `n00b_string_from_raw` for the `n00b_bnf_load` call.
 *   - `core/alloc.h::n00b_alloc(T)` — zero-fills automatically, so
 *     the engine struct fields don't need explicit `= {}`
 *     initialization (matches the Phase 2 `guidance.c` precedent).
 *
 * Per project DECISIONS.md D-005, this implementation's public
 * functions carry no `_kargs` block — n00b-audit's own surface does
 * not expose `.allocator` keyword arguments.
 */

#include "n00b.h"
#include "core/file.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "adt/result.h"
#include "adt/list.h"
#include "parsers/scanner.h"
#include "parsers/token_stream.h"
#include "slay/bnf.h"
#include "slay/c_tokenizer.h"
#include "slay/grammar.h"
#include "internal/slay/grammar_internal.h"
#include "slay/n00b_parse.h"
#include "slay/parse_tree.h"
#include "slay/rewrite.h"
#include "slay/token.h"
#include "adt/tree.h"
#include "text/strings/string_ops.h"
#include "util/path.h"

#include "naudit/engine.h"
#include "naudit/guidance.h"
#include "naudit/rule.h"
#include "naudit/violation.h"
#include "naudit/errors.h"

#include "audit_paths.h"

/* ---------------------------------------------------------------- */
/* Engine struct                                                    */
/* ---------------------------------------------------------------- */

/*
 * Full definition of the engine handle. Opaque to consumers — only
 * the typedef in `include/audit/engine.h` is public, so internal
 * fields can evolve without breaking ABI.
 *
 *   - `grammar`   the merged base+fragments grammar produced by
 *                 `n00b_bnf_load`. Used by every `check_file` call.
 *   - `guidance`  back-pointer to the guidance struct so violation
 *                 events can link back to the matched rule. The
 *                 engine borrows this — caller owns the lifetime.
 */
struct n00b_audit_engine {
    n00b_grammar_t        *grammar;
    n00b_audit_guidance_t *guidance;
};

/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

/*
 * Read `c_ncc.bnf` from disk into a fresh n00b_buffer_t *. Returns
 * nullptr on any I/O failure. The buffer is a copy of the mmap'd
 * file content (so we can mutate it via `n00b_buffer_concat` to
 * append rule fragments).
 *
 * The path comes from the configure-time-baked N00B_AUDIT_GRAMMAR_PATH
 * macro in audit_paths.h — no string literals in C source per the
 * NCC.md "Build system" rule.
 */
static n00b_buffer_t *
read_base_grammar(void)
{
    n00b_string_t *path = n00b_string_from_cstr(N00B_AUDIT_GRAMMAR_PATH);
    auto           fr   = n00b_file_open(path, .kind = N00B_FILE_KIND_MMAP);
    if (n00b_result_is_err(fr)) {
        return nullptr;
    }
    n00b_file_t *f  = n00b_result_get(fr);
    auto         br = n00b_file_as_buffer(f);
    n00b_file_close(f);
    if (n00b_result_is_err(br)) {
        return nullptr;
    }
    n00b_buffer_t *mmap_buf = n00b_result_get(br);
    /* Copy so we can append fragments in-place without disturbing
     * the mapping. */
    return n00b_buffer_copy(mmap_buf);
}

/*
 * Append `\n<fragment>\n` to `dst` so each merged rule's BNF text
 * is separated from the previous block. A leading newline is safer
 * than relying on the base grammar ending with one; the BNF loader
 * also tolerates extra blank lines.
 */
static void
append_fragment(n00b_buffer_t *dst, n00b_string_t *fragment)
{
    n00b_buffer_t *sep = n00b_buffer_from_bytes((char *)"\n", 1);
    n00b_buffer_concat(dst, sep);
    n00b_buffer_t *body = n00b_buffer_from_bytes(fragment->data,
                                                 (int64_t)fragment->u8_bytes);
    n00b_buffer_concat(dst, body);
    n00b_buffer_concat(dst, sep);
}

/* ---------------------------------------------------------------- */
/* Public entries                                                   */
/* ---------------------------------------------------------------- */

n00b_result_t(n00b_audit_engine_t *)
n00b_audit_engine_new(n00b_audit_guidance_t *guidance)
{
    if (!guidance || !guidance->rules) {
        return n00b_result_err(n00b_audit_engine_t *,
                               N00B_AUDIT_ERR_ENGINE_BAD_ARGS);
    }

    /* Step 1: load the base grammar text. */
    n00b_buffer_t *combined = read_base_grammar();
    if (!combined) {
        return n00b_result_err(n00b_audit_engine_t *,
                               N00B_AUDIT_ERR_ENGINE_GRAMMAR_LOAD);
    }

    /*
     * Step 1.5: validate the base grammar loads on its own — gives
     * us a clean separation between GRAMMAR_LOAD (base broken) and
     * RULE_MERGE (a fragment broke the merged grammar). On the happy
     * path this is a one-time configure-style check.
     */
    {
        n00b_string_t *base_text = n00b_string_from_raw(combined->data,
                                                        (int64_t)combined->byte_len);
        n00b_grammar_t *probe = n00b_grammar_new();
        bool ok = n00b_bnf_load(base_text, r"translation_unit", probe);
        n00b_grammar_free(probe);
        if (!ok) {
            return n00b_result_err(n00b_audit_engine_t *,
                                   N00B_AUDIT_ERR_ENGINE_GRAMMAR_LOAD);
        }
    }

    /* Step 2: append each rule's fragment to the combined text. */
    size_t nrules = n00b_list_len(*guidance->rules);
    for (size_t i = 0; i < nrules; i++) {
        n00b_audit_rule_t *rule = n00b_list_get(*guidance->rules, i);
        if (!rule || !rule->bnf_fragment || rule->bnf_fragment->u8_bytes == 0) {
            continue;
        }
        append_fragment(combined, rule->bnf_fragment);
    }

    /* Step 3: load the merged grammar into a fresh grammar object. */
    n00b_string_t  *merged_text = n00b_string_from_raw(combined->data,
                                                       (int64_t)combined->byte_len);
    n00b_grammar_t *grammar     = n00b_grammar_new();
    bool ok = n00b_bnf_load(merged_text, r"translation_unit", grammar);
    if (!ok) {
        n00b_grammar_free(grammar);
        return n00b_result_err(n00b_audit_engine_t *,
                               N00B_AUDIT_ERR_ENGINE_RULE_MERGE);
    }

    /*
     * Step 3.5: validate each rule's `violation_nt` resolves to an
     * NT that actually exists in the merged grammar. A dangling
     * violation_nt (e.g., one whose name doesn't appear in any
     * fragment's RHS or LHS) sends the parser into pathological
     * behavior on every input. Fail fast with a clear schema error.
     */
    for (size_t i = 0; i < nrules; i++) {
        n00b_audit_rule_t *rule = n00b_list_get(*guidance->rules, i);
        if (!rule || !rule->violation_nt
            || rule->violation_nt->u8_bytes == 0) {
            continue;
        }
        if (!n00b_dict_contains(grammar->nt_map, rule->violation_nt)) {
            n00b_grammar_free(grammar);
            return n00b_result_err(n00b_audit_engine_t *,
                                   N00B_AUDIT_ERR_GUIDANCE_SCHEMA);
        }
    }

    struct n00b_audit_engine *e = n00b_alloc(struct n00b_audit_engine);
    e->grammar  = grammar;
    e->guidance = guidance;

    return n00b_result_ok(n00b_audit_engine_t *, e);
}

n00b_result_t(n00b_list_t(n00b_audit_violation_t *) *)
n00b_audit_engine_check_file(n00b_audit_engine_t *engine,
                             n00b_string_t       *path)
{
    if (!engine || !path) {
        return n00b_result_err(n00b_list_t(n00b_audit_violation_t *) *,
                               N00B_AUDIT_ERR_ENGINE_BAD_ARGS);
    }

    /* Canonicalize the source-file path: cwd-independent
     * diagnostics + downstream I/O regardless of caller cwd. */
    path = n00b_path_canonical(path);

    /* Step 1: open the file. */
    auto fr = n00b_file_open(path, .kind = N00B_FILE_KIND_MMAP);
    if (n00b_result_is_err(fr)) {
        return n00b_result_err(n00b_list_t(n00b_audit_violation_t *) *,
                               N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND);
    }
    n00b_file_t *f  = n00b_result_get(fr);
    auto         br = n00b_file_as_buffer(f);
    n00b_file_close(f);
    if (n00b_result_is_err(br)) {
        return n00b_result_err(n00b_list_t(n00b_audit_violation_t *) *,
                               N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND);
    }
    n00b_buffer_t *src_buf = n00b_result_get(br);

    /* Step 2: set up the C tokenizer + token stream. */
    n00b_c_tokenizer_state_t *st = n00b_c_tokenizer_state_new();
    n00b_scanner_t           *sc = n00b_scanner_new(
        src_buf, n00b_c_tokenize, engine->grammar,
        .state    = st,
        .reset_cb = n00b_c_tokenizer_reset);
    n00b_token_stream_t *ts = n00b_token_stream_new(sc);

    /* Step 3: parse. */
    n00b_parse_result_t *pr = n00b_grammar_parse(engine->grammar, ts,
                                                 N00B_PARSE_MODE_DEFAULT);
    if (!n00b_parse_result_ok(pr)) {
        n00b_parse_result_free(pr);
        return n00b_result_err(n00b_list_t(n00b_audit_violation_t *) *,
                               N00B_AUDIT_ERR_ENGINE_PARSE);
    }

    n00b_parse_tree_t *tree = n00b_parse_result_tree(pr);

    /* Step 4: allocate the result list. n00b_alloc zero-fills, so
     * after this the value-of-pointer is uninitialized junk — we set
     * it explicitly to a fresh list via n00b_list_new. */
    n00b_list_t(n00b_audit_violation_t *) *violations =
        n00b_alloc(n00b_list_t(n00b_audit_violation_t *));
    *violations = n00b_list_new(n00b_audit_violation_t *);

    /* Step 5: for every rule, DFS the tree for nodes matching the
     * rule's violation_nt and emit one violation per match. */
    size_t nrules = n00b_list_len(*engine->guidance->rules);
    for (size_t i = 0; i < nrules; i++) {
        n00b_audit_rule_t *rule = n00b_list_get(*engine->guidance->rules, i);
        if (!rule || !rule->violation_nt
            || rule->violation_nt->u8_bytes == 0) {
            continue;
        }

        /*
         * The violation_nt string is GC-managed but parse_tree.h's
         * search-by-nt accessor takes a null-terminated `const char *`.
         * `n00b_string_from_cstr` (used by the loader) produces a
         * null-terminated `.data` field, so the `.data` pointer is
         * safe to pass through as a C string. (The Phase 2 loader
         * builds the string this way; see guidance.c::
         * require_string_field.)
         */
        const char *nt_cstr = rule->violation_nt->data;

        /*
         * Cap the per-rule match collection at a generous fixed
         * size. Production runs against arbitrary C source can
         * exceed any single allocation; if a future WP needs
         * unbounded matches, swap to a two-pass approach (count,
         * then allocate). For Phase 3 fixtures the upper bound is
         * single-digit matches.
         */
        enum { N00B_AUDIT_ENGINE_MAX_MATCHES_PER_RULE = 4096 };
        n00b_parse_tree_t *matches[N00B_AUDIT_ENGINE_MAX_MATCHES_PER_RULE];
        int n_matches = n00b_pt_search_by_nt(tree, nt_cstr, matches,
                                             N00B_AUDIT_ENGINE_MAX_MATCHES_PER_RULE);

        for (int m = 0; m < n_matches; m++) {
            n00b_parse_tree_t *match = matches[m];
            if (!match) {
                continue;
            }
            n00b_parse_tree_t *first_leaf = n00b_pt_first_token(match);
            n00b_parse_tree_t *last_leaf  = n00b_pt_last_token(match);
            int64_t line     = 0;
            int64_t col      = 0;
            int64_t end_line = 0;
            int64_t end_col  = 0;
            if (first_leaf) {
                n00b_token_info_t *tok = n00b_parse_node_token(first_leaf);
                if (tok) {
                    line = (int64_t)tok->line;
                    col  = (int64_t)tok->column;
                }
            }
            if (last_leaf) {
                n00b_token_info_t *tok = n00b_parse_node_token(last_leaf);
                if (tok) {
                    end_line = (int64_t)tok->line;
                    end_col  = (int64_t)tok->endcol;
                }
            }

            /*
             * WP-007 Phase 2: look up the matched node's production
             * via the grammar; if the production carries a rewrite
             * block, compute the suggested replacement text via
             * slay's text-mode rewrite API.
             *
             * Best-effort: an err result from
             * `n00b_production_rewrite_text` leaves `rewrite =
             * nullptr` on the violation. The audit still emits the
             * violation; the only loss is the `--fix` suggestion
             * for that one match. (For Phase 2 the rules with
             * rewrites have no captures and zero error paths in
             * practice, so this path is defensive.)
             */
            n00b_string_t *rewrite_text = nullptr;
            if (!n00b_tree_is_leaf(match)) {
                n00b_nt_node_t *pn = &n00b_tree_node_value(match);
                n00b_parse_rule_t *production =
                    n00b_get_node_rule(engine->grammar, pn);
                if (production
                    && n00b_production_has_rewrite(production)) {
                    n00b_result_t(n00b_string_t *) rr =
                        n00b_production_rewrite_text(production, match);
                    if (n00b_result_is_ok(rr)) {
                        rewrite_text = n00b_result_get(rr);
                    }
                }
            }

            n00b_audit_violation_t *v = n00b_alloc(n00b_audit_violation_t);
            v->file       = path;
            v->line       = line;
            v->column     = col;
            v->end_line   = end_line;
            v->end_column = end_col;
            v->rule       = rule;
            v->rewrite    = rewrite_text;
            n00b_list_push(*violations, v);
        }
    }

    /*
     * The parse result owns the tree; we've collected what we need.
     * Don't free yet — the violation list borrows nothing from the
     * tree (line/column are scalar copies, file points to the caller's
     * path), so the parse result is safe to free after this loop.
     */
    n00b_parse_result_free(pr);

    return n00b_result_ok(n00b_list_t(n00b_audit_violation_t *) *, violations);
}
