#include "scanner/lex.h"
#include "private/parse/grammar.h"
#include "private/scanner/lex_program.h"
#include "lib/alloc.h"
#include "unicode/encoding.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    core_string_t        spelling;
    core_string_t        origin;
    scan_lex_decl_kind_t kind;
    uint8_t              priority;
    bool                 committing;
} lex_opening_t;

typedef struct {
    size_t  opening;
    int32_t previous_at_priority;
    bool    tracked;
} lex_prefix_frame_t;

typedef struct {
    core_string_t token;
    core_string_t origin;
} lex_output_t;

typedef struct {
    scan_lex_decl_kind_t kind;
    core_string_t        spelling;
    core_string_t        origin;
    size_t               used;
} lex_decl_origin_t;

enum {
    PRIORITY_EMBED,
    PRIORITY_COMMENT,
    PRIORITY_DIRECTIVE,
    PRIORITY_HEREDOC,
    PRIORITY_FENCE,
    PRIORITY_STRING,
    PRIORITY_NUMBER,
    PRIORITY_SIGIL,
    PRIORITY_REGEX,
    PRIORITY_FIXED,
    PRIORITY_COUNT,
};

static const char *
kind_name(scan_lex_decl_kind_t kind)
{
    switch (kind) {
    case SCAN_LEX_DECL_COMMENT: return "comment";
    case SCAN_LEX_DECL_STRING: return "string";
    case SCAN_LEX_DECL_NUMBER: return "number";
    case SCAN_LEX_DECL_SIGIL: return "sigil";
    case SCAN_LEX_DECL_REGEX: return "regular-expression";
    case SCAN_LEX_DECL_HEREDOC: return "heredoc";
    case SCAN_LEX_DECL_EMBED: return "embedded-language";
    case SCAN_LEX_DECL_FENCE: return "fence";
    case SCAN_LEX_DECL_DIRECTIVE: return "directive";
    case SCAN_LEX_DECL_OPERATOR: return "operator";
    case SCAN_LEX_DECL_SOFT_KEYWORD: return "soft-keyword";
    case SCAN_LEX_DECL_LITERAL_HOOK: return "literal-hook";
    case SCAN_LEX_DECL_RECOVERY: return "recovery";
    case SCAN_LEX_DECL_NONE: return "fixed-terminal";
    }
    return "token";
}

static int
string_compare(core_string_t a, core_string_t b)
{
    size_t common = a.u8_bytes < b.u8_bytes ? a.u8_bytes : b.u8_bytes;
    int order = common ? memcmp(a.data, b.data, common) : 0;
    if (order) {
        return order;
    }
    return (a.u8_bytes > b.u8_bytes) - (a.u8_bytes < b.u8_bytes);
}

static int
declaration_compare(const void *left, const void *right)
{
    const lex_decl_origin_t *a = left;
    const lex_decl_origin_t *b = right;
    if (a->kind != b->kind) {
        return (a->kind > b->kind) - (a->kind < b->kind);
    }
    int order = string_compare(a->spelling, b->spelling);
    return order ? order : string_compare(a->origin, b->origin);
}

static lex_decl_origin_t *
declaration_index(const scan_lex_spec_t *spec, size_t *count)
{
    *count = 0;
    for (const scan_lex_declaration_t *item = spec->declarations;
         item && item->kind != SCAN_LEX_DECL_NONE; item++) {
        (*count)++;
    }
    if (!*count) {
        return nullptr;
    }
    lex_decl_origin_t *index
        = core_alloc_array(lex_decl_origin_t, *count);
    for (size_t i = 0; i < *count; i++) {
        index[i] = (lex_decl_origin_t){
            .kind = spec->declarations[i].kind,
            .spelling = spec->declarations[i].spelling,
            .origin = spec->declarations[i].origin,
        };
    }
    qsort(index, *count, sizeof(*index), declaration_compare);
    return index;
}

static core_string_t
lookup_origin(lex_decl_origin_t *index, size_t count,
              scan_lex_decl_kind_t kind, core_string_t spelling,
              bool consume)
{
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int order = index[mid].kind == kind
                  ? string_compare(index[mid].spelling, spelling)
                  : (index[mid].kind > kind ? 1 : -1);
        if (order < 0) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }
    if (lo == count || index[lo].kind != kind
        || !core_string_eq(index[lo].spelling, spelling)) {
        return (core_string_t){0};
    }
    size_t at = lo + (consume ? index[lo].used++ : 0);
    if (at >= count || index[at].kind != kind
        || !core_string_eq(index[at].spelling, spelling)) {
        at = lo;
    }
    return index[at].origin;
}

