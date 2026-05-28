/*
 * WP-009 Phase 1 — language-aware audit engine.
 *
 * Replaces the WP-001 Phase 3 hard-coded c_ncc.bnf load with
 * per-file dispatch via naudit's built-in language registry. For
 * each audited file:
 *
 *   1. Resolve the file's language via
 *      `n00b_naudit_lookup_language_by_extension`, consulting the
 *      project's `@extensions` overrides first and the built-in
 *      defaults second.
 *   2. Get-or-load the corresponding merged grammar from a
 *      per-engine cache keyed by language name. A multi-file
 *      invocation against a single language re-uses the same
 *      grammar object — the base grammar + fragments parse +
 *      `n00b_bnf_load` chain runs once per (engine, language)
 *      pair. (DF-V resolution: cache lives on the engine struct
 *      in a single `grammars` dict — string-keyed and populated
 *      on first miss; cache insertion only happens on the full
 *      load-AND-validate success path, so a cache hit implies
 *      D-017 validation already passed.)
 *   3. Construct the language's tokenizer via
 *      `n00b_naudit_lookup_tokenizer` (slay scan-cb + state-new
 *      factory + reset hook), parse, walk the parse tree, emit
 *      one violation per match.
 *
 * The D-017 dangling-`@violation_nt` validation moved from
 * `engine_new` to the per-(language, ruleset) grammar-load path
 * inside `check_file`: each merged grammar is validated against
 * every applicable rule's `violation_nt` exactly once. The
 * cached grammar is only inserted into `engine->grammars` after
 * validation passes, so a cache hit on `grammars` implies the
 * validation already succeeded. A dangling NT name aborts
 * `check_file` with `N00B_AUDIT_ERR_GUIDANCE_SCHEMA`, mirroring
 * the original WP-001 Phase 3 contract.
 *
 * D-018 (WP-007 Phase 2 rewrite blocks + violation `rewrite`
 * field) is preserved verbatim — the per-match rewrite-text
 * lookup loop in `check_file` is unchanged from its post-WP-007
 * shape.
 *
 * Per project DECISIONS.md D-005 / D-017 (the standing
 * compromise), this implementation's public functions carry no
 * `_kargs` block — naudit's existing surface does not thread
 * allocators.
 */

#include "n00b.h"
#include "core/file.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "adt/result.h"
#include "adt/list.h"
#include "adt/dict.h"
#include "parsers/scanner.h"
#include "parsers/token_stream.h"
#include "slay/bnf.h"
#include "slay/grammar.h"
#include "internal/slay/grammar_internal.h"
#include "slay/n00b_parse.h"
#include "slay/parse_tree.h"
#include "slay/rewrite.h"
#include "slay/token.h"
#include "adt/tree.h"
#include "text/strings/format.h"
#include "text/strings/string_ops.h"
#include "conduit/print.h"
#include "util/path.h"

#include "naudit/engine.h"
#include "naudit/guidance.h"
#include "naudit/rule.h"
#include "naudit/violation.h"
#include "naudit/errors.h"
#include "naudit/exemption.h"
#include "naudit/languages.h"
#include "naudit/tokenizer_registry.h"
#include "naudit/filter.h"
#include "n00b/eval.h"

/* ---------------------------------------------------------------- */
/* Engine struct                                                    */
/* ---------------------------------------------------------------- */

/*
 * Engine handle. Per-language merged grammars live in `grammars`,
 * keyed by language name; cache-hit on `grammars` skips both the
 * grammar load and the D-017 dangling-violation_nt validation
 * (the validation runs as part of the same cache-miss path and
 * only succeeds-and-caches together).
 *
 *   - `guidance`  back-pointer to the loaded guidance. The
 *                 engine borrows; the caller owns.
 *   - `grammars`  language-name → merged grammar pointer cache.
 *                 Each entry is a fully composed
 *                 `n00b_grammar_t *` (base grammar + applicable
 *                 rule fragments, post-D-017-validated) loaded
 *                 on first audit of that language. The dict is
 *                 locked (the WP-009 Phase 1 contract is
 *                 single-threaded, but the locked-by-default
 *                 `n00b_dict_new` precedent is the natural
 *                 primitive).
 */
