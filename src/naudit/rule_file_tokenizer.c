/*
 * WP-005 — custom tokenizer for the audit-rule-file format.
 *
 * Registers the `"audit_rule_file"` scan callback with libn00b's
 * tokenizer registry; the registered callback is consumed by the
 * Phase 2 loader (`n00b_audit_load_guidance`) via slay's standard
 * scanner → token_stream → grammar_parse pipeline.
 *
 * # DF-I resolution — tokenizer design
 *
 * Design selected: **state-machine line classifier.** Each callback
 * invocation processes one line edge at a time:
 *
 *   1. At the start of a line we peek the leading codepoint(s) to
 *      classify the line:
 *        - empty line              → emit NEWLINE, return true
 *        - leading `#`             → skip rest of line as trivia,
 *                                    emit NEWLINE, return true
 *        - `@rule` + whitespace    → emit RULE_MARKER (text = the
 *                                    rule id, i.e. rest of line
 *                                    trimmed), then transition to
 *                                    `STATE_EMIT_NEWLINE`
 *        - `@<word>`               → emit DIRECTIVE (text = the
 *                                    directive name), then transition
 *                                    to `STATE_EMIT_REST`
 *        - leading space or tab    → emit INDENT_LINE (text = trimmed
 *                                    line), then `STATE_EMIT_NEWLINE`
 *        - anything else           → emit BNF_LINE (text = entire
 *                                    line), then `STATE_EMIT_NEWLINE`
 *
 *   2. After a DIRECTIVE token, the next emission is REST (the bytes
 *      after the directive word + one separating space, trimmed of
 *      trailing whitespace). After REST, emit NEWLINE.
 *
 *   3. After a RULE_MARKER / INDENT_LINE / BNF_LINE token, the next
 *      emission is NEWLINE.
 *
 * Rationale for the line-classifier shape (vs. fully-tokenized,
 * vs. raw-line-text-and-parser-side-classify):
 *
 *   - The format's lexical structure is fundamentally line-oriented;
 *     classifying line-kind during scanning maps directly onto the
 *     metagrammar's per-line tokens and keeps the metagrammar
 *     trivial.
 *   - The alternative "raw line tokens, parser classifies" leaves
 *     the metagrammar with no way to discriminate `@rule` from a
 *     generic directive line without re-tokenizing each line's
 *     text downstream — extra work for no benefit.
 *   - A full character-level tokenizer (LANGLE, ASSIGN, …) would
 *     conflict with slay's BNF tokenizer's token IDs and force us
 *     to re-state slay's BNF in the metagrammar; the preflight
 *     authorized the opaque "raw BNF lines" approach to avoid this.
 *
 * # Token IDs
 *
 * Tokens are emitted via `n00b_scan_emit(.token_type = "NAME")` so
 * slay's BNF loader resolves them via the grammar's
 * `literal_type_map`. The metagrammar at `grammars/audit-rule-file.bnf`
 * references these as `%RULE_MARKER`, `%DIRECTIVE`, `%REST`,
 * `%INDENT_LINE`, `%BNF_LINE`, `%NEWLINE` — the BNF loader
 * registers each of those names as a literal type when it processes
 * the `%TYPE` atoms in the grammar.
 *
 * # Lifecycle + registration
 *
 * `n00b_audit_register_rule_file_tokenizer()` is invoked once from
 * `n00b_audit_module_init` (see src/audit/module.c). Registering
 * twice (e.g., on test re-entry) is safe — `n00b_tokenizer_register`
 * is idempotent (overwrites the prior callback).
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "parsers/scanner.h"
#include "parsers/tokenizer_registry.h"

#include "internal/naudit/_naudit_internal.h"

/* ---------------------------------------------------------------- */
/* Tokenizer state machine                                          */
/* ---------------------------------------------------------------- */

typedef enum {
    RULE_FILE_STATE_LINE_START = 0,
    RULE_FILE_STATE_EMIT_REST,
    RULE_FILE_STATE_EMIT_NEWLINE,
    RULE_FILE_STATE_DONE,
} rule_file_state_kind_t;