static void
add_opening(lex_opening_t *items, size_t *count,
            lex_decl_origin_t *origins, size_t origin_count,
            scan_lex_decl_kind_t kind,
            uint8_t priority, bool committing, core_string_t spelling)
{
    if (!spelling.data || !spelling.u8_bytes) {
        return;
    }
    items[(*count)++] = (lex_opening_t){
        .spelling = spelling,
        .origin = lookup_origin(origins, origin_count, kind, spelling, true),
        .kind = kind,
        .priority = priority,
        .committing = committing,
    };
}

static bool
string_array_contains(const core_string_t *items, core_string_t value)
{
    for (; items && items->data; items++) {
        if (core_string_eq(*items, value)) {
            return true;
        }
    }
    return false;
}

static size_t
recipe_count(const void *items, size_t stride, size_t spelling_offset)
{
    size_t count = 0;
    if (!items) {
        return 0;
    }
    for (;;) {
        const core_string_t *spelling = (const core_string_t *)(
            (const char *)items + count * stride + spelling_offset);
        if (!spelling->data) {
            return count;
        }
        count++;
    }
}

static int
int32_compare(const void *left, const void *right)
{
    int32_t a = *(const int32_t *)left;
    int32_t b = *(const int32_t *)right;
    return (a > b) - (a < b);
}

static int
opening_compare(const void *left, const void *right)
{
    const lex_opening_t *a = left;
    const lex_opening_t *b = right;
    size_t common = a->spelling.u8_bytes < b->spelling.u8_bytes
                  ? a->spelling.u8_bytes : b->spelling.u8_bytes;
    int order = memcmp(a->spelling.data, b->spelling.data, common);

    if (order) {
        return order;
    }
    if (a->spelling.u8_bytes != b->spelling.u8_bytes) {
        return a->spelling.u8_bytes < b->spelling.u8_bytes ? -1 : 1;
    }
    if (a->priority != b->priority) {
        return (a->priority > b->priority) - (a->priority < b->priority);
    }
    if (a->kind != b->kind) {
        return (a->kind > b->kind) - (a->kind < b->kind);
    }
    common = a->origin.u8_bytes < b->origin.u8_bytes
           ? a->origin.u8_bytes : b->origin.u8_bytes;
    order = common ? memcmp(a->origin.data, b->origin.data, common) : 0;
    if (order) {
        return order;
    }
    return (a->origin.u8_bytes > b->origin.u8_bytes)
         - (a->origin.u8_bytes < b->origin.u8_bytes);
}

static size_t
common_prefix(core_string_t left, core_string_t right)
{
    size_t limit = left.u8_bytes < right.u8_bytes
                 ? left.u8_bytes : right.u8_bytes;
    size_t length = 0;

    while (length < limit && left.data[length] == right.data[length]) {
        length++;
    }
    return length;
}

static void
format_declaration(char *out, size_t bytes, const lex_opening_t *item)
{
    if (item->origin.data) {
        snprintf(out, bytes, "%s '%.*s' declared in '%.*s'",
                 kind_name(item->kind),
                 (int)item->spelling.u8_bytes, item->spelling.data,
                 (int)item->origin.u8_bytes, item->origin.data);
    }
    else {
        snprintf(out, bytes, "%s '%.*s'",
                 kind_name(item->kind),
                 (int)item->spelling.u8_bytes, item->spelling.data);
    }
}

