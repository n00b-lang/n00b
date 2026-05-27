/*
 * WP-005 — guidance-file loader (rule-file format v2).
 *
 * Replaces the WP-001 Phase 2 JSON-based loader. The v2 format is a
 * slay-format `.bnf` text file carrying `@directive` metadata; the
 * loader uses libn00b's `parsers/scanner.h` + `slay/n00b_parse.h`
 * pipeline with:
 *
 *   - the custom `"audit_rule_file"` tokenizer registered by
 *     `n00b_audit_module_init` (see src/audit/rule_file_tokenizer.c);
 *   - the embedded metagrammar string
 *     `N00B_AUDIT_RULE_FILE_METAGRAMMAR` from
 *     `audit_rule_file_grammar.h` (built at configure-time from
 *     `grammars/audit-rule-file.bnf` via `scripts/embed_metagrammar.py`);
 *   - `n00b_bnf_load` to compose the metagrammar object, then
 *     `n00b_grammar_parse` to parse the rule-file token stream;
 *   - a recursive descent over the resulting parse tree to populate
 *     `n00b_audit_guidance_t` + per-rule `n00b_audit_rule_t` structs.
 *
 * Per-rule embedded BNF productions are extracted as opaque
 * `BNF_LINE` text and concatenated into the rule's `bnf_fragment`
 * field — the engine's existing `n00b_bnf_load` call (in
 * src/audit/engine.c) then merges that text into the base grammar
 * unchanged.
 *
 * File I/O surface: same `core/file.h` MMAP substrate used by the
 * Phase 2 loader; precedent
 * `subprojects/n00b/src/chalk/file_io.c::n00b_chalk_read_file`.
 *
 * Allocation: structs are allocated via `n00b_alloc(T)`. No `malloc`.
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
#include "slay/grammar.h"
#include "slay/n00b_parse.h"
#include "slay/parse_tree.h"
#include "slay/token.h"
#include "text/strings/string_ops.h"
#include "util/path.h"

#include "naudit/guidance.h"
#include "naudit/rule.h"
#include "naudit/errors.h"
#include "internal/naudit/_naudit_internal.h"

#include "audit_rule_file_grammar.h"

/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

/*
 * Compare an n00b_string_t to a C-string literal. The audit-rule
 * file's directive names are short ASCII (e.g. "schema_version") so
 * byte-wise compare is correct.
 */
