#include <stdio.h>
#include <stdlib.h>

#include "syntax.h"

#include "parse/ambiguity.h"
#include "parse/bnf.h"
#include "parse/forest.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree.h"
#include "scanner/lex.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"
#include "json_grammar.h"
#include "writ_grammar.h"
#include "writ_keywords.h"

typedef enum {
    WRIT_FORM_NODE,
    WRIT_FORM_CHAIN,
    WRIT_FORM_WRAP,
    WRIT_FORM_LEXICAL,
} writ_form_t;

typedef enum {
    WRIT_NAME_NONE,
    WRIT_NAME_TOKEN,
    WRIT_NAME_STRING,
    WRIT_NAME_INNER,
    WRIT_NAME_TEXT,
} writ_name_form_t;

typedef struct {
    const char      *nonterminal;
    writ_node_kind_t kind;
    writ_form_t      form;
    writ_name_form_t name_form;
    uint32_t         name_child;
} syntax_rule_t;

static const syntax_rule_t syntax_rules[] = {
#define WRIT_KIND(id, name)
#define WRIT_SYNTAX(nt, kind, form, name_form, name_child) {nt, WRIT_N_##kind, form, name_form, name_child},
#include "ast_nodes.def"
#undef WRIT_KIND
#undef WRIT_SYNTAX
};

static const scan_lex_comment_t lexer_comments[] = {
    {.kind = SCAN_LEX_LINE, .opener = CORE_STRING_STATIC("#")},
    {0},
};

static const scan_lex_string_t lexer_strings[] = {
    {.opener = CORE_STRING_STATIC("\""), .closer = CORE_STRING_STATIC("\""), .escape = CORE_STRING_STATIC("\\"),
     .tok = CORE_STRING_STATIC("STRING")},
    {0},
};

static const scan_lex_number_form_t lexer_numbers[] = {
    {.digits = CORE_STRING_STATIC("0123456789"), .separator = CORE_STRING_STATIC("_"),
     .tok_int = CORE_STRING_STATIC("INTEGER")},
    {.prefix = CORE_STRING_STATIC("0x"), .digits = CORE_STRING_STATIC("0123456789abcdefABCDEF"),
     .separator = CORE_STRING_STATIC("_"), .tok_int = CORE_STRING_STATIC("INTEGER"),
     .flags = SCAN_LEX_NUM_SEPARATOR_AFTER_PREFIX},
    {0},
};

static const scan_lex_sigil_t lexer_sigils[] = {
    {.prefix = CORE_STRING_STATIC("'"), .tok = CORE_STRING_STATIC("LIFETIME")},
    {0},
};

static const scan_lex_spec_t writ_lexer = {
    .name          = CORE_STRING_STATIC("writ"),
    .comments      = lexer_comments,
    .strings       = lexer_strings,
    .sigils        = lexer_sigils,
    .number_forms  = lexer_numbers,
    .soft_keywords = writ_soft_keywords,
    .tok_ident     = CORE_STRING_STATIC("IDENTIFIER"),
    .brackets      = CORE_STRING_STATIC("()[]{}"),
    .fallback      = SCAN_LEX_FALLBACK_REJECT,
};

static const scan_lex_spec_t json_lexer = {
    .name         = CORE_STRING_STATIC("json"),
    .strings      = lexer_strings,
    .number_forms = lexer_numbers,
    .tok_ident    = CORE_STRING_STATIC("IDENTIFIER"),
    .brackets     = CORE_STRING_STATIC("[]{}"),
    .fallback     = SCAN_LEX_FALLBACK_REJECT,
};

typedef struct {
    parse_grammar_t *grammar;
    core_dict_t      rule_by_id;
    parse_nt_id_t    file_start;
    parse_nt_id_t    declarations_start;
    parse_nt_id_t    embedded_start;
} writ_grammar_t;

static void
grammar_release(void *object)
{
    writ_grammar_t *grammar = object;
    core_dict_free(&grammar->rule_by_id);
    parse_grammar_free(grammar->grammar);
}

static parse_nt_id_t
nonterminal_id(parse_grammar_t *grammar, const char *name)
{
    parse_nonterm_t *nonterminal = parse_find_nonterm(grammar, core_string_borrow(name, strlen(name)));
    return nonterminal ? parse_nonterm_id(nonterminal) : -1;
}

typedef struct {
    core_string_t           text;
    const scan_lex_spec_t  *lexer;
    const char             *file;
    const char             *declarations;
    const char             *embedded;
} grammar_spec_t;