struct n00b_audit_engine {
    n00b_audit_guidance_t                          *guidance;
    n00b_dict_t(n00b_string_t *, n00b_grammar_t *) *grammars;
    /*
     * WP-009 Phase 4 — per-engine embedded-eval session, lazily
     * allocated on the first audit invocation that touches a rule
     * with a filter. The compiled-filter cache maps filter name →
     * JIT'd predicate; populated on first reference so re-runs
     * of the same engine skip recompilation.
     *
     * `filter_compile_failed` short-circuits future audit calls
     * after a previously-failed compile so each subsequent file
     * gets the same propagation rather than a silent retry.
     */
    n00b_eval_session_t                                       *eval_session;
    n00b_dict_t(n00b_string_t *, n00b_eval_predicate_fn_t)    *compiled_filters;
    int                                                        filter_compile_err;
    /*
     * WP-011: when set, `n00b_audit_engine_check_file` skips the
     * baseline-suppression check (still applies the exemption list).
     * Tracks the `--ignore-baseline` CLI flag. Defaults to false.
     */
    bool                                                       ignore_baseline;
    /*
     * WP-012: when set, the exemption + baseline verification gate
     * accepts unsigned / bad-signature / unknown-signer records with
     * a stderr warning instead of dropping them. Tracks the
     * `--allow-unsigned` CLI flag. Defaults to false.
     */
    bool                                                       allow_unsigned;
    /*
     * WP-012: idempotency flag for the signature verification gate.
     * The gate walks `guidance->exemptions` + `guidance->baseline`
     * once on first `check_file` invocation, drops records that
     * don't verify (unless `allow_unsigned`), and sets this flag so
     * subsequent `check_file` calls skip the (expensive) subprocess
     * sweep. Per-engine caching, not per-process — a fresh engine
     * re-verifies.
     */
    bool                                                       signatures_applied;
};

/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

/*
 * Read a BNF grammar file (mmap'd) into a fresh n00b_buffer_t *.
 * Returns nullptr on any I/O failure. The buffer is a copy of the
 * mmap'd file content (so we can mutate it via
 * `n00b_buffer_concat` to append rule fragments).
 */
static n00b_buffer_t *
read_grammar_file(n00b_string_t *path)
{
    if (!path) {
        return nullptr;
    }
    auto fr = n00b_file_open(path, .kind = N00B_FILE_KIND_MMAP);
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
    return n00b_buffer_copy(mmap_buf);
}

/*
 * Append `\n<fragment>\n` to `dst` so each merged rule's BNF text
 * is separated from the previous block. A leading newline is
 * safer than relying on the base grammar ending with one; the BNF
 * loader also tolerates extra blank lines.
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

/*
 * Decide whether `rule` applies to language `lang_name`.
 *
 * Per WP-009 Phase 1 spec: empty / null `rule->language` means
 * "apply to every supported language whose grammar defines the
 * rule's violation_nt" — so this function returns true. A
 * non-empty list filters by exact-name match.
 */
static bool
rule_applies_to_language(n00b_audit_rule_t *rule,
                         n00b_string_t     *lang_name)
{
    if (!rule || !lang_name) {
        return false;
    }
    if (!rule->language || n00b_list_len(*rule->language) == 0) {
        return true;
    }
    int64_t n = n00b_list_len(*rule->language);
    for (int64_t i = 0; i < n; i++) {
        n00b_string_t *l = n00b_list_get(*rule->language, i);
        if (l && n00b_unicode_str_eq(l, lang_name)) {
            return true;
        }
    }
    return false;
}

/*
 * Build (or fetch from cache) the merged grammar for a language.
 *
 * On a cache miss:
 *   1. Read the language's base grammar file.
 *   2. Sanity-check the base grammar loads on its own — separates
 *      GRAMMAR_LOAD failures from RULE_MERGE failures.
 *   3. Append each applicable rule's `bnf_fragment` to the
 *      combined text.
 *   4. Load the merged text into a fresh grammar object.
 *   5. Validate every applicable rule's `violation_nt` resolves
 *      to an NT in the merged grammar (D-017 dangling-NT check).
 *   6. Cache the grammar by language name.
 *
 * Returns the grammar pointer on success; sets `*err_out` to an
 * `N00B_AUDIT_ERR_*` code and returns nullptr on failure.
 */