static bool
str_eq_cstr(n00b_string_t *s, const char *expected)
{
    if (!s || !expected) {
        return false;
    }
    size_t elen = 0;
    while (expected[elen]) {
        elen++;
    }
    if (s->u8_bytes != elen) {
        return false;
    }
    for (size_t i = 0; i < elen; i++) {
        if (s->data[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

/*
 * Extract the n00b_string_t * value from a token leaf, or nullptr
 * if the leaf has no payload. The token's `value` is a
 * n00b_option_t(n00b_string_t *).
 */
static n00b_string_t *
token_text(n00b_token_info_t *tok)
{
    if (!tok) {
        return nullptr;
    }
    if (!n00b_option_is_set(tok->value)) {
        return nullptr;
    }
    return n00b_option_get(tok->value);
}

/*
 * Get the Nth child of `node`, descending through any
 * single-child non-terminal wrapper (slay's BNF loader wraps each
 * terminal in a `$term-...` synthetic NT when the terminal appears
 * inside a production's RHS — see the dumped tree shape).
 *
 * The wrapper has exactly one child: the token leaf. We return
 * the token-leaf's `n00b_token_info_t *` if the descent ends on a
 * token; otherwise nullptr.
 */
static n00b_token_info_t *
nth_token_child(n00b_parse_tree_t *node, int idx)
{
    if (!node) {
        return nullptr;
    }
    size_t n = n00b_pt_num_children(node);
    if (idx < 0 || (size_t)idx >= n) {
        return nullptr;
    }
    n00b_parse_tree_t *child = n00b_pt_get_child(node, (size_t)idx);
    if (!child) {
        return nullptr;
    }
    /* Descend through $term-* wrappers (each carries exactly one
     * child, the token leaf or another wrapper). */
    while (!n00b_pt_is_token(child)) {
        size_t nc = n00b_pt_num_children(child);
        if (nc != 1) {
            return nullptr;
        }
        child = n00b_pt_get_child(child, 0);
        if (!child) {
            return nullptr;
        }
    }
    return n00b_parse_node_token(child);
}

/* ---------------------------------------------------------------- */
/* Multi-line value assembly                                        */
/* ---------------------------------------------------------------- */

/*
 * Given a <meta_field> node, walk its <continuation> children and
 * append each continuation's text to the value. The result is a
 * single n00b_string_t that joins REST and all continuation lines
 * with "\n" separators (only when continuations are present).
 *
 * The <meta_field> tree shape is:
 *
 *   <meta_field>
 *     DIRECTIVE      (token leaf)
 *     REST           (token leaf)
 *     NEWLINE        (token leaf)
 *     <continuation>* (children, possibly under a $$group_N wrapper)
 *
 * `n00b_pt_collect_nt_deep` flattens through `$$group_N` synthetic
 * nodes so we get all continuation children directly.
 */
static n00b_string_t *
assemble_field_value(n00b_parse_tree_t *meta_field_node)
{
    n00b_token_info_t *rest_tok = nth_token_child(meta_field_node, 1);
    n00b_string_t     *rest     = token_text(rest_tok);
    if (!rest) {
        rest = n00b_string_empty();
    }

    /* Collect <continuation> children. */
    enum { MAX_CONT = 1024 };
    n00b_parse_tree_t *conts[MAX_CONT];
    int n_cont = n00b_pt_collect_nt_deep(meta_field_node, "continuation",
                                         conts, MAX_CONT);
    if (n_cont == 0) {
        return rest;
    }

    /*
     * Build the joined string: REST + ("\n" + INDENT_LINE) per
     * continuation. If REST is empty, drop the empty prefix and
     * start with the first continuation's text.
     */
    n00b_buffer_t *acc = n00b_buffer_empty();
    if (rest->u8_bytes > 0) {
        n00b_buffer_t *r = n00b_buffer_from_bytes(rest->data,
                                                  (int64_t)rest->u8_bytes);
        n00b_buffer_concat(acc, r);
    }
    bool need_sep = (rest->u8_bytes > 0);
    for (int i = 0; i < n_cont; i++) {
        n00b_token_info_t *line_tok = nth_token_child(conts[i], 0);
        n00b_string_t     *line     = token_text(line_tok);
        if (!line) {
            line = n00b_string_empty();
        }
        if (need_sep) {
            n00b_buffer_t *nl = n00b_buffer_from_bytes((char *)"\n", 1);
            n00b_buffer_concat(acc, nl);
        }
        n00b_buffer_t *lb = n00b_buffer_from_bytes(line->data,
                                                   (int64_t)line->u8_bytes);
        n00b_buffer_concat(acc, lb);
        need_sep = true;
    }

    return n00b_string_from_raw(acc->data, (int64_t)acc->byte_len);
}

/* ---------------------------------------------------------------- */
/* Directive table walking                                          */
/* ---------------------------------------------------------------- */

/*
 * For a node that contains <meta_field> children (either <file_meta>
 * or <rule_section>), walk its meta_field children and call
 * `handler` once per directive. `handler` receives the directive
 * name + the assembled value + an opaque carry pointer. If
 * `handler` returns false, `walk_meta_fields` returns immediately
 * with false (the caller's signal for "schema error").
 */
typedef bool (*meta_field_handler_t)(n00b_string_t *name,
                                     n00b_string_t *value,
                                     void          *carry);

static bool
walk_meta_fields(n00b_parse_tree_t   *parent,
                 meta_field_handler_t handler,
                 void                *carry)
{
    enum { MAX_FIELDS = 4096 };
    n00b_parse_tree_t *fields[MAX_FIELDS];
    int n = n00b_pt_collect_nt_deep(parent, "meta_field",
                                    fields, MAX_FIELDS);
    for (int i = 0; i < n; i++) {
        n00b_token_info_t *name_tok = nth_token_child(fields[i], 0);
        n00b_string_t     *name     = token_text(name_tok);
        if (!name) {
            name = n00b_string_empty();
        }
        n00b_string_t *value = assemble_field_value(fields[i]);
        if (!handler(name, value, carry)) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* File-level field handler                                         */
/* ---------------------------------------------------------------- */

typedef struct {
    n00b_audit_guidance_t *g;
    int                    err_code;
    bool                   saw_schema_version;
} file_field_ctx_t;

/*
 * Parse a decimal integer string into int64_t. Returns true on
 * success, false on empty input or non-digit characters.
 */
static bool
parse_int64(n00b_string_t *s, int64_t *out)
{
    if (!s || s->u8_bytes == 0) {
        return false;
    }
    int64_t     v   = 0;
    size_t      n   = s->u8_bytes;
    const char *buf = s->data;
    size_t      i   = 0;
    bool        neg = false;
    if (buf[0] == '-') {
        neg = true;
        i   = 1;
    }
    if (i >= n) {
        return false;
    }
    for (; i < n; i++) {
        char c = buf[i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (int64_t)(c - '0');
    }
    *out = neg ? -v : v;
    return true;
}

static bool
file_field_handler(n00b_string_t *name, n00b_string_t *value, void *carry)
{
    file_field_ctx_t      *ctx = (file_field_ctx_t *)carry;
    n00b_audit_guidance_t *g   = ctx->g;

    if (str_eq_cstr(name, "schema_version")) {
        int64_t v = 0;
        if (!parse_int64(value, &v)) {
            ctx->err_code = N00B_AUDIT_ERR_GUIDANCE_SCHEMA;
            return false;
        }
        g->schema_version       = v;
        ctx->saw_schema_version = true;
        if (v != 1) {
            ctx->err_code = N00B_AUDIT_ERR_GUIDANCE_SCHEMA_VERSION;
            return false;
        }
        return true;
    }
    if (str_eq_cstr(name, "project")) {
        g->project = value;
        return true;
    }
    if (str_eq_cstr(name, "description")) {
        g->description = value;
        return true;
    }
    if (str_eq_cstr(name, "source_doc")) {
        g->source_doc = value;
        return true;
    }
    if (str_eq_cstr(name, "dependencies")) {
        /*
         * Dependencies value is a whitespace-separated list of
         * paths. Empty value → no dependencies.
         */
        if (!value || value->u8_bytes == 0) {
            return true;
        }
        const char *buf = value->data;
        size_t      n   = value->u8_bytes;
        size_t      i   = 0;
        while (i < n) {
            while (i < n && (buf[i] == ' ' || buf[i] == '\t'
                             || buf[i] == '\n')) {
                i++;
            }
            size_t start = i;
            while (i < n && !(buf[i] == ' ' || buf[i] == '\t'
                              || buf[i] == '\n')) {
                i++;
            }
            if (i > start) {
                n00b_list_push(*g->dependencies,
                               n00b_string_from_raw(buf + start,
                                                    (int64_t)(i - start)));
            }
        }
        return true;
    }
    /* Unknown directive: tolerated silently (forward-compat). */
    return true;
}

/* ---------------------------------------------------------------- */
/* Per-rule field handler                                           */
/* ---------------------------------------------------------------- */

typedef struct {
    n00b_audit_rule_t *rule;
    int                err_code;
} rule_field_ctx_t;

static bool
rule_field_handler(n00b_string_t *name, n00b_string_t *value, void *carry)
{
    rule_field_ctx_t  *ctx = (rule_field_ctx_t *)carry;
    n00b_audit_rule_t *r   = ctx->rule;

    if (str_eq_cstr(name, "title")) {
        r->title = value;
        return true;
    }
    if (str_eq_cstr(name, "section")) {
        r->section = value;
        return true;
    }
    if (str_eq_cstr(name, "violation_nt")) {
        r->violation_nt = value;
        return true;
    }
    if (str_eq_cstr(name, "rationale")) {
        r->rationale = value;
        return true;
    }
    if (str_eq_cstr(name, "bad")) {
        r->bad_example = value;
        return true;
    }
    if (str_eq_cstr(name, "good")) {
        r->good_example = value;
        return true;
    }
    if (str_eq_cstr(name, "guidance")) {
        r->guidance = value;
        return true;
    }
    if (str_eq_cstr(name, "applies_to.include")) {
        if (!r->applies_to_include) {
            r->applies_to_include = n00b_alloc(
                n00b_list_t(n00b_string_t *));
            *r->applies_to_include = n00b_list_new(n00b_string_t *);
        }
        n00b_list_push(*r->applies_to_include, value);
        return true;
    }
    if (str_eq_cstr(name, "applies_to.exclude")) {
        if (!r->applies_to_exclude) {
            r->applies_to_exclude = n00b_alloc(
                n00b_list_t(n00b_string_t *));
            *r->applies_to_exclude = n00b_list_new(n00b_string_t *);
        }
        n00b_list_push(*r->applies_to_exclude, value);
        return true;
    }
    /* Unknown directive: tolerated silently. */
    return true;
}

/* ---------------------------------------------------------------- */
/* BNF body extraction                                              */
/* ---------------------------------------------------------------- */

/*
 * Walk a <rule_section> node's <bnf_body> child, concatenating each
 * <bnf_line>'s BNF_LINE text into a single string separated by "\n"
 * with a trailing newline. The resulting string is suitable for
 * direct concatenation into the engine's combined-grammar text in
 * `src/audit/engine.c`.
 */
static n00b_string_t *
assemble_bnf_body(n00b_parse_tree_t *rule_section)
{
    enum { MAX_BNF_LINES = 65536 };
    n00b_parse_tree_t *lines[MAX_BNF_LINES];
    /*
     * <bnf_body> is a regular NT (not a `$$group_N` synthetic) so
     * `n00b_pt_collect_nt_deep` won't recurse into it from
     * `rule_section`. Find it explicitly first.
     */
    n00b_parse_tree_t *body = n00b_pt_find_child_by_nt(rule_section,
                                                      "bnf_body");
    int n = 0;
    if (body) {
        n = n00b_pt_collect_nt_deep(body, "bnf_line",
                                    lines, MAX_BNF_LINES);
    }
    if (n == 0) {
        return n00b_string_empty();
    }
    n00b_buffer_t *acc = n00b_buffer_empty();
    n00b_buffer_t *nl  = n00b_buffer_from_bytes((char *)"\n", 1);
    for (int i = 0; i < n; i++) {
        n00b_token_info_t *tok = nth_token_child(lines[i], 0);
        n00b_string_t     *t   = token_text(tok);
        if (!t) {
            t = n00b_string_empty();
        }
        n00b_buffer_t *lb = n00b_buffer_from_bytes(t->data,
                                                   (int64_t)t->u8_bytes);
        n00b_buffer_concat(acc, lb);
        n00b_buffer_concat(acc, nl);
    }
    return n00b_string_from_raw(acc->data, (int64_t)acc->byte_len);
}

/* ---------------------------------------------------------------- */
/* Metagrammar grammar (built once per load)                        */
/* ---------------------------------------------------------------- */

/*
 * Build the metagrammar grammar by loading the embedded
 * N00B_AUDIT_RULE_FILE_METAGRAMMAR text via `n00b_bnf_load`.
 * Returns nullptr on failure (the metagrammar is build-time-baked,
 * so this should be infallible at runtime; a failure here indicates
 * a regression in either the metagrammar text or the slay BNF
 * loader's behavior).
 */
static n00b_grammar_t *
build_metagrammar(void)
{
    n00b_grammar_t *g = n00b_grammar_new();
    n00b_string_t  *text =
        n00b_string_from_cstr(N00B_AUDIT_RULE_FILE_METAGRAMMAR);
    bool ok = n00b_bnf_load(text, r"file", g);
    if (!ok) {
        n00b_grammar_free(g);
        return nullptr;
    }
    return g;
}

/* ---------------------------------------------------------------- */
/* Public entry                                                     */
/* ---------------------------------------------------------------- */

n00b_result_t(n00b_audit_guidance_t *)
n00b_audit_load_guidance(n00b_string_t *path)
{
    if (!path) {
        return n00b_result_err(n00b_audit_guidance_t *,
                               N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND);
    }

    /* Canonicalize the input path: expand env-vars / `~`, root to
     * cwd if relative. Diagnostics and downstream file operations
     * then see a single absolute form regardless of caller cwd. */
    path = n00b_path_canonical(path);

    /* Step 1: open the file (MMAP substrate, one-shot read). */
    auto fr = n00b_file_open(path, .kind = N00B_FILE_KIND_MMAP);
    if (n00b_result_is_err(fr)) {
        return n00b_result_err(n00b_audit_guidance_t *,
                               N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND);
    }
    n00b_file_t *f  = n00b_result_get(fr);
    auto         br = n00b_file_as_buffer(f);
    n00b_file_close(f);
    if (n00b_result_is_err(br)) {
        return n00b_result_err(n00b_audit_guidance_t *,
                               N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND);
    }
    n00b_buffer_t *src_buf = n00b_result_get(br);

    /*
     * Step 2: build the metagrammar grammar from the embedded
     * `audit-rule-file.bnf` text.
     */
    n00b_grammar_t *meta_g = build_metagrammar();
    if (!meta_g) {
        return n00b_result_err(n00b_audit_guidance_t *,
                               N00B_AUDIT_ERR_GUIDANCE_PARSE);
    }

    /*
     * Step 3: set up the audit-rule-file tokenizer + token stream.
     * The scanner needs a private state struct (line classifier
     * state); we allocate it here and pass via .state. The .reset_cb
     * lets the scanner reinitialize the state on reset.
     */
    void *st = _n00b_audit_rule_file_scanner_state_new();
    n00b_scanner_t *sc = n00b_scanner_new(
        src_buf, _n00b_audit_rule_file_scan_cb(), meta_g,
        .state    = st,
        .reset_cb = _n00b_audit_rule_file_reset_cb());
    n00b_token_stream_t *ts = n00b_token_stream_new(sc);

    /* Step 4: parse. */
    n00b_parse_result_t *pr = n00b_grammar_parse(meta_g, ts,
                                                 N00B_PARSE_MODE_DEFAULT);
    if (!n00b_parse_result_ok(pr)) {
        n00b_parse_result_free(pr);
        n00b_grammar_free(meta_g);
        return n00b_result_err(n00b_audit_guidance_t *,
                               N00B_AUDIT_ERR_GUIDANCE_PARSE);
    }

    n00b_parse_tree_t *tree = n00b_parse_result_tree(pr);

    /*
     * Step 5: allocate the guidance struct. n00b_alloc zero-fills
     * the struct; we initialize the list fields explicitly to
     * non-null empty lists so the schema's contract holds (every
     * list field is always-non-null on success).
     */
    n00b_audit_guidance_t *g = n00b_alloc(n00b_audit_guidance_t);
    g->dependencies = n00b_alloc(n00b_list_t(n00b_string_t *));
    *g->dependencies = n00b_list_new(n00b_string_t *);
    g->rules = n00b_alloc(n00b_list_t(n00b_audit_rule_t *));
    *g->rules = n00b_list_new(n00b_audit_rule_t *);
    g->project     = n00b_string_empty();
    g->description = n00b_string_empty();
    g->source_doc  = n00b_string_empty();

    /*
     * Step 6: walk file-level <file_meta> directives. The tree
     * shape is:
     *   <file>
     *     <file_meta>
     *       <meta_field>*
     *     <rule_section>*
     */
    n00b_parse_tree_t *file_meta = n00b_pt_find_child_by_nt(tree,
                                                            "file_meta");
    file_field_ctx_t fctx = {.g = g, .err_code = 0, .saw_schema_version = false};
    if (file_meta) {
        if (!walk_meta_fields(file_meta, file_field_handler, &fctx)) {
            int code = fctx.err_code ? fctx.err_code
                                     : N00B_AUDIT_ERR_GUIDANCE_SCHEMA;
            n00b_parse_result_free(pr);
            n00b_grammar_free(meta_g);
            return n00b_result_err(n00b_audit_guidance_t *, code);
        }
    }
    if (!fctx.saw_schema_version) {
        n00b_parse_result_free(pr);
        n00b_grammar_free(meta_g);
        return n00b_result_err(n00b_audit_guidance_t *,
                               N00B_AUDIT_ERR_GUIDANCE_SCHEMA);
    }

    /* Step 7: walk per-rule <rule_section> children. */
    enum { MAX_RULES = 4096 };
    n00b_parse_tree_t *sections[MAX_RULES];
    int n_sections = n00b_pt_collect_nt_deep(tree, "rule_section",
                                             sections, MAX_RULES);
    for (int i = 0; i < n_sections; i++) {
        n00b_parse_tree_t *sec = sections[i];

        /*
         * <rule_section>'s first child is <rule_marker>, whose first
         * token leaf is the RULE_MARKER carrying the rule id text.
         */
        n00b_parse_tree_t *marker = n00b_pt_find_child_by_nt(sec,
                                                             "rule_marker");
        n00b_token_info_t *id_tok = nth_token_child(marker, 0);
        n00b_string_t     *id     = token_text(id_tok);
        if (!id || id->u8_bytes == 0) {
            n00b_parse_result_free(pr);
            n00b_grammar_free(meta_g);
            return n00b_result_err(n00b_audit_guidance_t *,
                                   N00B_AUDIT_ERR_GUIDANCE_SCHEMA);
        }

        n00b_audit_rule_t *rule = n00b_alloc(n00b_audit_rule_t);
        rule->id                = id;
        rule->title             = n00b_string_empty();
        rule->section           = n00b_string_empty();
        rule->bnf_fragment      = n00b_string_empty();
        rule->violation_nt      = n00b_string_empty();
        rule->rationale         = n00b_string_empty();
        rule->bad_example       = n00b_string_empty();
        rule->good_example      = n00b_string_empty();
        rule->guidance          = n00b_string_empty();
        rule->applies_to_include = nullptr;
        rule->applies_to_exclude = nullptr;

        rule_field_ctx_t rctx = {.rule = rule, .err_code = 0};
        if (!walk_meta_fields(sec, rule_field_handler, &rctx)) {
            int code = rctx.err_code ? rctx.err_code
                                     : N00B_AUDIT_ERR_GUIDANCE_SCHEMA;
            n00b_parse_result_free(pr);
            n00b_grammar_free(meta_g);
            return n00b_result_err(n00b_audit_guidance_t *, code);
        }

        /* Assemble the rule's BNF body. */
        rule->bnf_fragment = assemble_bnf_body(sec);

        /*
         * Schema check: required fields must be non-empty. Match
         * the Phase 2 loader's per-rule required set + add
         * violation_nt + bnf_fragment.
         */
        if (rule->title->u8_bytes == 0
            || rule->section->u8_bytes == 0
            || rule->violation_nt->u8_bytes == 0
            || rule->rationale->u8_bytes == 0
            || rule->bad_example->u8_bytes == 0
            || rule->good_example->u8_bytes == 0
            || rule->guidance->u8_bytes == 0
            || rule->bnf_fragment->u8_bytes == 0) {
            n00b_parse_result_free(pr);
            n00b_grammar_free(meta_g);
            return n00b_result_err(n00b_audit_guidance_t *,
                                   N00B_AUDIT_ERR_GUIDANCE_SCHEMA);
        }

        n00b_list_push(*g->rules, rule);
    }

    /*
     * Step 8: dependency walk. WP-001 supports only the empty-list
     * path. Non-empty lists return DEPS_UNIMPLEMENTED (DF-B).
     */
    if (n00b_list_len(*g->dependencies) > 0) {
        n00b_parse_result_free(pr);
        n00b_grammar_free(meta_g);
        return n00b_result_err(n00b_audit_guidance_t *,
                               N00B_AUDIT_ERR_GUIDANCE_DEPS_UNIMPLEMENTED);
    }

    n00b_parse_result_free(pr);
    n00b_grammar_free(meta_g);
    return n00b_result_ok(n00b_audit_guidance_t *, g);
}

/* ---------------------------------------------------------------- */
/* Guidance-file discovery walk                                     */
/* ---------------------------------------------------------------- */

/*
 * Compute the parent of `dir` using libn00b's path primitives.
 * Same shape as the prior loader's parent_dir helper. Terminates
 * on filesystem root via parent-equals-self.
 */
static n00b_string_t *
parent_dir(n00b_string_t *dir)
{
    n00b_list_t(n00b_string_t *) *parts = n00b_path_parts(dir);
    if (!parts || n00b_list_len(*parts) <= 1) {
        return dir;
    }
    n00b_list_t(n00b_string_t *) *up = n00b_alloc(
        n00b_list_t(n00b_string_t *));
    *up      = n00b_list_new(n00b_string_t *);
    size_t n = n00b_list_len(*parts);
    for (size_t i = 0; i + 1 < n; i++) {
        n00b_list_push(*up, n00b_list_get(*parts, i));
    }
    if (n00b_list_len(*up) == 0) {
        return n00b_string_from_cstr("/");
    }
    if (n00b_list_len(*up) == 1) {
        n00b_string_t *only = n00b_list_get(*up, 0);
        if (!only || only->u8_bytes == 0) {
            return n00b_string_from_cstr("/");
        }
    }
    return n00b_path_join(up);
}

n00b_result_t(n00b_string_t *)
n00b_audit_find_guidance_file(n00b_string_t *start_dir)
{
    if (!start_dir) {
        return n00b_result_err(n00b_string_t *,
                               N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND);
    }

    n00b_string_t *dir = n00b_path_canonical(start_dir);
    if (!dir || dir->u8_bytes == 0) {
        return n00b_result_err(n00b_string_t *,
                               N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND);
    }

    /*
     * Walk up one parent at a time. At each level, build
     * `<dir>/audit-rules.bnf` (WP-005 — was previously
     * `<dir>/.agents/.audit-guidance.json`) and test for
     * existence via `n00b_path_is_file`.
     */
    for (;;) {
        n00b_string_t *candidate = n00b_path_simple_join(
            dir, n00b_string_from_cstr("audit-rules.bnf"));
        if (n00b_path_is_file(candidate)) {
            return n00b_result_ok(n00b_string_t *, candidate);
        }

        n00b_string_t *up = parent_dir(dir);
        if (!up || up->u8_bytes == 0 || n00b_unicode_str_eq(up, dir)) {
            return n00b_result_err(n00b_string_t *,
                                   N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND);
        }
        dir = up;
    }
}

n00b_string_t *
n00b_audit_err_str(int code)
{
    switch (code) {
    case N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND:
        return r"guidance file not found or unreadable";
    case N00B_AUDIT_ERR_GUIDANCE_PARSE:
        return r"audit-rule file failed to parse";
    case N00B_AUDIT_ERR_GUIDANCE_SCHEMA:
        return r"audit-rule file does not match the v1 schema";
    case N00B_AUDIT_ERR_GUIDANCE_SCHEMA_VERSION:
        return r"audit-rule file schema_version is not 1";
    case N00B_AUDIT_ERR_GUIDANCE_DEPS_UNIMPLEMENTED:
        return r"non-empty dependencies list is not yet supported (deferred to WP-002+)";
    case N00B_AUDIT_ERR_ENGINE_GRAMMAR_LOAD:
        return r"audit engine could not load the base C-ncc grammar";
    case N00B_AUDIT_ERR_ENGINE_RULE_MERGE:
        return r"a rule's BNF fragment failed to merge with the base grammar";
    case N00B_AUDIT_ERR_ENGINE_PARSE:
        return r"target source file failed to parse against the merged grammar";
    case N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND:
        return r"target source file not found or unreadable";
    case N00B_AUDIT_ERR_ENGINE_BAD_ARGS:
        return r"required engine argument was null or uninitialized";
    case N00B_AUDIT_ERR_CLI_ARGS:
        return r"CLI argument parsing failed (unknown flag, missing positional, or bad value)";
    case N00B_AUDIT_ERR_CLI_BAD_ARGS:
        return r"required argument to n00b_audit_run_cli was null or out-of-shape";
    case N00B_AUDIT_ERR_CLI_RENDER:
        return r"output renderer could not produce a violation block (missing required rule field)";
    default:
        return r"(unknown n00b-audit error code)";
    }
}