static bool
analyze_openings(const scan_lex_spec_t *spec, parse_grammar_t *grammar,
                 lex_decl_origin_t *origins, size_t origin_count,
                 char *error, size_t error_bytes,
                 scan_lex_issue_t **issues, size_t *issue_count)
{
    size_t codepoint_count = 0;
    for (size_t i = 0; i < grammar->rules.len; i++) {
        const parse_rule_t *rule = &grammar->rules.data[i];
        for (size_t j = 0; j < rule->contents.len; j++) {
            const parse_match_t *match = &rule->contents.data[j];
            if (match->kind == PARSE_MATCH_TERMINAL
                && match->terminal_id > 0
                && match->terminal_id < PARSE_TOK_START_ID) {
                codepoint_count++;
            }
        }
    }
    size_t capacity = grammar->named_terms.len + codepoint_count;
#define COUNT(field, type, member)                                             \
    capacity += recipe_count(spec->field, sizeof(type), offsetof(type, member))
    COUNT(comments, scan_lex_comment_t, opener);
    COUNT(strings, scan_lex_string_t, opener);
    COUNT(sigils, scan_lex_sigil_t, prefix);
    COUNT(regexes, scan_lex_regex_t, opener);
    COUNT(heredocs, scan_lex_heredoc_t, opener);
    COUNT(embeds, scan_lex_embed_t, open);
    COUNT(fences, scan_lex_fence_t, outer_open);
    COUNT(directives, scan_lex_directive_t, open);
    COUNT(number_forms, scan_lex_number_form_t, digits);
    capacity += recipe_count(spec->number.radixes, sizeof(scan_lex_radix_t),
                             offsetof(scan_lex_radix_t, prefix));
    capacity += recipe_count(spec->operators, sizeof(core_string_t), 0);
#undef COUNT

    lex_opening_t *items = core_alloc_array(lex_opening_t,
                                             capacity ? capacity : 1);
    size_t count = 0;
    int32_t *codepoints = core_alloc_array(int32_t,
                                            codepoint_count
                                                ? codepoint_count : 1);
    size_t codepoint_at = 0;
    for (size_t i = 0; i < grammar->rules.len; i++) {
        const parse_rule_t *rule = &grammar->rules.data[i];
        for (size_t j = 0; j < rule->contents.len; j++) {
            const parse_match_t *match = &rule->contents.data[j];
            if (match->kind == PARSE_MATCH_TERMINAL
                && match->terminal_id > 0
                && match->terminal_id < PARSE_TOK_START_ID) {
                codepoints[codepoint_at++] = (int32_t)match->terminal_id;
            }
        }
    }
    qsort(codepoints, codepoint_at, sizeof(*codepoints), int32_compare);
    char *codepoint_spellings = core_alloc_size(
        codepoint_at ? codepoint_at : 1, 4);
    size_t unique_codepoints = 0;
    for (size_t i = 0; i < codepoint_at; i++) {
        if (i && codepoints[i] == codepoints[i - 1]) {
            continue;
        }
        char *bytes = codepoint_spellings + unique_codepoints * 4;
        uint32_t width = unicode_utf8_encode((core_codepoint_t)codepoints[i],
                                             bytes);
        core_string_t spelling = core_string_borrow(bytes, width);
        unique_codepoints++;
        if (!string_array_contains(spec->operators, spelling)) {
            add_opening(items, &count, origins, origin_count,
                        SCAN_LEX_DECL_NONE, PRIORITY_FIXED, true, spelling);
        }
    }
    core_free(codepoints);
#define ADD_RECIPES(field, type, member, kind_value, priority_value, commits)  \
    do {                                                                       \
        for (const type *_item = spec->field;                                 \
             _item && _item->member.data; _item++) {                          \
            add_opening(items, &count, origins, origin_count, kind_value,    \
                        priority_value, commits,                              \
                        _item->member);                                        \
        }                                                                      \
    } while (0)
    ADD_RECIPES(embeds, scan_lex_embed_t, open,
                SCAN_LEX_DECL_EMBED, PRIORITY_EMBED, true);
    ADD_RECIPES(comments, scan_lex_comment_t, opener,
                SCAN_LEX_DECL_COMMENT, PRIORITY_COMMENT, true);
    ADD_RECIPES(directives, scan_lex_directive_t, open,
                SCAN_LEX_DECL_DIRECTIVE, PRIORITY_DIRECTIVE, false);
    ADD_RECIPES(heredocs, scan_lex_heredoc_t, opener,
                SCAN_LEX_DECL_HEREDOC, PRIORITY_HEREDOC, true);
    ADD_RECIPES(fences, scan_lex_fence_t, outer_open,
                SCAN_LEX_DECL_FENCE, PRIORITY_FENCE, false);
    ADD_RECIPES(strings, scan_lex_string_t, opener,
                SCAN_LEX_DECL_STRING, PRIORITY_STRING, true);
    ADD_RECIPES(number_forms, scan_lex_number_form_t, prefix,
                SCAN_LEX_DECL_NUMBER, PRIORITY_NUMBER, true);
    ADD_RECIPES(sigils, scan_lex_sigil_t, prefix,
                SCAN_LEX_DECL_SIGIL, PRIORITY_SIGIL, false);
    ADD_RECIPES(regexes, scan_lex_regex_t, opener,
                SCAN_LEX_DECL_REGEX, PRIORITY_REGEX, false);
#undef ADD_RECIPES
    for (const scan_lex_radix_t *item = spec->number.radixes;
         item && item->prefix.data; item++) {
        add_opening(items, &count, origins, origin_count,
                    SCAN_LEX_DECL_NUMBER,
                    PRIORITY_NUMBER, true, item->prefix);
    }
    for (const core_string_t *item = spec->operators;
         item && item->data; item++) {
        add_opening(items, &count, origins, origin_count,
                    SCAN_LEX_DECL_OPERATOR, PRIORITY_FIXED, true, *item);
    }
    for (size_t i = 0; i < grammar->named_terms.len; i++) {
        const parse_terminal_t *terminal = &grammar->named_terms.data[i];
        if (!parse_contextual_source_tid(grammar, terminal->id)
            && !string_array_contains(spec->operators, terminal->value)) {
            add_opening(items, &count, origins, origin_count,
                        SCAN_LEX_DECL_NONE,
                        PRIORITY_FIXED, true, terminal->value);
        }
    }

    qsort(items, count, sizeof(*items), opening_compare);
    lex_prefix_frame_t *stack = core_alloc_array(lex_prefix_frame_t,
                                                  count ? count : 1);
    int32_t at_priority[PRIORITY_COUNT];
    for (size_t i = 0; i < PRIORITY_COUNT; i++) {
        at_priority[i] = -1;
    }
    size_t depth = 0;
    bool valid = true;

    for (size_t i = 0; valid && i < count; i++) {
        size_t shared = i ? common_prefix(items[i - 1].spelling,
                                          items[i].spelling) : 0;
        while (depth
               && items[stack[depth - 1].opening].spelling.u8_bytes > shared) {
            lex_prefix_frame_t frame = stack[--depth];
            uint8_t priority = items[frame.opening].priority;
            if (frame.tracked) {
                at_priority[priority] = frame.previous_at_priority;
            }
        }

        if (depth) {
            const lex_opening_t *ancestor
                = &items[stack[depth - 1].opening];
            if (ancestor->spelling.u8_bytes == items[i].spelling.u8_bytes
                && ancestor->kind == items[i].kind) {
                char first[160];
                char second[160];
                format_declaration(first, sizeof(first), ancestor);
                format_declaration(second, sizeof(second), &items[i]);
                snprintf(error, error_bytes,
                         "duplicate lexer declaration: %s conflicts with %s",
                         first, second);
                valid = false;
                break;
            }
        }

        int32_t shadow = -1;
        for (size_t priority = 0; priority < items[i].priority; priority++) {
            if (at_priority[priority] >= 0) {
                shadow = at_priority[priority];
                break;
            }
        }
        if (shadow >= 0) {
            if (items[i].kind == SCAN_LEX_DECL_NONE) {
                *issues = core_realloc(
                    *issues, (*issue_count + 1) * sizeof(**issues));
                (*issues)[(*issue_count)++] = (scan_lex_issue_t){
                    .kind = SCAN_LEX_ISSUE_SHADOWED_TERMINAL,
                    .token = core_string_dup(items[i].spelling),
                    .origin = core_string_dup(items[shadow].origin),
                };
                goto keep_opening;
            }
            char first[160];
            char second[160];
            format_declaration(first, sizeof(first), &items[shadow]);
            format_declaration(second, sizeof(second), &items[i]);
            snprintf(error, error_bytes,
                     "unreachable lexer declaration: %s shadows %s",
                     first, second);
            valid = false;
            break;
        }

keep_opening:
        uint8_t priority = items[i].priority;
        bool tracked = items[i].committing;
        stack[depth++] = (lex_prefix_frame_t){
            .opening = i,
            .previous_at_priority = at_priority[priority],
            .tracked = tracked,
        };
        if (tracked) {
            at_priority[priority] = (int32_t)i;
        }
    }

    core_free(stack);
    core_free(items);
    core_free(codepoint_spellings);
    return valid;
}