static const grammar_spec_t grammar_specs[] = {
    [WRIT_GRAMMAR_WRIT] = {{.data = (char *)writ_grammar_text, .u8_bytes = sizeof(writ_grammar_text) - 1}, &writ_lexer, "file", "declarations", "embedded"},
    [WRIT_GRAMMAR_JSON] = {{.data = (char *)json_grammar_text, .u8_bytes = sizeof(json_grammar_text) - 1}, &json_lexer, "json-file", "json-file", "json-file"},
};

static writ_grammar_t *
grammar_for(writ_session_t *session, writ_grammar_id_t which)
{
    if (session->grammars[which]) {
        return session->grammars[which];
    }
    const grammar_spec_t *spec = &grammar_specs[which];
    writ_grammar_t *grammar = writ_alloc(session, sizeof(writ_grammar_t));
    grammar->grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar->grammar, false);
    parse_bnf_declare_terminals(grammar->grammar, spec->text);
    if (!parse_bnf_load(spec->text, core_string_borrow(spec->file, strlen(spec->file)), grammar->grammar)
        || !scan_grammar_set_lex(grammar->grammar, spec->lexer)) {
        parse_grammar_free(grammar->grammar);
        return nullptr;
    }
    core_dict_init(&grammar->rule_by_id, nullptr, nullptr);
    for (size_t i = 0; i < sizeof(syntax_rules) / sizeof(syntax_rules[0]); i++) {
        parse_nt_id_t id = nonterminal_id(grammar->grammar, syntax_rules[i].nonterminal);
        if (id >= 0) {
            core_dict_put(&grammar->rule_by_id, id + 1, i + 1);
        }
    }
    grammar->file_start         = nonterminal_id(grammar->grammar, spec->file);
    grammar->declarations_start = nonterminal_id(grammar->grammar, spec->declarations);
    grammar->embedded_start     = nonterminal_id(grammar->grammar, spec->embedded);
    session->grammars[which]    = grammar;
    writ_on_session_free(session, grammar_release, grammar);
    return grammar;
}

typedef struct {
    writ_unit_t          *unit;
    writ_session_t       *session;
    writ_grammar_t       *grammar;
    uint32_t              source;
    core_list_t(uint32_t) scratch;
} converter_t;

static writ_loc_t
token_loc(const converter_t *converter, const parse_token_info_t *token)
{
    return (writ_loc_t){converter->source, (uint32_t)parse_token_start_byte(token), (uint32_t)parse_token_end_byte(token)};
}

static writ_sym_t
text_sym(converter_t *converter, writ_loc_t loc)
{
    return writ_intern_str(converter->session, writ_loc_text(converter->session, loc));
}

static writ_sym_t
unescaped_sym(converter_t *converter, writ_loc_t loc)
{
    writ_str_t quoted = writ_loc_text(converter->session, loc);
    core_buffer_t *buffer = core_buffer_empty();
    for (size_t i = 1; i + 1 < quoted.length; i++) {
        char c = quoted.data[i];
        if (c == '\\' && i + 2 < quoted.length) {
            c = quoted.data[++i];
            if (c == 'u' && i + 5 < quoted.length) {
                uint32_t point = (uint32_t)strtoul((char[5]){quoted.data[i + 1], quoted.data[i + 2],
                                                            quoted.data[i + 3], quoted.data[i + 4], 0},
                                                   nullptr, 16);
                i += 4;
                if (point >= 0x800) {
                    core_buffer_putc(buffer, (char)(0xe0 | point >> 12));
                    core_buffer_putc(buffer, (char)(0x80 | (point >> 6 & 0x3f)));
                } else if (point >= 0x80) {
                    core_buffer_putc(buffer, (char)(0xc0 | point >> 6));
                }
                core_buffer_putc(buffer, (char)(point >= 0x80 ? 0x80 | (point & 0x3f) : point));
                continue;
            }
            c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == '0' ? '\0' : c;
        }
        core_buffer_putc(buffer, c);
    }
    writ_sym_t sym = writ_intern(converter->session, buffer->data ? buffer->data : "", buffer->byte_len);
    core_buffer_free(buffer);
    return sym;
}

static const syntax_rule_t *
rule_of(const converter_t *converter, parse_tree_t *tree)
{
    bool found = false;
    uintptr_t index = (uintptr_t)core_dict_get(&converter->grammar->rule_by_id,
                                               core_tree_node_value(tree).id + 1, &found);
    return found ? &syntax_rules[index - 1] : nullptr;
}