static n00b_grammar_t *
get_or_load_grammar(n00b_audit_engine_t *engine,
                    n00b_naudit_language_info_t *lang,
                    int                 *err_out)
{
    *err_out = 0;

    bool            found     = false;
    n00b_grammar_t *cached    = n00b_dict_get(engine->grammars,
                                              lang->name, &found);
    if (found && cached) {
        return cached;
    }

    /* Step 1: read the language's base grammar. */
    n00b_buffer_t *combined = read_grammar_file(lang->grammar_path);
    if (!combined) {
        *err_out = N00B_AUDIT_ERR_ENGINE_GRAMMAR_LOAD;
        return nullptr;
    }

    /* Step 2: probe-load the base grammar in isolation. */
    {
        n00b_string_t *base_text = n00b_string_from_raw(combined->data,
                                                        (int64_t)combined->byte_len);
        n00b_grammar_t *probe = n00b_grammar_new();
        bool ok = n00b_bnf_load(base_text, r"translation_unit", probe);
        n00b_grammar_free(probe);
        if (!ok) {
            *err_out = N00B_AUDIT_ERR_ENGINE_GRAMMAR_LOAD;
            return nullptr;
        }
    }

    /*
     * Step 3: append each applicable rule's fragment to the
     * combined text. A rule applies to this language if it has
     * no `@language` annotation (applies to all) or if its
     * `@language` list contains this language's name.
     *
     * Phase 1 keeps the existing bnf_fragment shape; future
     * phases will allow rules without fragments (query-mode).
     */
    size_t nrules = n00b_list_len(*engine->guidance->rules);
    for (size_t i = 0; i < nrules; i++) {
        n00b_audit_rule_t *rule = n00b_list_get(*engine->guidance->rules,
                                                i);
        if (!rule || !rule->bnf_fragment
            || rule->bnf_fragment->u8_bytes == 0) {
            continue;
        }
        if (!rule_applies_to_language(rule, lang->name)) {
            continue;
        }
        append_fragment(combined, rule->bnf_fragment);
    }

    /* Step 4: load the merged grammar. */
    n00b_string_t  *merged_text = n00b_string_from_raw(
        combined->data, (int64_t)combined->byte_len);
    n00b_grammar_t *grammar     = n00b_grammar_new();
    bool ok = n00b_bnf_load(merged_text, r"translation_unit", grammar);
    if (!ok) {
        n00b_grammar_free(grammar);
        *err_out = N00B_AUDIT_ERR_ENGINE_RULE_MERGE;
        return nullptr;
    }

    /*
     * Step 5: validate each applicable rule's `violation_nt`
     * resolves to an NT in the merged grammar. A dangling
     * `@violation_nt` sends the parser into pathological
     * behavior on every input — fail fast with the canonical
     * schema-error code (D-017 contract).
     */
    for (size_t i = 0; i < nrules; i++) {
        n00b_audit_rule_t *rule = n00b_list_get(*engine->guidance->rules,
                                                i);
        if (!rule || !rule->violation_nt
            || rule->violation_nt->u8_bytes == 0) {
            continue;
        }
        if (!rule_applies_to_language(rule, lang->name)) {
            continue;
        }
        if (!n00b_dict_contains(grammar->nt_map, rule->violation_nt)) {
            n00b_grammar_free(grammar);
            *err_out = N00B_AUDIT_ERR_GUIDANCE_SCHEMA;
            return nullptr;
        }
    }

    /*
     * Step 6: cache. The dict put is keyed by the language's own
     * name pointer (process-lifetime, from the registry). Cache
     * insertion happens only on the success path, so a
     * cache-hit on `grammars` is equivalent to "load AND
     * D-017 validation passed."
     */
    n00b_dict_put(engine->grammars, lang->name, grammar);
    return grammar;
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

    /*
     * The grammar load no longer happens here. Per WP-009 Phase
     * 1, the engine learns the audited file's language inside
     * `check_file` and loads (or fetches the cached) merged
     * grammar there. `engine_new` only stashes the guidance
     * pointer and initializes the per-language caches.
     */
    struct n00b_audit_engine *e = n00b_alloc(struct n00b_audit_engine);
    e->guidance = guidance;

    e->grammars = n00b_alloc(
        n00b_dict_t(n00b_string_t *, n00b_grammar_t *));
    n00b_dict_init(e->grammars,
                   .hash          = n00b_string_hash,
                   .skip_obj_hash = true);

    e->eval_session       = nullptr;
    e->compiled_filters   = nullptr;
    e->filter_compile_err = 0;
    e->ignore_baseline    = false;
    e->allow_unsigned     = false;
    e->signatures_applied = false;

    return n00b_result_ok(n00b_audit_engine_t *, e);
}

/*
 * WP-011: setter for the engine's per-invocation `ignore_baseline`
 * flag. The CLI calls this when `--ignore-baseline` was passed; the
 * default (false) leaves the baseline as a bulk suppression source.
 */
void
n00b_audit_engine_set_ignore_baseline(n00b_audit_engine_t *engine,
                                       bool                 ignore)
{
    if (!engine) {
        return;
    }
    engine->ignore_baseline = ignore;
}