static bool
known_token(parse_grammar_t *grammar, core_string_t name)
{
    return !name.data || !name.u8_bytes || parse_terminal_id(grammar, name);
}

static void
register_output(parse_grammar_t *grammar, core_string_t name)
{
    if (name.data && name.u8_bytes && !parse_terminal_id(grammar, name)) {
        parse_register_terminal(grammar, name);
    }
}

void
scan_lex_register_output_terminals(const scan_lex_spec_t *spec,
                                   parse_grammar_t *grammar)
{
    if (!spec || !grammar) {
        return;
    }
#define REGISTER(field) register_output(grammar, spec->field)
    REGISTER(tok_newline);
    REGISTER(tok_terminator);
    REGISTER(tok_indent);
    REGISTER(tok_dedent);
    REGISTER(tok_eof);
    REGISTER(tok_ident);
    REGISTER(tok_ident_upper);
    register_output(grammar, spec->number.tok_int);
    register_output(grammar, spec->number.tok_float);
#undef REGISTER
    for (const scan_lex_comment_t *item = spec->comments;
         item && item->opener.data; item++) {
        register_output(grammar, item->tok);
    }
    for (const scan_lex_string_t *item = spec->strings;
         item && item->opener.data; item++) {
        register_output(grammar, item->tok);
        register_output(grammar, item->tok_mid);
        register_output(grammar, item->tok_end);
    }
    for (const scan_lex_number_form_t *item = spec->number_forms;
         item && item->digits.data; item++) {
        register_output(grammar, item->tok_int);
        register_output(grammar, item->tok_float);
        for (const scan_lex_suffix_t *suffix = item->suffixes;
             suffix && suffix->suffix.data; suffix++) {
            register_output(grammar, suffix->tok);
        }
    }
    for (const scan_lex_radix_t *item = spec->number.radixes;
         item && item->prefix.data; item++) {
        register_output(grammar, item->tok);
    }
    for (const scan_lex_suffix_t *item = spec->number.suffixes;
         item && item->suffix.data; item++) {
        register_output(grammar, item->tok);
    }
    for (const scan_lex_sigil_t *item = spec->sigils;
         item && item->prefix.data; item++) {
        register_output(grammar, item->tok);
    }
    for (const scan_lex_regex_t *item = spec->regexes;
         item && item->opener.data; item++) {
        register_output(grammar, item->tok);
    }
    for (const scan_lex_heredoc_t *item = spec->heredocs;
         item && item->opener.data; item++) {
        register_output(grammar, item->tok);
        register_output(grammar, item->interpolated_tok);
    }
    for (const scan_lex_embed_t *item = spec->embeds;
         item && item->open.data; item++) {
        register_output(grammar, item->tok);
        register_output(grammar, item->open_tok);
        register_output(grammar, item->close_tok);
    }
    for (const scan_lex_fence_t *item = spec->fences;
         item && item->outer_open.data; item++) {
        register_output(grammar, item->tok);
    }
    for (const scan_lex_directive_t *item = spec->directives;
         item && item->open.data; item++) {
        register_output(grammar, item->tok_eol);
        register_output(grammar, item->tok_other);
    }
    for (const scan_lex_literal_hook_t *item = spec->literal_hooks;
         item && item->token.data; item++) {
        register_output(grammar, item->token);
        for (const core_string_t *output = item->outputs;
             output && output->data; output++) {
            register_output(grammar, *output);
        }
    }
    for (const scan_lex_recovery_t *item = spec->recoveries;
         item && item->token.data; item++) {
        register_output(grammar, item->token);
        register_output(grammar, item->replacement);
    }
}