static writ_sym_t
compact_sym(converter_t *converter, writ_loc_t loc)
{
    writ_str_t text = writ_loc_text(converter->session, loc);
    core_buffer_t *buffer = core_buffer_empty();
    for (size_t i = 0; i < text.length; i++) {
        if (text.data[i] != ' ' && text.data[i] != '\t' && text.data[i] != '\n' && text.data[i] != '\r') {
            core_buffer_putc(buffer, text.data[i]);
        }
    }
    writ_sym_t sym = writ_intern(converter->session, buffer->data ? buffer->data : "", buffer->byte_len);
    core_buffer_free(buffer);
    return sym;
}

static writ_sym_t
node_name(converter_t *converter, parse_tree_t *tree, const syntax_rule_t *rule, writ_loc_t span)
{
    size_t count = core_tree_num_children(tree);
    if (rule->name_form == WRIT_NAME_TEXT) {
        return compact_sym(converter, span);
    }
    if (rule->name_form == WRIT_NAME_NONE || rule->name_child >= count) {
        return 0;
    }
    if (rule->name_form == WRIT_NAME_INNER) {
        parse_token_info_t *open = parse_node_token(core_tree_child(tree, 0));
        parse_token_info_t *close = parse_node_token(core_tree_child(tree, count - 1));
        return text_sym(converter, (writ_loc_t){converter->source, (uint32_t)parse_token_end_byte(open),
                                                (uint32_t)parse_token_start_byte(close)});
    }
    parse_tree_t *child = core_tree_child(tree, rule->name_child);
    if (!parse_node_is_token(child)) {
        return 0;
    }
    writ_loc_t loc = token_loc(converter, parse_node_token(child));
    return rule->name_form == WRIT_NAME_STRING ? unescaped_sym(converter, loc) : text_sym(converter, loc);
}

static bool
convert(converter_t *converter, parse_tree_t *tree, writ_loc_t *loc, writ_sym_t *lexeme_out)
{
    if (parse_node_is_token(tree)) {
        *loc = token_loc(converter, parse_node_token(tree));
        core_list_push(converter->unit->tokens, *loc);
        return true;
    }
    const syntax_rule_t *rule = rule_of(converter, tree);
    size_t count = core_tree_num_children(tree);
    size_t mark = converter->scratch.len;
    bool has_span = false;
    writ_loc_t span = {0};
    writ_sym_t lexeme = 0;
    for (size_t i = 0; i < count; i++) {
        writ_loc_t child_loc;
        writ_sym_t child_lexeme = 0;
        if (!convert(converter, core_tree_child(tree, i), &child_loc, &child_lexeme)) {
            continue;
        }
        span = has_span ? writ_loc_join(span, child_loc) : child_loc;
        has_span = true;
        if (rule && rule->form == WRIT_FORM_CHAIN && i == 1) {
            child_lexeme = text_sym(converter, child_loc);
        }
        if (!lexeme) {
            lexeme = child_lexeme;
        }
    }
    if (rule && rule->form == WRIT_FORM_LEXICAL) {
        *lexeme_out = has_span ? text_sym(converter, span) : 0;
        *loc = span;
        return has_span;
    }
    if (!rule || (rule->form == WRIT_FORM_CHAIN && count == 1)
        || (rule->form == WRIT_FORM_WRAP && converter->scratch.len - mark <= 1)) {
        *lexeme_out = lexeme;
        *loc = span;
        return has_span;
    }
    writ_unit_t *unit = converter->unit;
    writ_node_t node = {
        .kind        = rule->kind,
        .loc         = span,
        .first_child = (uint32_t)unit->children.len,
        .child_count = (uint32_t)(converter->scratch.len - mark),
        .name        = node_name(converter, tree, rule, span),
        .payload     = lexeme,
    };
    for (size_t i = mark; i < converter->scratch.len; i++) {
        core_list_push(unit->children, converter->scratch.data[i]);
    }
    converter->scratch.len = mark;
    core_list_push(unit->nodes, node);
    core_list_push(converter->scratch, (uint32_t)unit->nodes.len - 1);
    *loc = span;
    return true;
}

