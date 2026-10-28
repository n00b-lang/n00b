#include "syntax_diagnostics.h"

#include "diagnostics.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/grammar.h"
#include "scanner/token_stream.h"
#include "unicode/encoding.h"

#define SYNTAX_ERROR_LIMIT 50
#define FOUND_TEXT_LIMIT 32

typedef struct {
    const n00bb_syntax_input_t *input;
    int32_t count;
    int32_t *match;
    int32_t *implicit_close;
    bool *resumes;
    int32_t resumed;
    int32_t *parent;
    bool *statement_block;
    int64_t newline;
    int64_t indent;
    int64_t dedent;
    int64_t identifier;
    int64_t pass;
    parse_pwz_parser_t *module_parser;
    parse_pwz_parser_t *block_parser;
    size_t reported;
} syntax_t;

typedef struct {
    const char *rule;
    n00bb_text_t phrase;
    n00bb_text_t unnamed;
    bool named;
    bool function_part;
} construct_name_t;

#define CONSTRUCT(rule, text) \
    {rule, N00B_TEXT_IN_##text, N00B_TEXT_IN_##text, false, false}
#define NAMED_CONSTRUCT(rule, text) \
    {rule, N00B_TEXT_IN_##text##_NAMED, N00B_TEXT_IN_##text, true, false}

static const construct_name_t constructs[] = {
    {"formals", N00B_TEXT_IN_FORMALS, N00B_TEXT_IN_FORMALS, false, true},
    CONSTRUCT("call-args", CALL_ARGS),
    CONSTRUCT("index-contents", INDEX),
    CONSTRUCT("slice-contents", SLICE),
    CONSTRUCT("list-lit", LIST_LITERAL),
    CONSTRUCT("dict-or-set-lit", DICT_LITERAL),
    CONSTRUCT("tuple-or-paren", PARENTHESES),
    CONSTRUCT("type-spec", TYPE),
    CONSTRUCT("return-stmt", RETURN),
    CONSTRUCT("fail-stmt", FAIL),
    CONSTRUCT("assert-stmt", ASSERTION),
    CONSTRUCT("import-stmt", IMPORT),
    CONSTRUCT("export-stmt", EXPORT),
    CONSTRUCT("assign-stmt", ASSIGNMENT),
    CONSTRUCT("binop-assign-stmt", ASSIGNMENT),
    CONSTRUCT("if-stmt", IF_STATEMENT),
    CONSTRUCT("if-value-expr", IF_EXPRESSION),
    CONSTRUCT("for-stmt", FOR_LOOP),
    CONSTRUCT("while-stmt", WHILE_LOOP),
    CONSTRUCT("when-stmt", WHEN_STATEMENT),
    CONSTRUCT("switch-stmt", SWITCH_STATEMENT),
    CONSTRUCT("switch-value-expr", SWITCH_EXPRESSION),
    CONSTRUCT("typecase-stmt", TYPECASE_STATEMENT),
    CONSTRUCT("typecase-value-expr", TYPECASE_EXPRESSION),
    CONSTRUCT("try-stmt", TRY_STATEMENT),
    CONSTRUCT("try-value-expr", TRY_EXPRESSION),
    CONSTRUCT("with-stmt", WITH_STATEMENT),
    NAMED_CONSTRUCT("variable-decl", VARIABLE),
    NAMED_CONSTRUCT("func-def", FUNCTION),
    NAMED_CONSTRUCT("record-decl", RECORD),
    NAMED_CONSTRUCT("object-decl", OBJECT),
    NAMED_CONSTRUCT("type-decl", TYPE_DECLARATION),
    NAMED_CONSTRUCT("variant-decl", VARIANT),
    NAMED_CONSTRUCT("trait-decl", TRAIT),
    NAMED_CONSTRUCT("enum-stmt", ENUM),
    NAMED_CONSTRUCT("flagset-stmt", FLAGSET),
    NAMED_CONSTRUCT("context-def", CONTEXT),
    NAMED_CONSTRUCT("callback-decl", CALLBACK),
    NAMED_CONSTRUCT("package-decl", PACKAGE),
    NAMED_CONSTRUCT("alias-stmt", ALIAS),
};

typedef struct {
    const char *rule;
    n00bb_text_t phrase;
} category_name_t;

static const category_name_t categories[] = {
    {"expression", N00B_TEXT_AN_EXPRESSION},
    {"unary-expr", N00B_TEXT_AN_EXPRESSION},
    {"type-spec", N00B_TEXT_A_TYPE},
    {"top-level-stmt", N00B_TEXT_A_STATEMENT},
    {"body-stmt", N00B_TEXT_A_STATEMENT},
    {"param-decl", N00B_TEXT_A_PARAMETER},
    {"block-start", N00B_TEXT_A_BLOCK},
};

static const char *statement_lists[] = {
    "module", "other-top-stmts", "body-stmts", "body-stmt-list",
    "value-body-stmts", "try-body-stmts", "eos",
};

static bool
text_is(core_string_t text, const char *literal)
{
    size_t length = 0;
    while (literal[length]) length++;
    return text.u8_bytes == length
        && core_string_eq(text, core_string_borrow(literal, length));
}

static bool
text_ends_with(core_string_t text, const char *suffix)
{
    size_t length = 0;
    while (suffix[length]) length++;
    return text.u8_bytes >= length
        && core_string_eq(core_string_slice(text, text.u8_bytes - length,
                                            length),
                          core_string_borrow(suffix, length));
}

static parse_token_info_t *
token_at(const syntax_t *s, int32_t index)
{
    return index >= 0 && index < s->count
        ? scan_stream_get(s->input->tokens, index) : nullptr;
}

static int32_t
tid_at(const syntax_t *s, int32_t index)
{
    parse_token_info_t *token = token_at(s, index);
    return token ? parse_token_tid(token) : PARSE_TOK_EOF;
}

static bool
is_opener(const syntax_t *s, int32_t index)
{
    int32_t tid = tid_at(s, index);
    return tid == '{' || tid == '(' || tid == '[' || tid == s->indent;
}

static bool
is_closer(const syntax_t *s, int32_t index)
{
    int32_t tid = tid_at(s, index);
    return tid == '}' || tid == ')' || tid == ']' || tid == s->dedent;
}

static int32_t
closer_for(const syntax_t *s, int32_t opener_tid)
{
    if (opener_tid == '{') return '}';
    if (opener_tid == '(') return ')';
    if (opener_tid == '[') return ']';
    return (int32_t)s->dedent;
}

static bool
is_statement_end(const syntax_t *s, int32_t index)
{
    int32_t tid = tid_at(s, index);
    return tid == s->newline || tid == ';';
}

static int32_t line_indent(const syntax_t *s, int32_t index);
static core_string_t token_source(const syntax_t *s,
                                  parse_token_info_t *token);

static bool
starts_declaration_line(const syntax_t *s, int32_t index)
{
    static const char *keywords[] = {
        "func", "record", "object", "type", "enum", "flagset", "trait",
        "variant", "import", "package", "alias", "context", "callback",
    };
    int32_t before = index ? tid_at(s, index - 1) : s->newline;
    if (before != s->newline && before != s->indent && before != s->dedent)
        return false;
    core_string_t text = token_source(s, token_at(s, index));
    for (size_t k = 0; k < sizeof(keywords) / sizeof(keywords[0]); k++)
        if (text_is(text, keywords[k])) return true;
    return false;
}

static void
match_brackets(syntax_t *s)
{
    int32_t *stack = core_alloc_array(int32_t, (size_t)s->count + 1);
    int32_t depth = 0;
    for (int32_t i = 0; i < s->count; i++) {
        s->match[i] = -1;
        s->implicit_close[i] = -1;
        s->resumes[i] = false;
        if (starts_declaration_line(s, i))
            while (depth && tid_at(s, stack[depth - 1]) == '{'
                   && line_indent(s, i) <= line_indent(s, stack[depth - 1])) {
                s->implicit_close[stack[--depth]] = i;
                s->resumes[i] = true;
            }
        s->parent[i] = depth ? stack[depth - 1] : -1;
        if (is_opener(s, i)) {
            stack[depth++] = i;
            continue;
        }
        if (!is_closer(s, i)) continue;
        int32_t tid = tid_at(s, i);
        int32_t found = depth;
        while (found > 0 && closer_for(s, tid_at(s, stack[found - 1])) != tid)
            found--;
        if (!found) continue;
        int32_t opener = stack[found - 1];
        depth = found - 1;
        s->match[i] = opener;
        s->match[opener] = i;
        s->parent[i] = depth ? stack[depth - 1] : -1;
    }
    core_free(stack);
}

static core_diag_location_t
token_location(parse_token_info_t *token)
{
    return (core_diag_location_t){
        .line = parse_token_line(token),
        .column = parse_token_column(token),
        .end_line = parse_token_endline(token),
        .end_column = parse_token_endcol(token),
        .has_location = true,
    };
}

static core_string_t
token_source(const syntax_t *s, parse_token_info_t *token)
{
    core_string_t text = s->input->text;
    size_t start = parse_token_start_byte(token);
    size_t end = parse_token_end_byte(token);
    if (start > text.u8_bytes) start = text.u8_bytes;
    if (end > text.u8_bytes || end < start) end = start;
    return core_string_slice(text, start, end - start);
}

static core_string_t
quoted(core_string_t text)
{
    size_t length = text.u8_bytes;
    for (size_t i = 0; i < length; i++)
        if (text.data[i] == '\n' || text.data[i] == '\r') length = i;
    if (length > FOUND_TEXT_LIMIT) {
        length = FOUND_TEXT_LIMIT;
        while (length && ((unsigned char)text.data[length] & 0xC0) == 0x80)
            length--;
    }
    return n00bb_text(length < text.u8_bytes ? N00B_TEXT_QUOTED_CUT
                                             : N00B_TEXT_QUOTED_NAME,
        N00BB_ARGS(ARG_TEXT(core_string_slice(text, 0, length))));
}

static core_string_t
describe_found(const syntax_t *s, int32_t index)
{
    parse_token_info_t *token = token_at(s, index);
    int32_t tid = token ? parse_token_tid(token) : PARSE_TOK_EOF;
    core_string_t text = token ? token_source(s, token) : (core_string_t){0};
    if (!token) return n00bb_text(N00B_TEXT_END_OF_FILE, N00BB_NO_ARGS);
    if (tid == s->newline) return n00bb_text(N00B_TEXT_END_OF_LINE,
                                             N00BB_NO_ARGS);
    if (tid == s->indent) return n00bb_text(N00B_TEXT_INDENTATION,
                                            N00BB_NO_ARGS);
    if (tid == s->dedent) return n00bb_text(N00B_TEXT_END_OF_BLOCK,
                                            N00BB_NO_ARGS);
    if (text.u8_bytes) return quoted(text);
    return n00bb_text(N00B_TEXT_THIS_TOKEN, N00BB_NO_ARGS);
}

static bool
is_keyword_name(core_string_t name)
{
    if (!name.u8_bytes) return false;
    for (size_t i = 0; i < name.u8_bytes; i++) {
        char c = name.data[i];
        if (!((c >= 'a' && c <= 'z') || c == '_')) return false;
    }
    return true;
}

static core_string_t
describe_terminal(const syntax_t *s, const parse_pwz_expected_t *entry)
{
    if (entry->spelling.u8_bytes) return quoted(entry->spelling);
    if (entry->tid > 0 && entry->tid < 128) {
        char c = (char)entry->tid;
        return quoted(core_string_borrow(&c, 1));
    }
    if (entry->tid == s->newline)
        return n00bb_text(N00B_TEXT_END_OF_LINE, N00BB_NO_ARGS);
    core_string_t name = parse_terminal_name(s->input->grammar, entry->tid);
    static const struct {
        const char *name;
        n00bb_text_t phrase;
    } classes[] = {
        {"IDENTIFIER", N00B_TEXT_A_NAME},
        {"INT_LIT", N00B_TEXT_A_NUMBER},
        {"HEX_LIT", N00B_TEXT_A_NUMBER},
        {"FLOAT_LIT", N00B_TEXT_A_NUMBER},
        {"STRING_LIT", N00B_TEXT_A_STRING},
        {"CHAR_LIT", N00B_TEXT_A_CHARACTER_LITERAL},
        {"TYPE_VAR", N00B_TEXT_A_TYPE_VARIABLE},
        {"EMBED", N00B_TEXT_A_FENCED_TEXT_BLOCK},
        {"INDENT", N00B_TEXT_AN_INDENTED_BLOCK},
        {"DEDENT", N00B_TEXT_THE_END_OF_BLOCK},
    };
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
        if (text_is(name, classes[i].name))
            return n00bb_text(classes[i].phrase, N00BB_NO_ARGS);
    if (name.u8_bytes) return quoted(name);
    return n00bb_text(N00B_TEXT_A_TOKEN, N00BB_NO_ARGS);
}

static n00bb_text_t
category_of(const parse_pwz_expected_t *entry, int32_t floor, int32_t at)
{
    n00bb_text_t found = N00B_TEXT_COUNT;
    int32_t start = entry->frame_count ? entry->frames[0].start : -1;
    if (start > at || start < floor) return N00B_TEXT_COUNT;
    for (size_t i = 0; i < entry->frame_count; i++) {
        if (entry->frames[i].start != start) break;
        for (size_t c = 0; c < sizeof(categories) / sizeof(categories[0]); c++)
            if (text_is(entry->frames[i].rule, categories[c].rule))
                found = categories[c].phrase;
    }
    return found;
}

static bool
is_operator_entry(const parse_pwz_expected_t *entry)
{
    return entry->frame_count
        && text_ends_with(entry->frames[0].rule, "-op")
        && !text_ends_with(entry->frames[0].rule, "assign-op");
}

static bool
phrase_known(core_list_t(core_string_t) *phrases, core_string_t phrase)
{
    for (size_t i = 0; i < phrases->len; i++)
        if (core_string_eq(phrases->data[i], phrase)) return true;
    return false;
}


static core_string_t
expected_list(const syntax_t *s, parse_pwz_parser_t *parser, int32_t at)
{
    enum { RANK_CLOSER, RANK_COMMA, RANK_END, RANK_CATEGORY, RANK_OPERATOR,
           RANK_OTHER, RANK_COUNT };
    core_list_t(core_string_t) ranked[RANK_COUNT];
    for (size_t r = 0; r < RANK_COUNT; r++)
        ranked[r] = core_list_new(core_string_t);
    int32_t floor = at;
    while (floor > 0 && tid_at(s, floor - 1) == s->newline) floor--;
    for (size_t i = 0; i < parse_pwz_expected_count(parser); i++) {
        const parse_pwz_expected_t *entry = parse_pwz_expected_at(parser, i);
        n00bb_text_t category = category_of(entry, floor, at);
        size_t rank = RANK_OTHER;
        core_string_t text;
        if (entry->tid == s->newline || entry->tid == ';') {
            text = n00bb_text(N00B_TEXT_END_OF_STATEMENT, N00BB_NO_ARGS);
            rank = RANK_END;
        }
        else if (category != N00B_TEXT_COUNT) {
            text = n00bb_text(category, N00BB_NO_ARGS);
            rank = RANK_CATEGORY;
        }
        else if (is_operator_entry(entry)) {
            text = n00bb_text(N00B_TEXT_AN_OPERATOR, N00BB_NO_ARGS);
            rank = RANK_OPERATOR;
        }
        else {
            text = describe_terminal(s, entry);
            rank = entry->tid == ')' || entry->tid == ']' || entry->tid == '}'
                ? RANK_CLOSER : entry->tid == ',' ? RANK_COMMA : RANK_OTHER;
        }
        bool known = false;
        for (size_t r = 0; r < RANK_COUNT && !known; r++)
            known = phrase_known(&ranked[r], text);
        if (known) core_free(text.data);
        else core_list_push(ranked[rank], text);
    }
    core_list_t(core_string_t) phrases = core_list_new(core_string_t);
    for (size_t r = 0; r < RANK_COUNT; r++) {
        for (size_t i = 0; i < ranked[r].len; i++)
            core_list_push(phrases, ranked[r].data[i]);
        core_list_free(ranked[r]);
    }
    size_t total = phrases.len;
    if (total > 6) {
        for (size_t i = 5; i < total; i++) core_free(phrases.data[i].data);
        phrases.len = 5;
        core_list_push(phrases, n00bb_text(N00B_TEXT_OTHER_TOKENS,
                                           N00BB_ARGS(ARG_NUMBER(total - 5))));
    }
    core_string_t result = phrases.len
        ? n00bb_text(N00B_TEXT_TEXT,
                     N00BB_ARGS(ARG_ALTERNATIVES(phrases.data, phrases.len)))
        : (core_string_t){0};
    for (size_t i = 0; i < phrases.len; i++) core_free(phrases.data[i].data);
    core_list_free(phrases);
    return result;
}

static core_string_t
name_between(const syntax_t *s, int32_t first, int32_t end)
{
    for (int32_t i = first; i < end && i < s->count; i++)
        if (tid_at(s, i) == s->identifier)
            return token_source(s, token_at(s, i));
    return (core_string_t){0};
}

static const construct_name_t *
construct_named(core_string_t rule)
{
    for (size_t c = 0; c < sizeof(constructs) / sizeof(constructs[0]); c++)
        if (text_is(rule, constructs[c].rule)) return &constructs[c];
    return nullptr;
}

static core_string_t
describe_construct(const syntax_t *s, const parse_pwz_expected_t *entry,
                   int32_t at)
{
    const construct_name_t *inner = nullptr;
    int32_t inner_start = -1;
    size_t inner_frame = 0;
    for (size_t i = 0; i < entry->frame_count && !inner; i++) {
        if (entry->frames[i].start >= at || entry->frames[i].start < 0)
            continue;
        inner = construct_named(entry->frames[i].rule);
        inner_start = entry->frames[i].start;
        inner_frame = i;
    }
    if (!inner) return (core_string_t){0};
    const construct_name_t *owner = inner;
    int32_t owner_start = inner_start;
    if (inner->function_part) {
        for (size_t i = inner_frame + 1; i < entry->frame_count; i++)
            if (text_is(entry->frames[i].rule, "func-def")) {
                owner = construct_named(entry->frames[i].rule);
                owner_start = entry->frames[i].start;
                break;
            }
    }
    if (!inner->named) {
        core_string_t name = owner != inner
            ? name_between(s, owner_start, inner_start) : (core_string_t){0};
        return n00bb_text(name.u8_bytes ? N00B_TEXT_IN_PART_OF_FUNCTION
                                        : inner->phrase,
            N00BB_ARGS(ARG_PHRASE_ID(inner->phrase), ARG_TEXT(name)));
    }
    core_string_t name = name_between(s, inner_start, at);
    return n00bb_text(name.u8_bytes ? inner->phrase : inner->unnamed,
                      N00BB_ARGS(ARG_TEXT(name)));
}

static core_string_t
construct_phrase(const syntax_t *s, parse_pwz_parser_t *parser, int32_t at)
{
    core_string_t out = {0};
    for (size_t i = 0; i < parse_pwz_expected_count(parser) && !out.u8_bytes;
         i++) {
        core_free(out.data);
        out = describe_construct(s, parse_pwz_expected_at(parser, i), at);
    }
    return out;
}

static void
mark_statement_blocks(syntax_t *s, parse_pwz_parser_t *parser)
{
    for (size_t i = 0; i < parse_pwz_expected_count(parser); i++) {
        const parse_pwz_expected_t *entry = parse_pwz_expected_at(parser, i);
        for (size_t f = 0; f < entry->frame_count; f++) {
            core_string_t rule = entry->frames[f].rule;
            if (!text_is(rule, "body") && !text_is(rule, "value-body")
                && !text_is(rule, "try-body"))
                continue;
            int32_t start = entry->frames[f].start;
            if (tid_at(s, start) == '{') s->statement_block[start] = true;
            else if (tid_at(s, start + 2) == s->indent)
                s->statement_block[start + 2] = true;
        }
    }
}

static bool
inside_statement(const parse_pwz_parser_t *parser, int32_t before)
{
    for (size_t i = 0; i < parse_pwz_expected_count(parser); i++) {
        const parse_pwz_expected_t *entry = parse_pwz_expected_at(parser, i);
        for (size_t f = 0; f < entry->frame_count; f++) {
            if (entry->frames[f].start >= before) continue;
            core_string_t rule = entry->frames[f].rule;
            if (!rule.u8_bytes || rule.data[0] == '$') continue;
            for (size_t k = 0;
                 k < sizeof(statement_lists) / sizeof(statement_lists[0]); k++)
                if (text_is(rule, statement_lists[k])) return false;
            return true;
        }
    }
    return false;
}

static int32_t
line_indent(const syntax_t *s, int32_t index)
{
    parse_token_info_t *token = token_at(s, index);
    if (!token) return 0;
    uint32_t line = parse_token_line(token);
    int32_t first = index;
    while (first > 0) {
        parse_token_info_t *before = token_at(s, first - 1);
        if (!before || parse_token_line(before) != line
            || parse_token_tid(before) == s->indent
            || parse_token_tid(before) == s->dedent
            || parse_token_tid(before) == s->newline)
            break;
        first--;
    }
    return (int32_t)parse_token_column(token_at(s, first));
}

static void
report_unexpected(const syntax_t *s, parse_pwz_parser_t *parser, int32_t at,
                  n00bb_diag_t id, core_diag_location_t where,
                  n00bb_arg_t found)
{
    core_string_t expected = expected_list(s, parser, at);
    core_string_t construct = construct_phrase(s, parser, at);
    n00bb_text_t message = expected.u8_bytes
        ? construct.u8_bytes ? N00B_TEXT_SYNTAX_EXPECTED_FOUND_IN
                             : N00B_TEXT_SYNTAX_EXPECTED_FOUND
        : construct.u8_bytes ? N00B_TEXT_SYNTAX_UNEXPECTED_IN
                             : N00B_TEXT_SYNTAX_UNEXPECTED;
    n00bb_report(s->input->diagnostics, s->input->unit, id, where, message,
        N00BB_ARGS(found, ARG_TEXT(expected), ARG_TEXT(construct)));
    core_free(construct.data);
    core_free(expected.data);
}

static bool
report_unclosed(syntax_t *s)
{
    int32_t chosen = -1;
    for (int32_t i = 0; i < s->count; i++)
        if (is_opener(s, i) && s->match[i] < 0 && s->implicit_close[i] < 0
            && tid_at(s, i) != s->indent)
            chosen = i;
    if (chosen < 0) return false;
    char opener = (char)tid_at(s, chosen);
    char closer = (char)closer_for(s, opener);
    const n00bb_syntax_input_t *in = s->input;
    n00bb_report(in->diagnostics, in->unit, N00B_DIAG_SYNTAX_UNCLOSED_DELIMITER,
        in->end_of_file, N00B_TEXT_UNCLOSED,
        N00BB_ARGS(ARG_CHARACTER(opener)));
    n00bb_report_label(in->diagnostics, N00B_TEXT_THE_FILE_ENDS_BEFORE_IS,
        N00BB_ARGS(ARG_CHARACTER(opener)));
    n00bb_report_secondary(in->diagnostics, nullptr,
        token_location(token_at(s, chosen)), N00B_TEXT_IS_OPENED_HERE,
        N00BB_ARGS(ARG_CHARACTER(opener)));
    for (int32_t i = chosen + 1; i < s->count; i++) {
        if (tid_at(s, i) != opener || s->match[i] < 0) continue;
        int32_t close = s->match[i];
        if (line_indent(s, close) == line_indent(s, i)) continue;
        parse_token_info_t *close_token = token_at(s, close);
        if (parse_token_line(close_token) == parse_token_line(token_at(s, i)))
            continue;
        n00bb_report_secondary(in->diagnostics, nullptr,
            token_location(token_at(s, i)), N00B_TEXT_THIS_MAY_BE_THE_ONE,
            N00BB_ARGS(ARG_CHARACTER(opener), ARG_CHARACTER(closer)));
        n00bb_report_secondary(in->diagnostics, nullptr,
            token_location(close_token), N00B_TEXT_THIS_IS_INDENTED_LIKE_AN,
            N00BB_ARGS(ARG_CHARACTER(closer)));
        break;
    }
    n00bb_report_help(in->diagnostics, N00B_TEXT_ADD_THE_MISSING,
        N00BB_ARGS(ARG_CHARACTER(closer)));
    return true;
}

static bool
report_unmatched_closer(syntax_t *s, int32_t at)
{
    if (!is_closer(s, at) || s->match[at] >= 0 || tid_at(s, at) == s->dedent)
        return false;
    char closer = (char)tid_at(s, at);
    char opener = closer == '}' ? '{' : closer == ')' ? '(' : '[';
    const n00bb_syntax_input_t *in = s->input;
    int32_t open = s->parent[at];
    while (open >= 0 && tid_at(s, open) == s->indent) open = s->parent[open];
    if (open >= 0 && (tid_at(s, open) != '{' || closer == '}')) {
        char other = (char)tid_at(s, open);
        n00bb_report(in->diagnostics, in->unit,
            N00B_DIAG_SYNTAX_UNMATCHED_CLOSER, token_location(token_at(s, at)),
            N00B_TEXT_DOES_NOT_MATCH_THE_OPEN,
            N00BB_ARGS(ARG_CHARACTER(closer), ARG_CHARACTER(other)));
        n00bb_report_label(in->diagnostics, N00B_TEXT_EXPECTED_BEFORE_THIS,
            N00BB_ARGS(ARG_CHARACTER((char)closer_for(s, other))));
        n00bb_report_secondary(in->diagnostics, nullptr,
            token_location(token_at(s, open)), N00B_TEXT_IS_OPENED_HERE,
            N00BB_ARGS(ARG_CHARACTER(other)));
        return true;
    }
    n00bb_report(in->diagnostics, in->unit, N00B_DIAG_SYNTAX_UNMATCHED_CLOSER,
        token_location(token_at(s, at)), N00B_TEXT_UNEXPECTED_WITH_NO_MATCHING,
        N00BB_ARGS(ARG_CHARACTER(closer), ARG_CHARACTER(opener)));
    n00bb_report_label(in->diagnostics, N00B_TEXT_NO_IS_OPEN_HERE,
        N00BB_ARGS(ARG_CHARACTER(opener)));
    return true;
}

static bool
starts_statement(const syntax_t *s, int32_t index, int32_t window_first)
{
    if (index <= window_first) return true;
    int32_t tid = tid_at(s, index - 1);
    return tid == s->newline || tid == ';' || tid == '{' || tid == s->indent
        || tid == s->dedent;
}

static bool
report_keyword_typo(syntax_t *s, parse_pwz_parser_t *parser, int32_t at,
                    int32_t window_first)
{
    int32_t typo = at - 1;
    if (typo < window_first || tid_at(s, typo) != s->identifier
        || !starts_statement(s, typo, window_first))
        return false;
    int32_t next = tid_at(s, at);
    if (next != s->identifier && next != '{' && next != '(') return false;
    core_string_t word = token_source(s, token_at(s, typo));
    if (!parse_pwz_collect_expected(parser, typo)) return false;
    size_t total = parse_pwz_expected_count(parser);
    core_string_t *keywords = core_alloc_array(core_string_t, total + 1);
    size_t found = 0;
    for (size_t i = 0; i < total; i++) {
        const parse_pwz_expected_t *entry = parse_pwz_expected_at(parser, i);
        core_string_t name = entry->spelling.u8_bytes
            ? entry->spelling
            : parse_terminal_name(s->input->grammar, entry->tid);
        if (is_keyword_name(name)) keywords[found++] = name;
    }
    size_t best[1];
    bool close = n00bb_closest_names(word, keywords, found, best, 1) > 0;
    if (close) {
        const n00bb_syntax_input_t *in = s->input;
        n00bb_report(in->diagnostics, in->unit,
            N00B_DIAG_SYNTAX_UNKNOWN_KEYWORD, token_location(token_at(s, typo)),
            N00B_TEXT_UNKNOWN_KEYWORD,
            N00BB_ARGS(ARG_TEXT(word)));
        n00bb_report_label(in->diagnostics, N00B_TEXT_NOT_A_KEYWORD,
            N00BB_NO_ARGS);
        n00bb_diag_did_you_mean(in->diagnostics, word, keywords, found);
    }
    core_free(keywords);
    return close;
}

static bool
report_interleaved_arguments(syntax_t *s, int32_t found)
{
    int32_t open = found < s->count ? s->parent[found] : -1;
    int32_t callee = open > 0 ? tid_at(s, open - 1) : PARSE_TOK_EOF;
    int32_t close = open >= 0 ? s->match[open] : -1;
    if (open <= 0 || tid_at(s, open) != '(' || close < 0
        || (callee != s->identifier && callee != ')' && callee != ']'))
        return false;
    int32_t *starts = core_alloc_array(int32_t, close - open + 1);
    int32_t *ends = core_alloc_array(int32_t, close - open + 1);
    bool *named = core_alloc_array(bool, close - open + 1);
    size_t count = 0, changes = 0, misplaced = 0;
    int32_t start = open + 1;
    for (int32_t i = open + 1; i <= close; i++) {
        if (i < close && !(tid_at(s, i) == ',' && s->parent[i] == open))
            continue;
        if (i > start) {
            starts[count] = start;
            ends[count] = i - 1;
            named[count] = tid_at(s, start) == s->identifier
                && core_string_eq(token_source(s, token_at(s, start + 1)),
                                  CORE_STRING_STATIC("="));
            if (count && named[count] != named[count - 1]
                && ++changes == 2) misplaced = count;
            count++;
        }
        start = i + 1;
    }
    const n00bb_syntax_input_t *in = s->input;
    if (changes > 1 && n00bb_report(in->diagnostics, in->unit,
            N00B_DIAG_SYNTAX_INTERLEAVED_ARGUMENTS,
            token_location(token_at(s, starts[misplaced])),
            N00B_TEXT_INTERLEAVED_ARGUMENTS, N00BB_NO_ARGS)) {
        n00bb_report_help(in->diagnostics,
            N00B_TEXT_INTERLEAVED_ARGUMENTS_HELP, N00BB_NO_ARGS);
        core_buffer_t *text = core_buffer_empty();
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i < count; i++) {
                if (named[i] != (pass == 1)) continue;
                size_t first = parse_token_start_byte(token_at(s, starts[i]));
                size_t last = parse_token_end_byte(token_at(s, ends[i]));
                if (text->byte_len) core_buffer_puts(text, ", ");
                core_buffer_append(text, in->text.data + first, last - first);
            }
        core_string_t replacement = core_buffer_take_string(text);
        n00bb_report_fix(in->diagnostics, in->unit, &(n00bb_edit_t){
            parse_token_start_byte(token_at(s, starts[0])),
            parse_token_end_byte(token_at(s, ends[count - 1])),
            replacement}, 1);
        core_free(replacement.data);
    }
    core_free(starts);
    core_free(ends);
    core_free(named);
    return changes > 1;
}

static bool
previous_is_comptime(const syntax_t *s, int32_t at)
{
    parse_token_info_t *before = token_at(s, at - 1);
    return before && tid_at(s, at) == '{'
        && text_is(token_source(s, before), "comptime");
}

static void
report_error(syntax_t *s, parse_pwz_parser_t *parser, int32_t at,
             int32_t window_first, int32_t window_end)
{
    const n00bb_syntax_input_t *in = s->input;
    bool unterminated = parse_pwz_error_unterminated(parser);
    bool commented = parse_pwz_error_commented(parser);
    int32_t found = at < window_end ? at
        : window_end < s->count ? window_end : s->count;
    parse_pwz_collect_expected(parser, at);
    mark_statement_blocks(s, parser);

    if (found >= s->count) {
        if (!report_unclosed(s))
            report_unexpected(s, parser, at, N00B_DIAG_SYNTAX_UNEXPECTED_END,
                              in->end_of_file, ARG_PHRASE(END_OF_FILE));
    }
    else if (report_unmatched_closer(s, found)) {}
    else if ((unterminated || commented) && found > window_first) {
        report_unexpected(s, parser, at,
            unterminated ? N00B_DIAG_SYNTAX_BLANK_LINE_IN_STATEMENT
                         : N00B_DIAG_SYNTAX_COMMENT_IN_STATEMENT,
            token_location(token_at(s, found - 1)),
            unterminated ? ARG_PHRASE(A_BLANK_LINE)
                         : ARG_PHRASE(A_LINE_BREAK_AFTER_COMMENT));
        n00bb_report_note(in->diagnostics, unterminated
            ? N00B_TEXT_BLANK_LINE_ENDS_STATEMENT
            : N00B_TEXT_COMMENT_ENDS_STATEMENT, N00BB_NO_ARGS);
    }
    else if (report_keyword_typo(s, parser, at, window_first)) {}
    else if (report_interleaved_arguments(s, found)) {}
    else if (tid_at(s, found) == s->indent) {
        report_unexpected(s, parser, at, N00B_DIAG_SYNTAX_UNEXPECTED_INDENT,
            token_location(token_at(s, found)), ARG_PHRASE(INDENTATION));
        n00bb_report_help(in->diagnostics, N00B_TEXT_INDENT_THIS_LINE_LIKE_THE,
            N00BB_NO_ARGS);
    }
    else if (tid_at(s, found) == s->pass) {
        n00bb_report(in->diagnostics, in->unit, N00B_DIAG_SYNTAX_MISPLACED_PASS,
            token_location(token_at(s, found)),
            N00B_TEXT_PASS_CANNOT_APPEAR_HERE,
            N00BB_NO_ARGS);
        n00bb_report_note(in->diagnostics, N00B_TEXT_PASS_MAY_ONLY_STAND_FOR,
            N00BB_NO_ARGS);
    }
    else if (previous_is_comptime(s, found)) {
        n00bb_report(in->diagnostics, in->unit, N00B_DIAG_SYNTAX_COMPTIME_BLOCK,
            token_location(token_at(s, found - 1)),
            N00B_TEXT_COMPTIME_IS_A_FUNCTION_NOT,
            N00BB_NO_ARGS);
        n00bb_report_help(in->diagnostics, N00B_TEXT_WRITE_IF_COMPTIME_ELSE,
            N00BB_NO_ARGS);
    }
    else {
        int32_t reported_at = found;
        if (found > window_first + 1 && tid_at(s, found - 1) == s->newline
            && inside_statement(parser, found - 1))
            reported_at = found - 1;
        core_diag_location_t where = token_location(token_at(s, reported_at));
        parse_token_info_t *before = token_at(s, reported_at - 1);
        if (tid_at(s, reported_at) == s->newline && before)
            where = (core_diag_location_t){
                .line = parse_token_endline(before),
                .column = parse_token_endcol(before),
                .end_line = parse_token_endline(before),
                .end_column = parse_token_endcol(before),
                .has_location = true,
            };
        core_string_t found_text = describe_found(s, reported_at);
        report_unexpected(s, parser, at, N00B_DIAG_SYNTAX_UNEXPECTED_TOKEN,
                          where, ARG_TEXT(found_text));
        core_free(found_text.data);
        int32_t open = s->parent[found];
        if (open >= 0 && s->match[open] < 0
            && (tid_at(s, open) == '(' || tid_at(s, open) == '['))
            n00bb_report_secondary(in->diagnostics, nullptr,
                token_location(token_at(s, open)),
                N00B_TEXT_IS_OPENED_HERE_AND_NEVER,
                N00BB_ARGS(ARG_CHARACTER((char)tid_at(s, open))));
        int32_t unclosed = -1;
        for (int32_t i = 0; s->resumes[found] && i < found; i++)
            if (s->implicit_close[i] == found) unclosed = i;
        if (unclosed >= 0) {
            n00bb_report_secondary(in->diagnostics, nullptr,
                token_location(token_at(s, unclosed)),
                N00B_TEXT_THIS_IS_NEVER_CLOSED,
                N00BB_NO_ARGS);
            n00bb_report_help(in->diagnostics,
                N00B_TEXT_ADD_THE_MISSING_BEFORE_THIS,
                N00BB_NO_ARGS);
        }
    }
    s->reported++;
}

static int32_t
statement_after(const syntax_t *s, int32_t from, int32_t block)
{
    int32_t limit = block >= 0 && s->match[block] >= 0 ? s->match[block]
                                                       : s->count;
    int32_t j = from;
    while (j < limit) {
        if (is_statement_end(s, j) && s->parent[j] == block) return j + 1;
        if (is_opener(s, j) && j != from) {
            if (s->match[j] < 0) {
                if (tid_at(s, j) == '{' || tid_at(s, j) == s->indent)
                    return -1;
                j++;
                continue;
            }
            j = s->match[j] + 1;
            continue;
        }
        j++;
    }
    return limit;
}

static int32_t
climb_to_block(syntax_t *s, int32_t *from, bool *exact)
{
    int32_t block = *from < s->count ? s->parent[*from] : -1;
    *exact = *from < s->count && s->resumes[*from] && s->resumed != *from;
    while (block >= 0
           && (!s->statement_block[block] || s->implicit_close[block] >= 0)) {
        int32_t tid = tid_at(s, block);
        if (s->implicit_close[block] >= 0) {
            *from = s->implicit_close[block];
            *exact = true;
            block = s->parent[*from];
            continue;
        }
        if (s->match[block] < 0 && tid != '(' && tid != '[') return -2;
        if (s->match[block] >= 0) *from = s->match[block];
        block = s->parent[block];
    }
    return block;
}

static bool
next_window(syntax_t *s, int32_t *from, int32_t *block,
            int32_t *first, int32_t *end)
{
    for (;;) {
        if (*from >= s->count) return false;
        bool exact = false;
        *block = climb_to_block(s, from, &exact);
        if (*block == -2) return false;
        if (exact) s->resumed = *from;
        int32_t start = exact ? *from : statement_after(s, *from, *block);
        if (start < 0) return false;
        int32_t limit = *block >= 0 && s->match[*block] >= 0
            ? s->match[*block] : s->count;
        while (start < limit && is_statement_end(s, start)) start++;
        int32_t stop = limit;
        while (stop > start && is_statement_end(s, stop - 1)) stop--;
        if (start < stop) {
            *first = start;
            *end = stop;
            return true;
        }
        if (*block < 0 || s->match[*block] < 0) return false;
        *from = s->match[*block];
    }
}

static parse_pwz_parser_t *
window_parser(syntax_t *s, bool block)
{
    parse_pwz_parser_t **slot = block ? &s->block_parser : &s->module_parser;
    if (*slot) return *slot;
    parse_nonterm_t *rule = parse_find_nonterm(
        s->input->grammar, block ? CORE_STRING_STATIC("body-stmts")
                                 : CORE_STRING_STATIC("module"));
    if (!rule) return nullptr;
    *slot = parse_pwz_new_for_start(s->input->grammar,
                                    parse_nonterm_id(rule));
    if (*slot)
        parse_pwz_set_disambiguator(*slot, s->input->disambiguator, nullptr);
    return *slot;
}

void
n00bb_report_syntax_errors(const n00bb_syntax_input_t *input)
{
    syntax_t s = {
        .input = input,
        .count = scan_stream_token_count(input->tokens),
    };
    parse_grammar_t *g = input->grammar;
    s.newline = parse_terminal_id(g, CORE_STRING_STATIC("NEWLINE"));
    s.indent = parse_terminal_id(g, CORE_STRING_STATIC("INDENT"));
    s.dedent = parse_terminal_id(g, CORE_STRING_STATIC("DEDENT"));
    s.identifier = parse_terminal_id(g, CORE_STRING_STATIC("IDENTIFIER"));
    s.pass = parse_terminal_id(g, CORE_STRING_STATIC("pass"));
    s.match = core_alloc_array(int32_t, (size_t)s.count + 1);
    s.implicit_close = core_alloc_array(int32_t, (size_t)s.count + 1);
    s.resumes = core_alloc_array(bool, (size_t)s.count + 1);
    s.resumed = -1;
    s.parent = core_alloc_array(int32_t, (size_t)s.count + 1);
    s.statement_block = core_alloc_array(bool, (size_t)s.count + 1);
    match_brackets(&s);

    parse_pwz_parser_t *parser = input->parser;
    int32_t first = 0;
    int32_t end = s.count;
    int32_t at = parse_pwz_error_pos(parser);
    while (at >= 0 && s.reported < SYNTAX_ERROR_LIMIT) {
        report_error(&s, parser, at, first, end);
        int32_t from = at < end ? at : end;
        if (from >= s.count) break;
        if (is_closer(&s, from) && s.match[from] < 0) from++;
        bool parsed = true;
        int32_t block = -1;
        while (parsed) {
            if (!next_window(&s, &from, &block, &first, &end)) {
                at = -1;
                break;
            }
            parser = window_parser(&s, block >= 0);
            if (!parser) {
                at = -1;
                break;
            }
            parsed = parse_pwz_parse_range(parser, input->tokens, first, end);
            if (parsed) {
                parse_pwz_release_parse_state(parser);
                parse_pwz_free(parser);
                if (block >= 0) s.block_parser = nullptr;
                else s.module_parser = nullptr;
                from = end;
            }
            else at = parse_pwz_error_pos(parser);
        }
    }
    parse_pwz_free(s.module_parser);
    parse_pwz_free(s.block_parser);
    core_free(s.statement_block);
    core_free(s.parent);
    core_free(s.resumes);
    core_free(s.implicit_close);
    core_free(s.match);
}

static core_diag_location_t
byte_span(core_string_t text, size_t start, size_t end)
{
    if (start > text.u8_bytes) start = text.u8_bytes;
    if (end > text.u8_bytes) end = text.u8_bytes;
    if (end < start) end = start;
    return core_diag_location_span(text, start, end);
}

static size_t
codepoint_end(core_string_t text, size_t start)
{
    if (start >= text.u8_bytes) return start;
    uint32_t at = (uint32_t)start;
    if (unicode_utf8_decode(text.data, (uint32_t)text.u8_bytes, &at) < 0)
        return start + 1;
    return at;
}

static core_diag_location_t
end_of_text(core_string_t text)
{
    return byte_span(text, text.u8_bytes, text.u8_bytes);
}

void
n00bb_report_lexical_error(core_diagnostics_t *d,
                           const core_translation_unit_t *unit,
                           core_string_t text,
                           const scan_diagnostic_t *diagnostic)
{
    scan_problem_t problem = diagnostic->problem;
    size_t start = diagnostic->start_byte;
    size_t end = diagnostic->end_byte;
    core_diag_location_t span = byte_span(text, start, end);

    if (problem.kind == SCAN_DIAG_STRING_NEWLINE
        || problem.kind == SCAN_DIAG_STRING_UNTERMINATED) {
        bool character = start < text.u8_bytes && text.data[start] == '\'';
        bool at_line_end = problem.kind == SCAN_DIAG_STRING_NEWLINE;
        char quote = character ? '\'' : '"';
        n00bb_report(d, unit, N00B_DIAG_LEX_UNTERMINATED_STRING,
            at_line_end ? byte_span(text, end, end) : end_of_text(text),
            character ? N00B_TEXT_UNTERMINATED_CHARACTER
                      : N00B_TEXT_UNTERMINATED_STRING, N00BB_NO_ARGS);
        n00bb_report_label(d, at_line_end ? N00B_TEXT_LINE_ENDS_BEFORE_QUOTE
                                          : N00B_TEXT_FILE_ENDS_BEFORE_QUOTE,
                           N00BB_ARGS(ARG_CHARACTER(quote)));
        n00bb_report_secondary(d, nullptr, byte_span(text, start, start + 1),
            character ? N00B_TEXT_LITERAL_STARTS_HERE
                      : N00B_TEXT_STRING_STARTS_HERE, N00BB_NO_ARGS);
        n00bb_report_help(d, N00B_TEXT_ADD_THE_CLOSING,
            N00BB_ARGS(ARG_CHARACTER(quote)));
        return;
    }
    if (problem.kind == SCAN_DIAG_BLOCK_COMMENT_UNTERMINATED) {
        n00bb_report(d, unit, N00B_DIAG_LEX_UNTERMINATED_COMMENT,
            end_of_text(text), N00B_TEXT_UNTERMINATED_BLOCK_COMMENT,
            N00BB_NO_ARGS);
        n00bb_report_label(d, N00B_TEXT_THE_FILE_ENDS_BEFORE, N00BB_NO_ARGS);
        n00bb_report_secondary(d, nullptr, byte_span(text, start, start + 2),
            N00B_TEXT_THE_COMMENT_STARTS_HERE,
            N00BB_NO_ARGS);
        return;
    }
    if (problem.kind == SCAN_DIAG_STRING_INVALID_ESCAPE) {
        size_t escape = end;
        while (escape > start && text.data[escape - 1] != '\\') escape--;
        if (escape > start) escape--;
        size_t stop = codepoint_end(text, escape + 1);
        n00bb_report(d, unit, N00B_DIAG_LEX_INVALID_ESCAPE,
            byte_span(text, escape, stop), N00B_TEXT_INVALID_ESCAPE_IN_STRING,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(text.data + escape,
                (size_t)((stop - escape))))));
        n00bb_report_help(d, N00B_TEXT_WRITE_A_BACKSLASH_AS_SEE, N00BB_NO_ARGS);
        return;
    }
    if (problem.kind == SCAN_DIAG_NUMBER_MALFORMED) {
        size_t stop = end;
        while (stop < text.u8_bytes
               && ((text.data[stop] >= 'a' && text.data[stop] <= 'z')
                   || (text.data[stop] >= 'A' && text.data[stop] <= 'Z')
                   || (text.data[stop] >= '0' && text.data[stop] <= '9')
                   || text.data[stop] == '_'))
            stop++;
        core_string_t number = core_string_slice(text, start, stop - start);
        n00bb_report(d, unit, N00B_DIAG_LEX_MALFORMED_NUMBER,
            byte_span(text, start, stop), N00B_TEXT_MALFORMED_NUMBER,
            N00BB_ARGS(ARG_TEXT(number)));
        return;
    }
    if (problem.kind == SCAN_DIAG_IDENTIFIER_NOT_NFC
        || problem.kind == SCAN_DIAG_IDENTIFIER_NOT_NFKC) {
        core_string_t suggestion = problem.detail;
        bool compat = problem.kind == SCAN_DIAG_IDENTIFIER_NOT_NFKC;
        n00bb_report(d, unit, N00B_DIAG_LEX_IDENTIFIER_NOT_NFC, span,
            N00B_TEXT_IDENTIFIER_NOT_NORMALIZED,
            N00BB_ARGS(ARG_TEXT(compat ? CORE_STRING_STATIC("NFKC")
                                       : CORE_STRING_STATIC("NFC"))));
        if (suggestion.u8_bytes)
            n00bb_report_help(d, N00B_TEXT_WRITE_IT_AS,
                N00BB_ARGS(ARG_TEXT(suggestion)));
        return;
    }
    n00bb_text_t script_text =
          problem.kind == SCAN_DIAG_IDENTIFIER_MIXED_SCRIPTS
            ? N00B_TEXT_IDENTIFIER_MIXES_SCRIPTS
        : problem.kind == SCAN_DIAG_IDENTIFIER_DISALLOWED_SCRIPT
            ? N00B_TEXT_IDENTIFIER_DISALLOWED_SCRIPT
        : problem.kind == SCAN_DIAG_IDENTIFIER_EMOJI
            ? N00B_TEXT_IDENTIFIER_EMOJI
        : problem.kind == SCAN_DIAG_IDENTIFIER_INVALID_UTF8
            ? N00B_TEXT_IDENTIFIER_INVALID_UTF8
        : problem.kind == SCAN_DIAG_IDENTIFIER_CONFUSABLE
            ? N00B_TEXT_IDENTIFIER_CONFUSABLE : N00B_TEXT_COUNT;
    if (script_text != N00B_TEXT_COUNT) {
        n00bb_report(d, unit, N00B_DIAG_LEX_IDENTIFIER_SCRIPT, span,
            script_text,
                     N00BB_NO_ARGS);
        return;
    }
    if (problem.kind == SCAN_DIAG_TABS_NOT_ALLOWED
        || problem.kind == SCAN_DIAG_TABS_IN_INDENTATION
        || problem.kind == SCAN_DIAG_MIXED_INDENTATION) {
        n00bb_report(d, unit, N00B_DIAG_LEX_TAB_INDENT, span,
            problem.kind == SCAN_DIAG_MIXED_INDENTATION
                ? N00B_TEXT_INDENTS_WITH_TABS_AND_SPACES
                : N00B_TEXT_INDENTS_WITH_TAB, N00BB_NO_ARGS);
        n00bb_report_help(d, N00B_TEXT_INDENT_WITH_SPACES_ONLY, N00BB_NO_ARGS);
        return;
    }
    if (problem.kind == SCAN_DIAG_DEDENT_MISMATCH
        || problem.kind == SCAN_DIAG_ALIGNMENT_INDENTATION) {
        n00bb_report(d, unit, N00B_DIAG_LEX_INCONSISTENT_DEDENT, span,
            N00B_TEXT_THIS_LINE_S_INDENTATION_MATCHES,
            N00BB_NO_ARGS);
        n00bb_report_help(d, N00B_TEXT_LINE_IT_UP_WITH_THE, N00BB_NO_ARGS);
        return;
    }
    if (problem.kind == SCAN_DIAG_UNEXPECTED_CHARACTER) {
        size_t stop = codepoint_end(text, start);
        uint32_t at = (uint32_t)start;
        int32_t codepoint = unicode_utf8_decode(
            text.data, (uint32_t)text.u8_bytes, &at);
        n00bb_report(d, unit, N00B_DIAG_LEX_UNEXPECTED_CHARACTER,
            byte_span(text, start, stop), N00B_TEXT_UNEXPECTED_CHARACTER_U,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(text.data + start,
                (size_t)((stop - start)))),
            ARG_CODEPOINT(codepoint < 0 ? 0xFFFD : (unsigned)codepoint)));
        return;
    }
    if (problem.kind == SCAN_DIAG_CHARACTER_LENGTH) {
        n00bb_report(d, unit, N00B_DIAG_LEX_CHARACTER_LITERAL_LENGTH, span,
            N00B_TEXT_A_CHARACTER_LITERAL_MUST_HOLD,
            N00BB_NO_ARGS);
        n00bb_report_help(d, N00B_TEXT_USE_DOUBLE_QUOTES_FOR_A, N00BB_NO_ARGS);
        return;
    }
    if (problem.kind == SCAN_DIAG_BIDI_CONTROL) {
        uint32_t at = (uint32_t)start;
        int32_t codepoint = unicode_utf8_decode(
            text.data, (uint32_t)text.u8_bytes, &at);
        n00bb_report(d, unit, N00B_DIAG_LEX_BIDI_CONTROL,
            byte_span(text, start, at),
            N00B_TEXT_INVISIBLE_BIDIRECTIONAL_CONTROL_CHARACTER_U,
            N00BB_ARGS(ARG_CODEPOINT(codepoint < 0 ? 0xFFFD
                                     : (unsigned)codepoint)));
        n00bb_report_note(d, N00B_TEXT_IT_CAN_MAKE_CODE_DISPLAY, N00BB_NO_ARGS);
        n00bb_report_help(d, N00B_TEXT_INSIDE_A_STRING_LITERAL_WRITE,
            N00BB_ARGS(ARG_CODEPOINT(codepoint < 0 ? 0xFFFD
                                     : (unsigned)codepoint)));
        return;
    }
    if (problem.kind == SCAN_DIAG_FENCE_UNTERMINATED
        || problem.kind == SCAN_DIAG_FENCE_OPENER_INCOMPLETE
        || problem.kind == SCAN_DIAG_EMBED_UNTERMINATED) {
        n00bb_report(d, unit, N00B_DIAG_LEX_UNTERMINATED_FENCE,
            end_of_text(text), N00B_TEXT_UNTERMINATED_FENCED_TEXT_BLOCK,
            N00BB_NO_ARGS);
        n00bb_report_label(d, N00B_TEXT_THE_FILE_ENDS_BEFORE_THE,
            N00BB_NO_ARGS);
        n00bb_report_secondary(d, nullptr, byte_span(text, start, start + 1),
            N00B_TEXT_THE_BLOCK_STARTS_HERE,
            N00BB_NO_ARGS);
        return;
    }
    n00bb_report(d, unit, N00B_DIAG_LEX_INVALID_TOKEN, span,
                 n00bb_scan_text(problem.kind),
                 N00BB_ARGS(ARG_TEXT(problem.detail)));
}