static bool
require_token(parse_grammar_t *grammar, core_string_t name,
              const char *declaration, char *error, size_t error_bytes)
{
    if (known_token(grammar, name)) {
        return true;
    }
    snprintf(error, error_bytes, "%s emits unknown token '%.*s'",
             declaration, (int)name.u8_bytes, name.data);
    return false;
}

static int
string_item_compare(const void *left, const void *right)
{
    return string_compare(*(const core_string_t *)left,
                          *(const core_string_t *)right);
}

static bool
require_unique_outputs(const core_string_t *items, const char *declaration,
                       char *error, size_t error_bytes)
{
    size_t count = 0;
    while (items && items[count].data) {
        count++;
    }
    if (count < 2) {
        return true;
    }
    core_string_t *sorted = core_alloc_array(core_string_t, count);
    memcpy(sorted, items, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), string_item_compare);
    bool unique = true;
    for (size_t i = 1; i < count; i++) {
        if (core_string_eq(sorted[i - 1], sorted[i])) {
            snprintf(error, error_bytes,
                     "%s declares duplicate output token '%.*s'",
                     declaration, (int)sorted[i].u8_bytes, sorted[i].data);
            unique = false;
            break;
        }
    }
    core_free(sorted);
    return unique;
}

static bool
analyze_outputs(const scan_lex_spec_t *spec, parse_grammar_t *grammar,
                char *error, size_t error_bytes)
{
#define REQUIRE(name, declaration)                                            \
    do {                                                                       \
        if (!require_token(grammar, name, declaration, error, error_bytes)) { \
            return false;                                                      \
        }                                                                      \
    } while (0)
    REQUIRE(spec->tok_newline, "@lex-layout");
    REQUIRE(spec->tok_terminator, "@lex-layout");
    REQUIRE(spec->tok_indent, "@lex-layout");
    REQUIRE(spec->tok_dedent, "@lex-layout");
    REQUIRE(spec->tok_eof, "@lex-layout");
    REQUIRE(spec->tok_ident, "@lex-ident");
    REQUIRE(spec->tok_ident_upper, "@lex-ident");
    REQUIRE(spec->number.tok_int, "@lex-number");
    REQUIRE(spec->number.tok_float, "@lex-number");

    for (const scan_lex_comment_t *item = spec->comments;
         item && item->opener.data; item++) {
        REQUIRE(item->tok, "@lex-comment");
    }
    for (const scan_lex_string_t *item = spec->strings;
         item && item->opener.data; item++) {
        REQUIRE(item->tok, "@lex-string");
        REQUIRE(item->tok_mid, "@lex-string");
        REQUIRE(item->tok_end, "@lex-string");
    }
    for (const scan_lex_number_form_t *item = spec->number_forms;
         item && item->digits.data; item++) {
        REQUIRE(item->tok_int, "@lex-number");
        REQUIRE(item->tok_float, "@lex-number");
        for (const scan_lex_suffix_t *suffix = item->suffixes;
             suffix && suffix->suffix.data; suffix++) {
            REQUIRE(suffix->tok, "@lex-number");
        }
    }
    for (const scan_lex_radix_t *item = spec->number.radixes;
         item && item->prefix.data; item++) {
        REQUIRE(item->tok, "@lex-number");
    }
    for (const scan_lex_suffix_t *item = spec->number.suffixes;
         item && item->suffix.data; item++) {
        REQUIRE(item->tok, "@lex-number");
    }
    for (const scan_lex_sigil_t *item = spec->sigils;
         item && item->prefix.data; item++) {
        REQUIRE(item->tok, "@lex-sigil");
    }
    for (const scan_lex_regex_t *item = spec->regexes;
         item && item->opener.data; item++) {
        REQUIRE(item->tok, "@lex-regex");
    }
    for (const scan_lex_heredoc_t *item = spec->heredocs;
         item && item->opener.data; item++) {
        REQUIRE(item->tok, "@lex-heredoc");
        REQUIRE(item->interpolated_tok, "@lex-heredoc");
    }
    for (const scan_lex_embed_t *item = spec->embeds;
         item && item->open.data; item++) {
        REQUIRE(item->tok, "@lex-embed");
        REQUIRE(item->open_tok, "@lex-embed");
        REQUIRE(item->close_tok, "@lex-embed");
    }
    for (const scan_lex_fence_t *item = spec->fences;
         item && item->outer_open.data; item++) {
        REQUIRE(item->tok, "@lex-fence");
    }
    for (const scan_lex_directive_t *item = spec->directives;
         item && item->open.data; item++) {
        REQUIRE(item->tok_eol, "@lex-directive");
        REQUIRE(item->tok_other, "@lex-directive");
    }
    for (const scan_lex_literal_hook_t *item = spec->literal_hooks;
         item && item->token.data; item++) {
        REQUIRE(item->token, "@lex-literal-hook");
        if (!require_unique_outputs(item->outputs, "@lex-literal-hook",
                                    error, error_bytes)) {
            return false;
        }
        for (const core_string_t *output = item->outputs;
             output && output->data; output++) {
            REQUIRE(*output, "@lex-literal-hook");
        }
    }
    for (const scan_lex_recovery_t *item = spec->recoveries;
         item && item->token.data; item++) {
        REQUIRE(item->token, "@lex-recover");
        REQUIRE(item->replacement, "@lex-recover");
    }
    for (const core_string_t *item = spec->operators;
         item && item->data; item++) {
        REQUIRE(*item, "@lex-operators");
    }
    for (const core_string_t *item = spec->soft_keywords;
         item && item->data; item++) {
        if (!parse_lookup_terminal(grammar, *item)) {
            snprintf(error, error_bytes,
                     "@lex-soft-keywords names unknown terminal '%.*s'",
                     (int)item->u8_bytes, item->data);
            return false;
        }
    }
#undef REQUIRE
    return true;
}