static void
report_syntax_error(writ_session_t *session, uint32_t source, parse_token_stream_t *stream, int32_t position,
                    parse_scanner_t *scanner, bool embedded)
{
    scan_diagnostic_t lexical;
    if (scanner && scan_status(scanner) == SCAN_STATUS_INVALID && scan_diagnostic(scanner, &lexical)) {
        writ_report(session, WRIT_D_LEXICAL, (writ_loc_t){source, (uint32_t)lexical.start_byte, (uint32_t)lexical.end_byte},
                    "%.*s", (int)lexical.message.u8_bytes, lexical.message.data);
        return;
    }
    scan_stream_reset(stream);
    parse_token_info_t *token = position >= 0 ? scan_stream_get(stream, position) : nullptr;
    const writ_source_t *text = writ_source_at(session, source);
    if (!token) {
        uint32_t end = (uint32_t)text->text.length;
        writ_report(session, WRIT_D_SYNTAX, (writ_loc_t){source, end, end}, "unexpected end of input");
        return;
    }
    writ_loc_t loc = {source, (uint32_t)parse_token_start_byte(token), (uint32_t)parse_token_end_byte(token)};
    writ_str_t spelling = writ_loc_text(session, loc);
    bool second_binding = false;
    for (int32_t i = 0; embedded && writ_str_eq(spelling, writ_cstr("binding")) && i < position; i++) {
        parse_token_info_t *earlier = scan_stream_get(stream, i);
        second_binding = second_binding
                      || (earlier && writ_str_eq(writ_loc_text(session, (writ_loc_t){source, (uint32_t)parse_token_start_byte(earlier),
                                                                                      (uint32_t)parse_token_end_byte(earlier)}),
                                                 writ_cstr("binding")));
    }
    if (second_binding) {
        writ_report(session, WRIT_D_BIND_LANGUAGE, loc, "a second binding part");
        return;
    }
    writ_report(session, WRIT_D_SYNTAX, loc, "unexpected `%.*s`", (int)spelling.length, spelling.data);
}

static writ_status_t
parse_source(writ_unit_t *unit, uint32_t source, writ_source_kind_t kind, writ_grammar_id_t which)
{
    writ_session_t *session = unit->session;
    writ_grammar_t *grammar = grammar_for(session, which);
    if (!grammar) {
        return WRIT_TOOL_FAILED;
    }
    const writ_source_t *text = writ_source_at(session, source);
    core_buffer_t *buffer = nullptr;
    parse_scanner_t *scanner = nullptr;
    parse_token_stream_t *stream =
        scan_tokenize(grammar->grammar, core_string_borrow(text->text.data, text->text.length),
                      &buffer, &scanner);
    parse_nt_id_t start = kind == WRIT_SOURCE_CONTRACT_DECLARATIONS ? grammar->declarations_start
                        : kind == WRIT_SOURCE_EMBEDDED           ? grammar->embedded_start
                                                                 : grammar->file_start;
    parse_pwz_parser_t *parser = parse_pwz_new_for_start(grammar->grammar, start);
    writ_status_t status = WRIT_OK;
    if (!parse_pwz_parse(parser, stream)) {
        report_syntax_error(session, source, stream, parse_pwz_error_pos(parser), scanner,
                            kind == WRIT_SOURCE_EMBEDDED);
        status = WRIT_HAS_ERRORS;
    } else {
        parse_forest_t forest = parse_pwz_get_forest(parser);
        if (parse_forest_count(&forest) > 1) {
            writ_report(session, WRIT_D_AMBIGUOUS, (writ_loc_t){source, 0, (uint32_t)text->text.length}, nullptr);
        }
        converter_t converter = {unit, session, grammar, source, core_list_new(uint32_t)};
        writ_loc_t loc;
        writ_sym_t lexeme = 0;
        uint32_t first_token = (uint32_t)unit->tokens.len;
        convert(&converter, parse_forest_tree(&forest, 0), &loc, &lexeme);
        if (converter.scratch.len) {
            core_list_push(unit->roots, ((writ_unit_root_t){kind, source, converter.scratch.data[0], first_token,
                                                            (uint32_t)unit->tokens.len, false}));
        }
        core_list_free(converter.scratch);
        parse_forest_free(&forest);
    }
    parse_pwz_free(parser);
    scan_token_stream_free(stream);
    scan_scanner_free(scanner);
    core_buffer_free(buffer);
    return status;
}

writ_status_t
writ_unit_add_source(writ_unit_t *unit, writ_source_kind_t kind, writ_origin_t origin, writ_str_t text,
                     uint32_t *source_id)
{
    if (!unit || (!text.data && text.length)) {
        return WRIT_INVALID_ARGUMENT;
    }
    size_t first_diag = unit->session->diags.len;
    uint32_t source = writ_source_add(unit->session, kind, origin, origin.host_path, text);
    if (source_id) {
        *source_id = source;
    }
    writ_status_t status = parse_source(unit, source, kind, WRIT_GRAMMAR_WRIT);
    writ_diag_flush(unit->session, first_diag);
    return status;
}