/*
 * WP-012: setter for the engine's `allow_unsigned` policy flag.
 * Default false; the CLI calls this when `--allow-unsigned` was
 * passed. When set, the exemption / baseline signature gate
 * downgrades verification failure from "drop the record" to
 * "warn + keep the record" — matching the WP-012 prompt's
 * policy contract.
 */
void
n00b_audit_engine_set_allow_unsigned(n00b_audit_engine_t *engine,
                                      bool                 allow)
{
    if (!engine) {
        return;
    }
    engine->allow_unsigned = allow;
}

/* ---------------------------------------------------------------- */
/* Filter compilation (WP-009 Phase 4)                              */
/* ---------------------------------------------------------------- */

/*
 * Lazily allocate the per-engine eval session + compiled-filter
 * cache. Returns 0 on success or an N00B_AUDIT_ERR_* code that the
 * caller propagates back to check_file's caller.
 *
 * Per the WP-009 Phase 4 design decision documented in
 * `key_findings`, filter compilation happens lazily on the first
 * audit invocation that touches a filter-bearing rule (rather than
 * eagerly in `n00b_audit_engine_new`). Rationale: rule files
 * without filters pay zero startup cost; rule files with filters
 * front-load the JIT setup once, then per-match invocation is a
 * native call.
 */
static int
ensure_eval_session(n00b_audit_engine_t *engine)
{
    if (engine->eval_session) {
        return 0;
    }
    if (engine->filter_compile_err) {
        return engine->filter_compile_err;
    }

    auto sr = n00b_naudit_filter_session_new();
    if (n00b_result_is_err(sr)) {
        engine->filter_compile_err = N00B_AUDIT_ERR_GUIDANCE_SCHEMA;
        return engine->filter_compile_err;
    }
    engine->eval_session = n00b_result_get(sr);

    engine->compiled_filters = n00b_alloc(
        n00b_dict_t(n00b_string_t *, n00b_eval_predicate_fn_t));
    n00b_dict_init(engine->compiled_filters,
                   .hash          = n00b_string_hash,
                   .skip_obj_hash = true);
    return 0;
}

/*
 * Look up (or compile + cache) a filter's JIT'd predicate by name.
 * Returns nullptr + sets `*err` on failure; the caller treats a
 * failed-compile as N00B_AUDIT_ERR_GUIDANCE_SCHEMA so the diagnostic
 * surface stays consistent with the dangling-name path.
 */
static n00b_eval_predicate_fn_t
get_or_compile_filter(n00b_audit_engine_t *engine,
                      n00b_string_t       *filter_name,
                      int                 *err)
{
    *err = 0;
    if (!filter_name || filter_name->u8_bytes == 0) {
        return nullptr;
    }
    if (engine->filter_compile_err) {
        *err = engine->filter_compile_err;
        return nullptr;
    }
    int session_err = ensure_eval_session(engine);
    if (session_err) {
        *err = session_err;
        return nullptr;
    }

    bool found = false;
    n00b_eval_predicate_fn_t cached =
        n00b_dict_get(engine->compiled_filters, filter_name, &found);
    if (found && cached) {
        return cached;
    }

    if (!engine->guidance->filters) {
        engine->filter_compile_err = N00B_AUDIT_ERR_GUIDANCE_SCHEMA;
        *err = engine->filter_compile_err;
        return nullptr;
    }
    bool filter_present = false;
    n00b_audit_filter_t *filter = n00b_dict_get(engine->guidance->filters,
                                                 filter_name,
                                                 &filter_present);
    if (!filter_present || !filter || !filter->expr) {
        engine->filter_compile_err = N00B_AUDIT_ERR_GUIDANCE_SCHEMA;
        *err = engine->filter_compile_err;
        return nullptr;
    }

    auto cr = n00b_naudit_filter_compile(engine->eval_session,
                                         filter_name, filter->expr);
    if (n00b_result_is_err(cr)) {
        engine->filter_compile_err = N00B_AUDIT_ERR_GUIDANCE_SCHEMA;
        *err = engine->filter_compile_err;
        return nullptr;
    }
    n00b_eval_predicate_fn_t fn = n00b_result_get(cr);
    n00b_dict_put(engine->compiled_filters, filter_name, fn);
    return fn;
}

/* ---------------------------------------------------------------- */
/* Capture binding (WP-009 Phase 4)                                 */
/* ---------------------------------------------------------------- */