static void
add_output(lex_output_t **items, size_t *count, size_t *capacity,
           core_string_t token, core_string_t origin)
{
    if (!token.data || !token.u8_bytes) {
        return;
    }
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 32;
        *items = core_realloc(*items, next * sizeof(**items));
        *capacity = next;
    }
    (*items)[(*count)++] = (lex_output_t){
        .token = token,
        .origin = origin,
    };
}

static int
output_compare(const void *left, const void *right)
{
    const lex_output_t *a = left;
    const lex_output_t *b = right;
    size_t common = a->token.u8_bytes < b->token.u8_bytes
                  ? a->token.u8_bytes : b->token.u8_bytes;
    int order = memcmp(a->token.data, b->token.data, common);
    if (order) {
        return order;
    }
    if (a->token.u8_bytes != b->token.u8_bytes) {
        return (a->token.u8_bytes > b->token.u8_bytes)
             - (a->token.u8_bytes < b->token.u8_bytes);
    }
    common = a->origin.u8_bytes < b->origin.u8_bytes
           ? a->origin.u8_bytes : b->origin.u8_bytes;
    order = common ? memcmp(a->origin.data, b->origin.data, common) : 0;
    if (order) {
        return order;
    }
    return (a->origin.u8_bytes > b->origin.u8_bytes)
         - (a->origin.u8_bytes < b->origin.u8_bytes);
}