/*
 * Persistent state threaded through scanner callbacks. The scanner
 * borrows this via its `user_state` slot; the loader allocates it
 * with `n00b_alloc` and the scanner zeroes the relevant fields via
 * the `reset_cb`.
 *
 * Fields:
 *   - `state`           the state machine's current state.
 *   - `pending_rest`    after emitting DIRECTIVE we stash the rest-of-
 *                       line text here so the next callback can emit
 *                       it as REST.
 */
typedef struct {
    rule_file_state_kind_t state;
    n00b_string_t         *pending_rest;
} rule_file_scanner_state_t;

/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

static bool
is_line_terminator(n00b_codepoint_t cp)
{
    return cp == '\n' || cp == '\r' || cp == 0;
}

static bool
is_indent_char(n00b_codepoint_t cp)
{
    return cp == ' ' || cp == '\t';
}

/*
 * Advance past one line terminator: `\n`, `\r`, or `\r\n`. Caller
 * has positioned the cursor at the terminator codepoint; on return
 * the cursor sits at the start of the next line (or EOF).
 */
static void
consume_line_terminator(n00b_scanner_t *s)
{
    if (n00b_scan_at_eof(s)) {
        return;
    }
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);
    if (cp == '\r') {
        n00b_scan_advance(s);
        if (!n00b_scan_at_eof(s) && n00b_scan_peek(s, 0) == '\n') {
            n00b_scan_advance(s);
        }
        return;
    }
    if (cp == '\n') {
        n00b_scan_advance(s);
    }
}

/*
 * Read the line content from the current cursor up to (but not
 * including) the line terminator, returning it as a fresh n00b
 * string. The cursor is left at the terminator codepoint.
 */
static n00b_string_t *
extract_to_eol(n00b_scanner_t *s)
{
    n00b_scan_mark(s);
    while (!n00b_scan_at_eof(s)) {
        n00b_codepoint_t cp = n00b_scan_peek(s, 0);
        if (is_line_terminator(cp)) {
            break;
        }
        n00b_scan_advance(s);
    }
    return n00b_scan_extract(s);
}

/*
 * Trim trailing whitespace (spaces, tabs) off an n00b string.
 * Returns a fresh string (or the same one if there's no trim
 * needed). Empty input returns the empty string.
 */
static n00b_string_t *
trim_trailing_ws(n00b_string_t *s)
{
    if (!s) {
        return n00b_string_empty();
    }
    size_t      n   = s->u8_bytes;
    const char *buf = s->data;
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t'
                     || buf[n - 1] == '\r')) {
        n--;
    }
    if (n == s->u8_bytes) {
        return s;
    }
    return n00b_string_from_raw(buf, (int64_t)n);
}

/*
 * Trim leading whitespace (spaces, tabs) off an n00b string.
 */
static n00b_string_t *
trim_leading_ws(n00b_string_t *s)
{
    if (!s) {
        return n00b_string_empty();
    }
    size_t      n   = s->u8_bytes;
    const char *buf = s->data;
    size_t      i   = 0;
    while (i < n && (buf[i] == ' ' || buf[i] == '\t')) {
        i++;
    }
    if (i == 0) {
        return s;
    }
    return n00b_string_from_raw(buf + i, (int64_t)(n - i));
}

/*
 * Split a directive line's content (the bytes AFTER the leading
 * `@`) into (directive_name, rest). The directive name runs from
 * the start until the first whitespace; `rest` is the bytes after
 * that, leading whitespace stripped, trailing whitespace stripped.
 * Empty rest is fine — `pending_rest` is set to the empty string
 * so the REST token always gets emitted.
 */