/*
 * Bind each `$name:<NT>` capture declared on the rule to the
 * appropriate descendant of @p match in document order (pre-order
 * DFS — `n00b_pt_search_by_nt`'s documented contract; see
 * `include/slay/parse_tree.h` line 261).
 *
 * Per the WP-009 Phase 4 DF-U resolution: the Nth `$name<I>:<NT>`
 * declaration (in the rule's source-order capture list) binds to
 * the Nth descendant of @p match whose NT matches `<NT>`.
 * Declarations sharing a `<NT>` name resolve positionally; declarations with distinct NT names bind independently (each
 * uses its own zero-th descendant of that NT).
 *
 * Captures whose declared NT has fewer than the requested ordinal
 * of matches in the subtree are left unbound — the filter
 * expression's `arg.capture(name)` then returns nullptr, which
 * the JIT exposes as a falsy value the predicate can detect.
 *
 * The 4096 cap on per-NT match lookups matches
 * `n00b_audit_engine_check_file`'s per-rule cap so the budgets
 * stay aligned.
 */
static void
bind_captures(n00b_audit_rule_t   *rule,
              n00b_parse_tree_t   *match,
              n00b_naudit_match_t *handle)
{
    if (!rule || !rule->captures || !match || !handle) {
        return;
    }
    /*
     * Per-NT counter for declaration-order binding. We rebuild
     * this for every match (matches are independent); within a
     * single match's capture list, the Nth declaration of an NT
     * binds to the Nth descendant.
     */
    int64_t ndecls = n00b_list_len(*rule->captures);
    for (int64_t i = 0; i < ndecls; i++) {
        n00b_audit_capture_decl_t *decl = n00b_list_get(*rule->captures,
                                                         i);
        if (!decl || !decl->name || !decl->nt
            || decl->nt->u8_bytes == 0) {
            continue;
        }
        /*
         * Count how many prior declarations share this NT — that
         * gives us the ordinal of the descendant we want for the
         * current declaration.
         */
        int64_t ordinal = 0;
        for (int64_t j = 0; j < i; j++) {
            n00b_audit_capture_decl_t *prev = n00b_list_get(*rule->captures,
                                                             j);
            if (prev && prev->nt
                && n00b_unicode_str_eq(prev->nt, decl->nt)) {
                ordinal++;
            }
        }

        enum { N00B_AUDIT_ENGINE_MAX_CAPTURE_MATCHES = 4096 };
        n00b_parse_tree_t *cands[N00B_AUDIT_ENGINE_MAX_CAPTURE_MATCHES];
        int n_cands = n00b_pt_search_by_nt(match, decl->nt->data, cands,
                                           N00B_AUDIT_ENGINE_MAX_CAPTURE_MATCHES);
        if (ordinal < n_cands) {
            n00b_naudit_match_bind_capture(handle, decl->name,
                                           cands[ordinal]);
        }
    }
}

/*
 * WP-012 — apply the signature verification gate to the loaded
 * exemption + baseline lists. Walks each record, calls
 * `n00b_audit_exemption_verify` against the roster, and either
 * drops or keeps the record per the engine's `allow_unsigned`
 * policy. Idempotent via `engine->signatures_applied`.
 *
 * Rebuilds the suppression lists in place: records that fail
 * verification are filtered out (unless allow_unsigned), records
 * that pass are kept. The list-of-void-pointer cast pattern is the
 * same one used by the existing suppression loop below.
 *
 * Behavior in the corner cases:
 *   - guidance->allowed_signers_path is null  → no roster on disk:
 *       * allow_unsigned: keep all records + emit ONE roster-
 *         missing warning to stderr (not per-record).
 *       * else: drop all records + emit ONE roster-missing
 *         refusal to stderr.
 *   - roster present, individual record fails verify:
 *       * allow_unsigned: keep record + per-record warning.
 *       * else: drop record + per-record warning.
 *
 * All warnings go to stderr via `n00b_eprintf`; no return code
 * (the gate is advisory — the affected records simply disappear
 * from the suppression list in the strict path).
 */