static bool
grammar_uses_token(const parse_grammar_t *grammar, int64_t tid)
{
    for (size_t i = 0; i < grammar->rules.len; i++) {
        const parse_rule_t *rule = &grammar->rules.data[i];
        for (size_t j = 0; j < rule->contents.len; j++) {
            const parse_match_t *match = &rule->contents.data[j];
            switch (match->kind) {
            case PARSE_MATCH_TERMINAL:
                if (match->terminal_id == tid
                    || parse_contextual_source_tid(grammar,
                                                   match->terminal_id) == tid) {
                    return true;
                }
                break;
            case PARSE_MATCH_ANY:
                return true;
            case PARSE_MATCH_SET:
                if (parse_match_set_holds(match->set_items, tid)) {
                    return true;
                }
                break;
            default:
                break;
            }
        }
    }
    return false;
}

static bool
collect_unused_outputs(const scan_lex_spec_t *spec, parse_grammar_t *grammar,
                       lex_decl_origin_t *origins, size_t origin_count,
                       scan_lex_issue_t **issues, size_t *issue_count)
{
    if (!grammar->rules.len) {
        return true;
    }

    lex_output_t *outputs = nullptr;
    size_t count = 0;
    size_t capacity = 0;
#define ADD_BASE(field) add_output(&outputs, &count, &capacity, spec->field, \
                                   (core_string_t){0})
    ADD_BASE(tok_newline);
    ADD_BASE(tok_terminator);
    ADD_BASE(tok_indent);
    ADD_BASE(tok_dedent);
    ADD_BASE(tok_eof);
    ADD_BASE(tok_ident);
    ADD_BASE(tok_ident_upper);
    add_output(&outputs, &count, &capacity, spec->number.tok_int,
               (core_string_t){0});
    add_output(&outputs, &count, &capacity, spec->number.tok_float,
               (core_string_t){0});
#undef ADD_BASE

#define RECIPE_ORIGIN(kind_value, spelling_value)                              \
    lookup_origin(origins, origin_count, kind_value, spelling_value, false)
    for (const scan_lex_comment_t *item = spec->comments;
         item && item->opener.data; item++) {
        add_output(&outputs, &count, &capacity, item->tok,
                   RECIPE_ORIGIN(SCAN_LEX_DECL_COMMENT, item->opener));
    }
    for (const scan_lex_string_t *item = spec->strings;
         item && item->opener.data; item++) {
        core_string_t origin
            = RECIPE_ORIGIN(SCAN_LEX_DECL_STRING, item->opener);
        add_output(&outputs, &count, &capacity, item->tok, origin);
        add_output(&outputs, &count, &capacity, item->tok_mid, origin);
        add_output(&outputs, &count, &capacity, item->tok_end, origin);
    }
    for (const scan_lex_number_form_t *item = spec->number_forms;
         item && item->digits.data; item++) {
        core_string_t origin
            = RECIPE_ORIGIN(SCAN_LEX_DECL_NUMBER, item->prefix);
        add_output(&outputs, &count, &capacity, item->tok_int, origin);
        add_output(&outputs, &count, &capacity, item->tok_float, origin);
        for (const scan_lex_suffix_t *suffix = item->suffixes;
             suffix && suffix->suffix.data; suffix++) {
            add_output(&outputs, &count, &capacity, suffix->tok, origin);
        }
    }
    for (const scan_lex_radix_t *item = spec->number.radixes;
         item && item->prefix.data; item++) {
        add_output(&outputs, &count, &capacity, item->tok,
                   RECIPE_ORIGIN(SCAN_LEX_DECL_NUMBER, item->prefix));
    }
    for (const scan_lex_suffix_t *item = spec->number.suffixes;
         item && item->suffix.data; item++) {
        add_output(&outputs, &count, &capacity, item->tok, (core_string_t){0});
    }
    for (const scan_lex_sigil_t *item = spec->sigils;
         item && item->prefix.data; item++) {
        add_output(&outputs, &count, &capacity, item->tok,
                   RECIPE_ORIGIN(SCAN_LEX_DECL_SIGIL, item->prefix));
    }
    for (const scan_lex_regex_t *item = spec->regexes;
         item && item->opener.data; item++) {
        add_output(&outputs, &count, &capacity, item->tok,
                   RECIPE_ORIGIN(SCAN_LEX_DECL_REGEX, item->opener));
    }
    for (const scan_lex_heredoc_t *item = spec->heredocs;
         item && item->opener.data; item++) {
        core_string_t origin
            = RECIPE_ORIGIN(SCAN_LEX_DECL_HEREDOC, item->opener);
        add_output(&outputs, &count, &capacity, item->tok, origin);
        add_output(&outputs, &count, &capacity, item->interpolated_tok,
                   origin);
    }
    for (const scan_lex_embed_t *item = spec->embeds;
         item && item->open.data; item++) {
        core_string_t origin
            = RECIPE_ORIGIN(SCAN_LEX_DECL_EMBED, item->open);
        add_output(&outputs, &count, &capacity, item->tok, origin);
        add_output(&outputs, &count, &capacity, item->open_tok, origin);
        add_output(&outputs, &count, &capacity, item->close_tok, origin);
    }
    for (const scan_lex_fence_t *item = spec->fences;
         item && item->outer_open.data; item++) {
        add_output(&outputs, &count, &capacity, item->tok,
                   RECIPE_ORIGIN(SCAN_LEX_DECL_FENCE, item->outer_open));
    }
    for (const scan_lex_directive_t *item = spec->directives;
         item && item->open.data; item++) {
        core_string_t origin
            = RECIPE_ORIGIN(SCAN_LEX_DECL_DIRECTIVE, item->open);
        add_output(&outputs, &count, &capacity, item->tok_eol, origin);
        add_output(&outputs, &count, &capacity, item->tok_other, origin);
    }
    for (const scan_lex_literal_hook_t *item = spec->literal_hooks;
         item && item->token.data; item++) {
        core_string_t origin
            = RECIPE_ORIGIN(SCAN_LEX_DECL_LITERAL_HOOK, item->token);
        for (const core_string_t *output = item->outputs;
             output && output->data; output++) {
            add_output(&outputs, &count, &capacity, *output, origin);
        }
    }
    for (const scan_lex_recovery_t *item = spec->recoveries;
         item && item->token.data; item++) {
        add_output(&outputs, &count, &capacity, item->replacement,
                   RECIPE_ORIGIN(SCAN_LEX_DECL_RECOVERY, item->token));
    }
    for (const core_string_t *item = spec->operators;
         item && item->data; item++) {
        add_output(&outputs, &count, &capacity, *item,
                   RECIPE_ORIGIN(SCAN_LEX_DECL_OPERATOR, *item));
    }