void
n00bb_report_lexical_warning(core_diagnostics_t *d,
                             const core_translation_unit_t *unit,
                             core_string_t text,
                             const scan_warning_t *warning)
{
    core_diag_location_t span = byte_span(text, warning->start_byte,
                                          warning->end_byte);
    if (warning->problem.kind == SCAN_DIAG_FENCE_BIDI_CONTROL) {
        uint32_t at = (uint32_t)warning->start_byte;
        int32_t codepoint = at < text.u8_bytes
            ? unicode_utf8_decode(text.data, (uint32_t)text.u8_bytes, &at)
            : -1;
        n00bb_report(d, unit, N00B_DIAG_LEX_BIDI_CONTROL_IN_FENCE, span,
            N00B_TEXT_FENCED_TEXT_BLOCK_CONTAINS_INVISIBLE,
            N00BB_ARGS(ARG_CODEPOINT(codepoint < 0 ? 0xFFFD
                                     : (unsigned)codepoint)));
        n00bb_report_note(d, N00B_TEXT_IT_CAN_MAKE_THE_TEXT, N00BB_NO_ARGS);
        return;
    }
    n00bb_report(d, unit, N00B_DIAG_LEX_SOURCE_WARNING, span,
                 n00bb_scan_text(warning->problem.kind),
                 N00BB_ARGS(ARG_TEXT(warning->problem.detail)));
}