static void
apply_signature_gate(n00b_audit_engine_t *engine)
{
    if (!engine || engine->signatures_applied) {
        return;
    }
    engine->signatures_applied = true;

    n00b_audit_guidance_t *g = engine->guidance;
    if (!g) {
        return;
    }

    /* Combined view of "do we have anything to verify?" */
    bool have_exemptions = (g->exemptions
                            && n00b_list_len(*g->exemptions) > 0);
    bool have_baseline   = (g->baseline
                            && n00b_list_len(*g->baseline) > 0);
    if (!have_exemptions && !have_baseline) {
        return;
    }

    /*
     * Roster discovery behavior (WP-012 documented contract):
     *   - roster absent + --allow-unsigned: warn once, accept-all.
     *   - roster absent + strict mode: warn once, refuse-all (clear
     *     the suppression lists).
     *   - roster present: verify per-record below.
     */
    if (!g->allowed_signers_path
        || g->allowed_signers_path->u8_bytes == 0) {
        if (engine->allow_unsigned) {
            n00b_eprintf(
                "n00b-audit: no audit/allowed_signers roster found; --allow-unsigned: accepting all exemption + baseline records");
            return;
        }
        n00b_eprintf(
            "n00b-audit: no audit/allowed_signers roster found; refusing all exemption + baseline records (pass --allow-unsigned to accept)");
        if (have_exemptions) {
            *g->exemptions = n00b_list_new(void *);
        }
        if (have_baseline) {
            *g->baseline = n00b_list_new(void *);
        }
        return;
    }

    /*
     * Per-list verification. We walk each loaded record, attempt
     * to verify its source file against the roster, and either
     * keep the record (verify ok OR allow_unsigned warn-and-keep)
     * or drop it (verify failed under strict mode).
     *
     * Two list shapes — exemptions and baseline — but identical
     * structure. The helper lambda-ish pattern in C: a small
     * walker writing to a fresh list of the same type.
     */
    n00b_list_t(void *) *both[2];
    both[0] = have_exemptions ? g->exemptions : nullptr;
    both[1] = have_baseline ? g->baseline : nullptr;
    const char *labels[2] = {"exemption", "baseline"};

    for (int side = 0; side < 2; side++) {
        n00b_list_t(void *) *src = both[side];
        if (!src) {
            continue;
        }
        n00b_list_t(void *) *keep = n00b_alloc(n00b_list_t(void *));
        *keep = n00b_list_new(void *);
        int64_t n = n00b_list_len(*src);
        for (int64_t i = 0; i < n; i++) {
            n00b_audit_exemption_t *ex =
                (n00b_audit_exemption_t *)n00b_list_get(*src, i);
            if (!ex || !ex->source_file) {
                continue;
            }
            n00b_string_t *signer = ex->signer_id;
            /*
             * Records baselined under WP-011 carry an empty
             * `signer_id`. The verify helper would treat an empty
             * principal as "no name supplied" — ssh-keygen rejects
             * that. Treat empty as missing-signature so the policy
             * decision is the same: drop in strict mode, warn-and-
             * keep in allow_unsigned mode.
             */
            int err = 0;
            if (!signer || signer->u8_bytes == 0) {
                err = N00B_AUDIT_ERR_EXEMPTION_NO_SIGNATURE;
            }
            else {
                auto vr = n00b_audit_exemption_verify(
                    ex->source_file, g->allowed_signers_path, signer);
                if (n00b_result_is_err(vr)) {
                    err = n00b_result_get_err(vr);
                }
            }

            if (err == 0) {
                n00b_list_push(*keep, (void *)ex);
                continue;
            }

            /* Failure path — differentiate diagnostic on err. */
            n00b_string_t *reason = n00b_audit_err_str(err);
            if (engine->allow_unsigned) {
                n00b_eprintf(
                    "n00b-audit: --allow-unsigned: keeping «#» «#» despite signature failure: «#»",
                    n00b_string_from_cstr(labels[side]),
                    ex->source_file,
                    reason);
                n00b_list_push(*keep, (void *)ex);
            }
            else {
                n00b_eprintf(
                    "n00b-audit: refusing «#» «#»: «#»",
                    n00b_string_from_cstr(labels[side]),
                    ex->source_file,
                    reason);
            }
        }
        *src = *keep;
    }
}