#undef RECIPE_ORIGIN

    qsort(outputs, count, sizeof(*outputs), output_compare);
    size_t unused = 0;
    for (size_t i = 0; i < count;) {
        size_t next = i + 1;
        while (next < count
               && core_string_eq(outputs[i].token, outputs[next].token)) {
            next++;
        }
        int64_t tid = parse_terminal_id(grammar, outputs[i].token);
        if (tid && !grammar_uses_token(grammar, tid)) {
            unused++;
        }
        i = next;
    }
    if (unused) {
        size_t old_count = *issue_count;
        *issues = core_realloc(*issues,
                               (old_count + unused) * sizeof(**issues));
        size_t out = old_count;
        for (size_t i = 0; i < count;) {
            size_t next = i + 1;
            while (next < count
                   && core_string_eq(outputs[i].token, outputs[next].token)) {
                next++;
            }
            int64_t tid = parse_terminal_id(grammar, outputs[i].token);
            if (tid && !grammar_uses_token(grammar, tid)) {
                (*issues)[out++] = (scan_lex_issue_t){
                    .kind = SCAN_LEX_ISSUE_UNUSED_OUTPUT,
                    .token = core_string_dup(outputs[i].token),
                    .origin = core_string_dup(outputs[i].origin),
                };
            }
            i = next;
        }
        *issue_count = out;
    }
    core_free(outputs);
    return true;
}

bool
scan_lex_analyze_spec(const scan_lex_spec_t *spec, parse_grammar_t *grammar,
                      char *error, size_t error_bytes,
                      scan_lex_issue_t **issues, size_t *issue_count)
{
    if (!spec || !grammar || !error || !error_bytes
        || !issues || !issue_count) {
        return false;
    }
    error[0] = '\0';
    *issues = nullptr;
    *issue_count = 0;
    size_t origin_count = 0;
    lex_decl_origin_t *origins = declaration_index(spec, &origin_count);
    bool valid = analyze_outputs(spec, grammar, error, error_bytes)
              && analyze_openings(spec, grammar, origins, origin_count,
                                  error, error_bytes, issues, issue_count)
              && collect_unused_outputs(spec, grammar, origins, origin_count,
                                        issues, issue_count);
    core_free(origins);
    if (!valid) {
        for (size_t i = 0; i < *issue_count; i++) {
            core_free((*issues)[i].token.data);
            core_free((*issues)[i].origin.data);
        }
        core_free(*issues);
        *issues = nullptr;
        *issue_count = 0;
    }
    return valid;
}