static void
split_directive_line(n00b_string_t *line,
                     n00b_string_t **name_out,
                     n00b_string_t **rest_out)
{
    size_t      n   = line ? line->u8_bytes : 0;
    const char *buf = line ? line->data : "";
    size_t      i   = 0;
    while (i < n && !(buf[i] == ' ' || buf[i] == '\t')) {
        i++;
    }
    *name_out = n00b_string_from_raw(buf, (int64_t)i);
    size_t j  = i;
    while (j < n && (buf[j] == ' ' || buf[j] == '\t')) {
        j++;
    }
    n00b_string_t *raw_rest = n00b_string_from_raw(buf + j,
                                                   (int64_t)(n - j));
    *rest_out = trim_trailing_ws(raw_rest);
}

/* ---------------------------------------------------------------- */
/* Per-state handlers                                               */
/* ---------------------------------------------------------------- */

static bool
emit_pending_rest(n00b_scanner_t *s, rule_file_scanner_state_t *st)
{
    n00b_string_t *rest = st->pending_rest ? st->pending_rest
                                            : n00b_string_empty();
    st->pending_rest = nullptr;
    st->state        = RULE_FILE_STATE_EMIT_NEWLINE;
    n00b_scan_emit(s, .token_type = "REST",
                   .contents = n00b_option_set(n00b_string_t *, rest));
    return true;
}

static bool
emit_pending_newline(n00b_scanner_t *s, rule_file_scanner_state_t *st)
{
    consume_line_terminator(s);
    st->state = RULE_FILE_STATE_LINE_START;
    n00b_scan_emit(s, .token_type = "NEWLINE",
                   .contents = n00b_option_set(n00b_string_t *,
                                               n00b_string_empty()));
    return true;
}

static bool
classify_line_start(n00b_scanner_t *s, rule_file_scanner_state_t *st)
{
    /* Skip any number of empty / comment lines up front. */
    for (;;) {
        if (n00b_scan_at_eof(s)) {
            st->state = RULE_FILE_STATE_DONE;
            return false;
        }
        n00b_codepoint_t cp = n00b_scan_peek(s, 0);
        if (cp == '\n' || cp == '\r') {
            /* Empty line: skip the terminator and continue. */
            consume_line_terminator(s);
            continue;
        }
        if (cp == '#') {
            /* Line comment: skip to end of line and the terminator. */
            while (!n00b_scan_at_eof(s)) {
                n00b_codepoint_t c = n00b_scan_peek(s, 0);
                if (is_line_terminator(c)) {
                    break;
                }
                n00b_scan_advance(s);
            }
            consume_line_terminator(s);
            continue;
        }
        break;
    }

    /* Now at the first byte of a non-empty, non-comment line. */
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    if (cp == '@') {
        /* Read the whole line, then split off the directive word. */
        n00b_scan_advance(s); /* skip '@' */
        n00b_string_t *line = extract_to_eol(s);
        line                = trim_trailing_ws(line);

        n00b_string_t *name = nullptr;
        n00b_string_t *rest = nullptr;
        split_directive_line(line, &name, &rest);

        /*
         * Section markers: `@rule <id>` for guidance files and
         * `@exemption <id>` for WP-011 exemption / baseline files.
         * Both emit a RULE_MARKER token carrying the id text so the
         * shared metagrammar's `<rule_section>` reduction succeeds
         * against either kind of file. The loader downstream knows
         * which kind of file it's parsing from context (which entry
         * function was invoked) and interprets the section's
         * `<meta_field>` children accordingly.
         */
        bool is_section_marker = false;
        if (name && name->u8_bytes == 4 && name->data[0] == 'r'
            && name->data[1] == 'u' && name->data[2] == 'l'
            && name->data[3] == 'e') {
            is_section_marker = true;
        }
        else if (name && name->u8_bytes == 9
                 && name->data[0] == 'e' && name->data[1] == 'x'
                 && name->data[2] == 'e' && name->data[3] == 'm'
                 && name->data[4] == 'p' && name->data[5] == 't'
                 && name->data[6] == 'i' && name->data[7] == 'o'
                 && name->data[8] == 'n') {
            is_section_marker = true;
        }
        if (is_section_marker) {
            /*
             * `@rule <id>` or `@exemption <id>` — emit RULE_MARKER
             * carrying the id. The REST text becomes the token's
             * text payload (so the loader's tree walk pulls it off
             * the RULE_MARKER node directly rather than waiting for
             * a separate REST token).
             */
            st->state = RULE_FILE_STATE_EMIT_NEWLINE;
            n00b_scan_emit(s, .token_type = "RULE_MARKER",
                           .contents = n00b_option_set(n00b_string_t *,
                                                       rest));
            return true;
        }

        /*
         * Other directive: emit DIRECTIVE with the name, stash REST
         * for the next callback.
         */
        st->pending_rest = rest;
        st->state        = RULE_FILE_STATE_EMIT_REST;
        n00b_scan_emit(s, .token_type = "DIRECTIVE",
                       .contents = n00b_option_set(n00b_string_t *,
                                                   name));
        return true;
    }

    if (is_indent_char(cp)) {
        /* Continuation line. */
        n00b_string_t *line = extract_to_eol(s);
        line                = trim_trailing_ws(trim_leading_ws(line));
        st->state           = RULE_FILE_STATE_EMIT_NEWLINE;
        n00b_scan_emit(s, .token_type = "INDENT_LINE",
                       .contents = n00b_option_set(n00b_string_t *,
                                                   line));
        return true;
    }

    /* Everything else: BNF body line. Preserve whole line verbatim. */
    n00b_string_t *line = extract_to_eol(s);
    line                = trim_trailing_ws(line);
    st->state           = RULE_FILE_STATE_EMIT_NEWLINE;
    n00b_scan_emit(s, .token_type = "BNF_LINE",
                   .contents = n00b_option_set(n00b_string_t *, line));
    return true;
}