writ_status_t
writ_unit_add_embedded(writ_unit_t *unit, writ_str_t language, writ_origin_t origin, writ_str_t text,
                       uint32_t *source_id)
{
    if (!unit || !language.length) {
        return WRIT_INVALID_ARGUMENT;
    }
    unit->language = writ_intern_str(unit->session, language);
    return writ_unit_add_source(unit, WRIT_SOURCE_EMBEDDED, origin, text, source_id);
}

static writ_status_t
read_source(writ_unit_t *unit, writ_str_t path, uint32_t *source_id)
{
    writ_str_t terminated = writ_str_copy(unit->session, path.data, path.length);
    FILE *file = fopen(terminated.data, "rb");
    if (!file) {
        return WRIT_IO_FAILED;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    char *data = writ_alloc(unit->session, size > 0 ? (size_t)size : 1);
    size_t read = size > 0 ? fread(data, 1, (size_t)size, file) : 0;
    fclose(file);
    if (size < 0 || read != (size_t)size) {
        return WRIT_IO_FAILED;
    }
    writ_origin_t origin = {.host_path = terminated};
    *source_id = writ_source_add(unit->session, WRIT_SOURCE_CONTRACT_FILE, origin, terminated,
                                 (writ_str_t){data, read});
    writ_sym_t file_sym = writ_intern_str(unit->session, terminated);
    core_list_foreach(unit->files, seen) {
        if (*seen == file_sym) {
            return WRIT_OK;
        }
    }
    core_list_push(unit->files, file_sym);
    return WRIT_OK;
}

writ_status_t
writ_unit_read_file(writ_unit_t *unit, writ_str_t path, uint32_t *source_id)
{
    uint32_t source = 0;
    writ_status_t status = read_source(unit, path, &source);
    if (source_id) {
        *source_id = source;
    }
    return status == WRIT_OK ? parse_source(unit, source, WRIT_SOURCE_CONTRACT_FILE, WRIT_GRAMMAR_WRIT) : status;
}

writ_status_t
writ_unit_read_json(writ_unit_t *unit, writ_str_t path, uint32_t *root)
{
    uint32_t source = 0;
    writ_status_t status = read_source(unit, path, &source);
    if (status == WRIT_OK) {
        status = parse_source(unit, source, WRIT_SOURCE_CONTRACT_FILE, WRIT_GRAMMAR_JSON);
    }
    if (status == WRIT_OK && unit->roots.len) {
        *root = writ_node_child(unit, unit->roots.data[unit->roots.len - 1].root, 0);
    }
    return status;
}

writ_status_t
writ_unit_add_file(writ_unit_t *unit, writ_str_t path, uint32_t *source_id)
{
    if (!unit || !path.data) {
        return WRIT_INVALID_ARGUMENT;
    }
    size_t first_diag = unit->session->diags.len;
    writ_status_t status = writ_unit_read_file(unit, path, source_id);
    writ_diag_flush(unit->session, first_diag);
    return status;
}

writ_str_t
writ_syntax_audit(writ_session_t *session, bool candidates)
{
    writ_grammar_t *grammar = grammar_for(session, WRIT_GRAMMAR_WRIT);
    if (!grammar) {
        return writ_cstr("the grammar does not load\n");
    }
    parse_ambiguity_options_t options = {.max_witness_tokens = 12, .max_findings = 512,
                                         .include_candidates = candidates};
    parse_ambiguity_list_t findings = parse_grammar_audit_ambiguity(grammar->grammar, &options);
    core_buffer_t *buffer = core_buffer_empty();
    for (size_t i = 0; i < findings.len; i++) {
        const parse_ambiguity_finding_t *finding = &findings.data[i];
        core_string_t severity = parse_ambiguity_severity_name(finding->severity);
        core_string_t kind = parse_ambiguity_kind_name(finding->kind);
        core_buffer_printf(buffer, "%.*s %.*s in <%.*s>: %.*s\n", (int)severity.u8_bytes, severity.data,
                           (int)kind.u8_bytes, kind.data, (int)finding->nonterminal_name.u8_bytes,
                           finding->nonterminal_name.data, (int)finding->witness.u8_bytes, finding->witness.data);
    }
    parse_ambiguity_list_free(&findings);
    return writ_str_from_buffer(session, buffer);
}