n00b_result_t(n00b_list_t(n00b_audit_violation_t *) *)
n00b_audit_engine_check_file(n00b_audit_engine_t *engine,
                             n00b_string_t       *path)
{
    if (!engine || !path) {
        return n00b_result_err(n00b_list_t(n00b_audit_violation_t *) *,
                               N00B_AUDIT_ERR_ENGINE_BAD_ARGS);
    }

    /*
     * WP-012: apply the signature verification gate before the per-
     * file audit pass. Idempotent — runs once per engine lifetime.
     * Records that fail verification under strict mode are dropped
     * from `guidance->exemptions` / `guidance->baseline` before the
     * suppression loop below ever sees them; records that pass (or
     * that --allow-unsigned re-admits with a warning) survive.
     */
    apply_signature_gate(engine);

    /* Canonicalize the source-file path: cwd-independent
     * diagnostics + downstream I/O regardless of caller cwd. */
    path = n00b_path_canonical(path);

    /*
     * Step 0 (WP-009 Phase 1): resolve the audited file's
     * language via the built-in registry plus any project-level
     * extension overrides on the guidance struct.
     */
    /*
     * `n00b_path_get_extension` returns the extension WITH the
     * leading `.` (see `src/util/path.c::n00b_path_get_extension`,
     * which slices from the `rfind_period` index); the registry's
     * default-extensions list stores entries with the leading `.`
     * too, so the lookup compares apples to apples. The defensive
     * fallback below catches the (unlikely) case where the
     * accessor changes shape in a future libn00b sweep.
     */
    n00b_string_t *ext = n00b_path_get_extension(path);
    if (ext && ext->u8_bytes > 0 && ext->data[0] != '.') {
        n00b_buffer_t *acc = n00b_buffer_from_bytes((char *)".", 1);
        n00b_buffer_t *eb  = n00b_buffer_from_bytes(ext->data,
                                                    (int64_t)ext->u8_bytes);
        n00b_buffer_concat(acc, eb);
        ext = n00b_string_from_raw(acc->data, (int64_t)acc->byte_len);
    }
    n00b_naudit_language_info_t *lang =
        n00b_naudit_lookup_language_by_extension(
            ext, engine->guidance->extension_overrides);
    if (!lang) {
        return n00b_result_err(n00b_list_t(n00b_audit_violation_t *) *,
                               N00B_AUDIT_ERR_ENGINE_UNKNOWN_LANGUAGE);
    }

    /* Get-or-load the merged grammar for this language. */
    int             err     = 0;
    n00b_grammar_t *grammar = get_or_load_grammar(engine, lang, &err);
    if (!grammar) {
        return n00b_result_err(n00b_list_t(n00b_audit_violation_t *) *,
                               err);
    }

    /* Look up the tokenizer triple by name. */
    n00b_naudit_tokenizer_info_t *tok =
        n00b_naudit_lookup_tokenizer(lang->tokenizer_name);
    if (!tok || !tok->scan_cb || !tok->state_new) {
        return n00b_result_err(n00b_list_t(n00b_audit_violation_t *) *,
                               N00B_AUDIT_ERR_ENGINE_UNKNOWN_LANGUAGE);
    }

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
    /*
     * Materialize the file's full source text once per audit
     * invocation; the match handle's `.text` accessor + the Phase 4
     * `arg.starts_with(...)` predicate need it. We pass the same
     * string into every match handle constructed for this file.
     */
    n00b_string_t *src_text = n00b_string_from_raw(
        src_buf->data, (int64_t)src_buf->byte_len);

    /* Step 2: set up the tokenizer + token stream via the
     * registry's per-language triple. */
    void *st = tok->state_new();
    n00b_scanner_t *sc = n00b_scanner_new(
        src_buf, tok->scan_cb, grammar,
        .state    = st,
        .reset_cb = tok->reset_cb);
    n00b_token_stream_t *ts = n00b_token_stream_new(sc);

    /* Step 3: parse. */
    n00b_parse_result_t *pr = n00b_grammar_parse(grammar, ts,
                                                 N00B_PARSE_MODE_DEFAULT);
    if (!n00b_parse_result_ok(pr)) {
        n00b_parse_result_free(pr);
        return n00b_result_err(n00b_list_t(n00b_audit_violation_t *) *,
                               N00B_AUDIT_ERR_ENGINE_PARSE);
    }

    n00b_parse_tree_t *tree = n00b_parse_result_tree(pr);

    /* Step 4: allocate the result list. */
    n00b_list_t(n00b_audit_violation_t *) *violations =
        n00b_alloc(n00b_list_t(n00b_audit_violation_t *));
    *violations = n00b_list_new(n00b_audit_violation_t *);

    /*
     * Step 5: for every rule applicable to this language, DFS
     * the tree for nodes matching the rule's violation_nt and
     * emit one violation per match.
     *
     * Rules NOT applicable to this language are skipped silently
     * — they don't apply to this file.
     */
    size_t nrules = n00b_list_len(*engine->guidance->rules);
    for (size_t i = 0; i < nrules; i++) {
        n00b_audit_rule_t *rule = n00b_list_get(*engine->guidance->rules,
                                                i);
        if (!rule || !rule->violation_nt
            || rule->violation_nt->u8_bytes == 0) {
            continue;
        }
        if (!rule_applies_to_language(rule, lang->name)) {
            continue;
        }

        const char *nt_cstr = rule->violation_nt->data;

        enum { N00B_AUDIT_ENGINE_MAX_MATCHES_PER_RULE = 4096 };
        n00b_parse_tree_t *matches[N00B_AUDIT_ENGINE_MAX_MATCHES_PER_RULE];
        int n_matches = n00b_pt_search_by_nt(tree, nt_cstr, matches,
                                             N00B_AUDIT_ENGINE_MAX_MATCHES_PER_RULE);

        /*
         * WP-009 Phase 4: resolve the filter predicate once per
         * (rule, file). The predicate is engine-cached so subsequent
         * files audited by the same engine reuse the JIT'd
         * function pointer.
         */
        n00b_eval_predicate_fn_t filter_fn = nullptr;
        if (rule->filter_name && rule->filter_name->u8_bytes > 0) {
            int ferr = 0;
            filter_fn = get_or_compile_filter(engine, rule->filter_name,
                                              &ferr);
            if (ferr) {
                return n00b_result_err(
                    n00b_list_t(n00b_audit_violation_t *) *, ferr);
            }
        }

        for (int m = 0; m < n_matches; m++) {
            n00b_parse_tree_t *match = matches[m];
            if (!match) {
                continue;
            }

            /*
             * WP-009 Phase 4: bind captures + apply the filter
             * BEFORE computing the rest of the violation record.
             * Suppressed matches contribute no list entries.
             */
            if (filter_fn) {
                n00b_naudit_match_t *handle = n00b_naudit_match_new(
                    match, src_text);
                bind_captures(rule, match, handle);
                bool keep = n00b_naudit_filter_apply_handle(filter_fn,
                                                            handle);
                if (!keep) {
                    continue;
                }
            }

            n00b_parse_tree_t *first_leaf = n00b_pt_first_token(match);
            n00b_parse_tree_t *last_leaf  = n00b_pt_last_token(match);
            int64_t line     = 0;
            int64_t col      = 0;
            int64_t end_line = 0;
            int64_t end_col  = 0;
            if (first_leaf) {
                n00b_token_info_t *tk = n00b_parse_node_token(first_leaf);
                if (tk) {
                    line = (int64_t)tk->line;
                    col  = (int64_t)tk->column;
                }
            }
            if (last_leaf) {
                n00b_token_info_t *tk = n00b_parse_node_token(last_leaf);
                if (tk) {
                    end_line = (int64_t)tk->line;
                    end_col  = (int64_t)tk->endcol;
                }
            }

            /*
             * D-018 / WP-007 Phase 2: preserve the rewrite-text
             * lookup for the matched production. Unchanged from
             * the prior shape.
             */
            n00b_string_t *rewrite_text = nullptr;
            if (!n00b_tree_is_leaf(match)) {
                n00b_nt_node_t *pn = &n00b_tree_node_value(match);
                n00b_parse_rule_t *production =
                    n00b_get_node_rule(grammar, pn);
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
            /*
             * WP-011: compute the region fingerprint from the
             * matched span's source bytes (the locator is already
             * resolved above). The fingerprint becomes part of the
             * violation record and is the matching primitive for
             * exemption + baseline suppression below.
             */
            n00b_string_t *region_bytes = n00b_audit_extract_region_bytes(
                path, line, col, end_line, end_col);
            v->region_fingerprint = n00b_audit_compute_region_fingerprint(
                region_bytes ? region_bytes : n00b_string_empty());

            /*
             * WP-011: apply the suppression engine. Baseline first
             * (unless `--ignore-baseline` is set), then per-record
             * exemptions. A matching entry drops the violation
             * before it ever reaches the result list.
             */
            bool suppressed = false;
            if (!engine->ignore_baseline && engine->guidance->baseline) {
                int64_t nb = n00b_list_len(*engine->guidance->baseline);
                for (int64_t k = 0; k < nb && !suppressed; k++) {
                    n00b_audit_exemption_t *ex =
                        (n00b_audit_exemption_t *)
                            n00b_list_get(*engine->guidance->baseline, k);
                    if (n00b_audit_exemption_match(ex, v)) {
                        suppressed = true;
                    }
                }
            }
            if (!suppressed && engine->guidance->exemptions) {
                int64_t ne =
                    n00b_list_len(*engine->guidance->exemptions);
                for (int64_t k = 0; k < ne && !suppressed; k++) {
                    n00b_audit_exemption_t *ex =
                        (n00b_audit_exemption_t *)
                            n00b_list_get(*engine->guidance->exemptions, k);
                    if (n00b_audit_exemption_match(ex, v)) {
                        suppressed = true;
                    }
                }
            }
            if (suppressed) {
                continue;
            }

            n00b_list_push(*violations, v);
        }
    }

    n00b_parse_result_free(pr);
    return n00b_result_ok(n00b_list_t(n00b_audit_violation_t *) *, violations);
}