/* ---------------------------------------------------------------- */
/* Scanner callback (registered with the tokenizer registry)        */
/* ---------------------------------------------------------------- */

static bool
audit_rule_file_scan(n00b_scanner_t *s)
{
    rule_file_scanner_state_t *st =
        (rule_file_scanner_state_t *)s->user_state;
    if (!st) {
        return false;
    }
    if (st->state == RULE_FILE_STATE_DONE) {
        return false;
    }
    if (st->state == RULE_FILE_STATE_EMIT_REST) {
        return emit_pending_rest(s, st);
    }
    if (st->state == RULE_FILE_STATE_EMIT_NEWLINE) {
        return emit_pending_newline(s, st);
    }
    return classify_line_start(s, st);
}

/* ---------------------------------------------------------------- */
/* Reset callback (called by n00b_scanner_reset)                    */
/* ---------------------------------------------------------------- */

static void
audit_rule_file_reset(n00b_scanner_t *s)
{
    rule_file_scanner_state_t *st =
        (rule_file_scanner_state_t *)s->user_state;
    if (!st) {
        return;
    }
    st->state        = RULE_FILE_STATE_LINE_START;
    st->pending_rest = nullptr;
}

/* ---------------------------------------------------------------- */
/* Internal-only allocator for the state struct                     */
/* ---------------------------------------------------------------- */

void *
_n00b_audit_rule_file_scanner_state_new(void)
{
    rule_file_scanner_state_t *st =
        n00b_alloc(rule_file_scanner_state_t);
    st->state        = RULE_FILE_STATE_LINE_START;
    st->pending_rest = nullptr;
    return st;
}

n00b_scan_cb_t
_n00b_audit_rule_file_scan_cb(void)
{
    return audit_rule_file_scan;
}

n00b_scan_reset_cb_t
_n00b_audit_rule_file_reset_cb(void)
{
    return audit_rule_file_reset;
}

/* ---------------------------------------------------------------- */
/* Registration                                                     */
/* ---------------------------------------------------------------- */

void
n00b_audit_register_rule_file_tokenizer(void)
{
    n00b_tokenizer_register("audit_rule_file", audit_rule_file_scan);
}
