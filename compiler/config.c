#include "config.h"

#include "cache_location.h"
#include "compiler_cli.h"
#include "diagnostics.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "lib/os.h"
#include "parse/bnf.h"
#include "parse/forest.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree.h"
#include "parse/tree_util.h"
#include "scanner/lex.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROJECT_FILE "n00b.cfg"
#ifdef __APPLE__
#define NATIVE_DEFAULT "true"
#else
#define NATIVE_DEFAULT "false"
#endif
#define ENVIRONMENT(variable) .environment = CORE_STRING_STATIC(variable)
#define ENVIRONMENT_MATCH(variable, word) \
    ENVIRONMENT(variable), .environment_match = CORE_STRING_STATIC(word)
#define RANGE(low, high) .minimum = (low), .maximum = (high)
#define N00BB_SETTING(id, section_name, key, kind, fallback, ...) \
    [N00BB_KEY_##id] = { \
        .section = CORE_STRING_STATIC(section_name), \
        .name = CORE_STRING_STATIC(key), \
        .type = N00BB_SETTING_##kind, \
        .default_value = CORE_STRING_STATIC(fallback), \
        __VA_ARGS__ \
    },

const n00bb_setting_spec_t n00bb_setting_specs[N00BB_KEY_COUNT] = {
#include "settings.def"
};

#undef N00BB_SETTING

typedef struct {
    bool                       boolean;
    int64_t                    integer;
    core_string_t              text;
    core_list_t(core_string_t) items;
} setting_value_t;

typedef struct {
    const n00bb_setting_spec_t *spec;
    core_string_t               key;
    n00bb_setting_source_t      source;
    core_string_t               origin;
    setting_value_t             value;
} setting_t;

core_list_decl(setting_t);

typedef struct {
    bool                     loaded;
    bool                     reported;
    bool                     quiet;
    bool                     flag_failed;
    core_list_t(setting_t)   settings;
    core_diagnostics_t      *diagnostics;
    core_string_t            user_path;
    bool                     user_found;
    core_string_t            project_path;
} config_t;

static config_t config;

static void
release_value(setting_value_t *value)
{
    core_free(value->text.data);
    for (size_t i = 0; i < value->items.len; i++)
        core_free(value->items.data[i].data);
    core_list_free(value->items);
    *value = (setting_value_t){0};
}

static void
assign(setting_t *setting, setting_value_t value,
       n00bb_setting_source_t source, core_string_t origin)
{
    release_value(&setting->value);
    setting->value = value;
    setting->source = source;
    core_free(setting->origin.data);
    setting->origin = core_string_dup(origin);
}

static core_string_t
join_key(core_string_t section, core_string_t name)
{
    core_buffer_t key = {0};
    core_buffer_append_string(&key, section);
    core_buffer_putc(&key, '.');
    core_buffer_append_string(&key, name);
    core_string_t joined = core_string_dup(core_buffer_string(&key));
    core_buffer_release(&key);
    return joined;
}

static bool
is_text(n00bb_setting_type_t type)
{
    return type == N00BB_SETTING_CHOICE || type == N00BB_SETTING_PATH
        || type == N00BB_SETTING_PROGRAM;
}

static setting_value_t
default_value(const n00bb_setting_spec_t *spec)
{
    setting_value_t value = {0};
    core_string_t text = spec->default_value;
    switch (spec->type) {
    case N00BB_SETTING_BOOL:
        value.boolean = core_string_eq(text, CORE_STRING_STATIC("true"));
        break;
    case N00BB_SETTING_INT:
        core_string_parse_i64(text, 10, &value.integer, nullptr);
        break;
    default:
        if (is_text(spec->type)) value.text = core_string_dup(text);
        break;
    }
    return value;
}

static setting_t *
find_setting(core_string_t section, core_string_t name)
{
    for (size_t i = 0; i < config.settings.len; i++) {
        setting_t *setting = &config.settings.data[i];
        if (core_string_eq(setting->spec->section, section)
            && core_string_eq(setting->spec->name, name))
            return setting;
    }
    return nullptr;
}

static bool
known_section(core_string_t section)
{
    for (size_t i = 0; i < config.settings.len; i++)
        if (core_string_eq(config.settings.data[i].spec->section, section))
            return true;
    return false;
}

static void
collect_settings(void)
{
    for (size_t id = 0; id < N00BB_KEY_COUNT; id++) {
        const n00bb_setting_spec_t *spec = &n00bb_setting_specs[id];
        core_list_push(config.settings, ((setting_t){
            .spec = spec,
            .key = join_key(spec->section, spec->name),
            .source = N00BB_SETTING_FROM_DEFAULT,
            .value = default_value(spec),
        }));
    }
}

static void
set_path(size_t before, core_string_t path)
{
    size_t count = core_diagnostics_count(config.diagnostics);
    if (count > before)
        core_diagnostic_set_path(config.diagnostics, count - 1, path);
}

static bool
report(n00bb_diag_t id, core_string_t path, core_diag_location_t location,
       n00bb_text_t text, n00bb_args_t args)
{
    size_t before = core_diagnostics_count(config.diagnostics);
    n00bb_report(config.diagnostics, nullptr, id, location, text, args);
    set_path(before, path);
    return core_diagnostics_count(config.diagnostics) > before;
}

static n00bb_text_t
type_description(n00bb_setting_type_t type)
{
    switch (type) {
    case N00BB_SETTING_BOOL: return N00B_TEXT_TRUE_OR_FALSE;
    case N00BB_SETTING_INT: return N00B_TEXT_A_WHOLE_NUMBER;
    case N00BB_SETTING_CHOICE: return N00B_TEXT_A_QUOTED_WORD;
    case N00BB_SETTING_STRINGS: return N00B_TEXT_A_LIST_OF_STRINGS;
    case N00BB_SETTING_PATHS: return N00B_TEXT_A_LIST_OF_PATHS;
    case N00BB_SETTING_PATH: return N00B_TEXT_A_QUOTED_PATH;
    case N00BB_SETTING_PROGRAM: return N00B_TEXT_A_QUOTED_PROGRAM;
    }
    return N00B_TEXT_A_VALUE;
}

static const scan_lex_comment_t config_comments[] = {
    {.kind = SCAN_LEX_LINE, .opener = CORE_STRING_STATIC("#")},
    {0},
};

static const scan_lex_string_t config_strings[] = {
    {.opener = CORE_STRING_STATIC("\""), .closer = CORE_STRING_STATIC("\""),
     .escape = CORE_STRING_STATIC("\\"), .tok = CORE_STRING_STATIC("STRING")},
    {0},
};

static const scan_lex_number_form_t config_numbers[] = {
    {.digits = CORE_STRING_STATIC("0123456789"),
     .tok_int = CORE_STRING_STATIC("INTEGER")},
    {0},
};

static const core_string_t config_keywords[] = {
    CORE_STRING_STATIC("true"),
    CORE_STRING_STATIC("false"),
    {0},
};

static const scan_lex_spec_t config_lexer = {
    .name          = CORE_STRING_STATIC("n00b-config"),
    .comments      = config_comments,
    .strings       = config_strings,
    .number_forms  = config_numbers,
    .soft_keywords = config_keywords,
    .tok_ident     = CORE_STRING_STATIC("IDENTIFIER"),
    .brackets      = CORE_STRING_STATIC("[]"),
    .fallback      = SCAN_LEX_FALLBACK_REJECT,
};

static const char config_grammar_text[] = {
#embed "config.bnf"
    , 0};

typedef enum {
    VALUE_STRING,
    VALUE_INTEGER,
    VALUE_BOOLEAN,
    VALUE_LIST,
} value_kind_t;

core_list_decl(core_diag_location_t);

typedef struct {
    parse_grammar_t *grammar;
    core_string_t    path;
    core_string_t    text;
    core_string_t    section;
    bool             in_section;
    core_list_t(core_string_t) keys;
    core_list_t(core_diag_location_t) key_locations;
    n00bb_setting_source_t source;
} reader_t;

static core_diag_location_t
token_location(const reader_t *reader, parse_token_info_t *token)
{
    return core_diag_location_span(reader->text,
        parse_token_start_byte(token), parse_token_end_byte(token));
}

static core_string_t
token_text(const reader_t *reader, parse_token_info_t *token)
{
    size_t start = parse_token_start_byte(token);
    return core_string_slice(reader->text, start,
                             parse_token_end_byte(token) - start);
}

static parse_token_info_t *
leaf(parse_tree_t *node, size_t index)
{
    parse_tree_t *child = index < core_tree_num_children(node)
        ? core_tree_child(node, index) : nullptr;
    return child && parse_node_is_token(child) ? parse_node_token(child)
                                               : nullptr;
}

static bool
is(const reader_t *reader, parse_tree_t *node, core_string_t name)
{
    return node && !parse_node_is_token(node)
        && parse_tree_is_nt(reader->grammar, node, name);
}

static value_kind_t
value_kind(const reader_t *reader, parse_tree_t *value)
{
    parse_tree_t *first = core_tree_child(value, 0);
    if (is(reader, first, CORE_STRING_STATIC("list"))) return VALUE_LIST;
    core_string_t text = token_text(reader, parse_node_token(first));
    if (text.u8_bytes && text.data[0] == '"') return VALUE_STRING;
    if (text.u8_bytes && text.data[0] >= '0' && text.data[0] <= '9')
        return VALUE_INTEGER;
    return VALUE_BOOLEAN;
}

static n00bb_text_t
value_description(value_kind_t kind)
{
    switch (kind) {
    case VALUE_STRING: return N00B_TEXT_A_STRING;
    case VALUE_INTEGER: return N00B_TEXT_AN_INTEGER;
    case VALUE_BOOLEAN: return N00B_TEXT_A_BOOLEAN;
    case VALUE_LIST: return N00B_TEXT_A_LIST;
    }
    return N00B_TEXT_A_VALUE;
}

static core_diag_location_t
value_location(const reader_t *reader, parse_tree_t *value)
{
    parse_tree_t *first = core_tree_child(value, 0);
    if (parse_node_is_token(first))
        return token_location(reader, parse_node_token(first));
    return token_location(reader, leaf(first, 0));
}

static bool
unquote(core_string_t quoted, core_buffer_t *out)
{
    for (size_t i = 1; i + 1 < quoted.u8_bytes; i++) {
        char c = quoted.data[i];
        if (c != '\\') {
            core_buffer_putc(out, c);
            continue;
        }
        c = quoted.data[++i];
        switch (c) {
        case '"': case '\\': core_buffer_putc(out, c); break;
        case 'n': core_buffer_putc(out, '\n'); break;
        case 't': core_buffer_putc(out, '\t'); break;
        case 'r': core_buffer_putc(out, '\r'); break;
        default: return false;
        }
    }
    return true;
}

static bool
string_value(const reader_t *reader, parse_tree_t *value, core_string_t *out)
{
    core_buffer_t text = {0};
    core_string_t quoted = token_text(reader, leaf(value, 0));
    bool ok = unquote(quoted, &text);
    if (ok) *out = core_string_dup(core_buffer_string(&text));
    else
        report(N00B_DIAG_CONFIG_SYNTAX, reader->path,
            value_location(reader, value), N00B_TEXT_HAS_AN_ESCAPE_OTHER_THAN,
            N00BB_ARGS(ARG_TEXT(quoted)));
    core_buffer_release(&text);
    return ok;
}

static void
example_help(const setting_t *setting)
{
    const n00bb_setting_spec_t *spec = setting->spec;
    core_string_t name = spec->name;
    switch (spec->type) {
    case N00BB_SETTING_BOOL:
        n00bb_report_help(config.diagnostics, N00B_TEXT_WRITE_TRUE_OR_FALSE,
            N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(name)));
        break;
    case N00BB_SETTING_INT:
        n00bb_report_help(config.diagnostics,
                          N00B_TEXT_WRITE_A_NUMBER_WITHOUT_QUOTES,
            N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(spec->default_value)));
        break;
    case N00BB_SETTING_CHOICE:
        n00bb_report_help(config.diagnostics,
                          N00B_TEXT_WRITE_THE_WORD_IN_QUOTES,
            N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(spec->default_value)));
        break;
    case N00BB_SETTING_STRINGS:
    case N00BB_SETTING_PATHS:
        n00bb_report_help(config.diagnostics,
                          N00B_TEXT_WRITE_A_LIST_IN_BRACKETS,
            N00BB_ARGS(ARG_TEXT(name)));
        break;
    case N00BB_SETTING_PATH:
    case N00BB_SETTING_PROGRAM:
        n00bb_report_help(config.diagnostics,
                          N00B_TEXT_WRITE_THE_PATH_IN_QUOTES,
            N00BB_ARGS(ARG_TEXT(name)));
        break;
    }
}

static bool
wrong_type(const reader_t *reader, const setting_t *setting,
           parse_tree_t *value, n00bb_text_t what)
{
    n00bb_text_t found = value_description(value_kind(reader, value));
    if (report(N00B_DIAG_CONFIG_WRONG_TYPE, reader->path,
        value_location(reader, value), N00B_TEXT_MUST_BE_NOT,
        N00BB_ARGS(ARG_TEXT(setting->key), ARG_PHRASE_ID(what),
                   ARG_PHRASE_ID(found)))) {
        n00bb_report_label(config.diagnostics, N00B_TEXT_THIS_IS_2,
            N00BB_ARGS(ARG_PHRASE_ID(found)));
        example_help(setting);
    }
    return false;
}

static size_t
split_words(core_string_t text, core_string_t *words, size_t capacity)
{
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= text.u8_bytes && count < capacity; i++) {
        if (i < text.u8_bytes && text.data[i] != ' ') continue;
        if (i > start)
            words[count++] = core_string_slice(text, start, i - start);
        start = i + 1;
    }
    return count;
}

static bool
valid_choice(const n00bb_setting_spec_t *spec, core_string_t word)
{
    core_string_t choices[16];
    size_t count = split_words(spec->choices, choices, 16);
    for (size_t i = 0; i < count; i++)
        if (core_string_eq(choices[i], word)) return true;
    return false;
}

static void
suggest_choice(const n00bb_setting_spec_t *spec, core_string_t word)
{
    core_string_t choices[16];
    size_t count = split_words(spec->choices, choices, 16);
    n00bb_diag_did_you_mean(config.diagnostics, word, choices, count);
}

static void
describe_choices(const n00bb_setting_spec_t *spec, core_buffer_t *out)
{
    core_string_t choices[16];
    size_t count = split_words(spec->choices, choices, 16);
    for (size_t i = 0; i < count; i++) {
        if (i) core_buffer_printf(out, "%s", i + 1 == count ? " or " : ", ");
        core_buffer_printf(out, "`%.*s`", (int)choices[i].u8_bytes,
                           choices[i].data);
    }
}

static bool
check_choice(const reader_t *reader, const setting_t *setting,
             core_diag_location_t location, core_string_t word)
{
    if (valid_choice(setting->spec, word)) return true;
    core_buffer_t choices = {0};
    describe_choices(setting->spec, &choices);
    if (report(N00B_DIAG_CONFIG_INVALID_VALUE, reader->path, location,
        N00B_TEXT_MUST_BE_ONE_OF_NOT,
        N00BB_ARGS(ARG_TEXT(setting->key),
                   ARG_TEXT(core_buffer_string(&choices)), ARG_TEXT(word))))
        suggest_choice(setting->spec, word);
    core_buffer_release(&choices);
    return false;
}

static core_string_t
file_relative(core_string_t file, core_string_t path, bool program)
{
    bool relative = !core_os_is_absolute(path)
        && (!program || core_string_find_byte(path, '/') >= 0
            || core_string_find_byte(path, '\\') >= 0);
    if (!relative || !path.u8_bytes) return core_string_dup(path);
    core_string_t directory = core_os_path_parent(file);
    core_string_t joined = core_os_path_join(directory, path);
    core_free(directory.data);
    return joined;
}

static bool
read_list(const reader_t *reader, const setting_t *setting,
          parse_tree_t *value, setting_value_t *out)
{
    if (value_kind(reader, value) != VALUE_LIST)
        return wrong_type(reader, setting, value,
                          type_description(setting->spec->type));
    bool paths = setting->spec->type == N00BB_SETTING_PATHS;
    parse_tree_t *list = core_tree_child(value, 0);
    parse_tree_t *elements = core_tree_num_children(list) > 2
        ? core_tree_child(list, 1) : nullptr;
    while (elements) {
        parse_tree_t *element = core_tree_child(elements, 0);
        if (value_kind(reader, element) != VALUE_STRING) {
            if (report(N00B_DIAG_CONFIG_WRONG_TYPE, reader->path,
                value_location(reader, element),
                N00B_TEXT_MUST_BE_BUT_THIS_ELEMENT,
                N00BB_ARGS(ARG_TEXT(setting->key),
                    ARG_PHRASE_ID(type_description(setting->spec->type)),
                    ARG_PHRASE_ID(value_description(
                        value_kind(reader, element))))))
                n00bb_report_help(config.diagnostics,
                    N00B_TEXT_PUT_THE_ELEMENT_IN_QUOTES,
                    N00BB_NO_ARGS);
            return false;
        }
        core_string_t text = {0};
        if (!string_value(reader, element, &text)) return false;
        core_list_push(out->items,
                       paths ? file_relative(reader->path, text, false) : text);
        if (paths) core_free(text.data);
        elements = core_tree_num_children(elements) > 2
            ? core_tree_child(elements, 2) : nullptr;
    }
    return true;
}

static bool
read_value(const reader_t *reader, const setting_t *setting,
           parse_tree_t *value, setting_value_t *out)
{
    const n00bb_setting_spec_t *spec = setting->spec;
    n00bb_text_t expected = type_description(spec->type);
    value_kind_t kind = value_kind(reader, value);
    core_diag_location_t location = value_location(reader, value);
    switch (spec->type) {
    case N00BB_SETTING_BOOL:
        if (kind != VALUE_BOOLEAN)
            return wrong_type(reader, setting, value, expected);
        out->boolean = core_string_eq(token_text(reader, leaf(value, 0)),
                                      CORE_STRING_STATIC("true"));
        return true;
    case N00BB_SETTING_INT: {
        if (kind != VALUE_INTEGER)
            return wrong_type(reader, setting, value, expected);
        core_string_t digits = token_text(reader, leaf(value, 0));
        int64_t number = 0;
        bool parsed = core_string_parse_i64(digits, 10, &number, nullptr);
        if (!parsed || number < spec->minimum || number > spec->maximum) {
            if (spec->maximum == INT64_MAX)
                report(N00B_DIAG_CONFIG_INVALID_VALUE, reader->path, location,
                    N00B_TEXT_MUST_BE_OR_MORE_NOT,
                    N00BB_ARGS(ARG_TEXT(setting->key),
                        ARG_NUMBER((long long)spec->minimum),
                    ARG_TEXT(digits)));
            else
                report(N00B_DIAG_CONFIG_INVALID_VALUE, reader->path, location,
                    N00B_TEXT_MUST_BE_BETWEEN_AND_NOT,
                    N00BB_ARGS(ARG_TEXT(setting->key),
                        ARG_NUMBER((long long)spec->minimum),
                    ARG_NUMBER((long long)spec->maximum), ARG_TEXT(digits)));
            return false;
        }
        out->integer = number;
        return true;
    }
    case N00BB_SETTING_CHOICE: {
        if (kind != VALUE_STRING)
            return wrong_type(reader, setting, value, expected);
        core_string_t word = {0};
        if (!string_value(reader, value, &word)) return false;
        if (!check_choice(reader, setting, location, word)) {
            core_free(word.data);
            return false;
        }
        out->text = word;
        return true;
    }
    case N00BB_SETTING_PATH:
    case N00BB_SETTING_PROGRAM: {
        if (kind != VALUE_STRING)
            return wrong_type(reader, setting, value, expected);
        core_string_t text = {0};
        if (!string_value(reader, value, &text)) return false;
        out->text = file_relative(reader->path, text,
                                  spec->type == N00BB_SETTING_PROGRAM);
        core_free(text.data);
        return true;
    }
    case N00BB_SETTING_STRINGS:
    case N00BB_SETTING_PATHS:
        return read_list(reader, setting, value, out);
    }
    return false;
}

static core_string_t
file_origin(core_string_t path, core_diag_location_t location)
{
    core_buffer_t origin = {0};
    core_buffer_printf(&origin, "%.*s:%u", (int)path.u8_bytes, path.data,
                       location.line);
    core_string_t text = core_string_dup(core_buffer_string(&origin));
    core_buffer_release(&origin);
    return text;
}

static size_t
section_names(core_string_t *names, size_t capacity)
{
    size_t count = 0;
    for (size_t i = 0; i < config.settings.len && count < capacity; i++) {
        core_string_t section = config.settings.data[i].spec->section;
        bool seen = false;
        for (size_t j = 0; j < count && !seen; j++)
            seen = core_string_eq(names[j], section);
        if (!seen) names[count++] = section;
    }
    return count;
}

static size_t
names_in_section(core_string_t section, core_string_t *names,
                 size_t capacity)
{
    size_t count = 0;
    for (size_t i = 0; i < config.settings.len && count < capacity; i++)
        if (core_string_eq(config.settings.data[i].spec->section, section))
            names[count++] = config.settings.data[i].spec->name;
    return count;
}

static const setting_t *
setting_named(core_string_t name)
{
    for (size_t i = 0; i < config.settings.len; i++)
        if (core_string_eq(config.settings.data[i].spec->name, name))
            return &config.settings.data[i];
    return nullptr;
}

static void
help_owner(core_string_t key)
{
    const setting_t *owner = setting_named(key);
    if (owner)
        n00bb_report_help(config.diagnostics, N00B_TEXT_BELONGS_UNDER,
            N00BB_ARGS(ARG_TEXT(key), ARG_TEXT(owner->spec->section)));
    else
        n00bb_report_help(config.diagnostics, N00B_TEXT_RUN_N00B_CONFIG_TO_LIST,
            N00BB_NO_ARGS);
}

static void
open_section(reader_t *reader, parse_tree_t *section)
{
    parse_token_info_t *name = leaf(section, 1);
    reader->section = token_text(reader, name);
    reader->in_section = true;
    if (known_section(reader->section)
        || !report(N00B_DIAG_CONFIG_UNKNOWN_SETTING, reader->path,
            token_location(reader, name),
            N00B_TEXT_THERE_IS_NO_SETTINGS_SECTION,
            N00BB_ARGS(ARG_TEXT(reader->section))))
        return;
    core_string_t names[32];
    size_t count = section_names(names, 32);
    if (!n00bb_diag_did_you_mean(config.diagnostics, reader->section, names,
                                 count))
        n00bb_report_help(config.diagnostics, N00B_TEXT_RUN_N00B_CONFIG_TO_LIST,
            N00BB_NO_ARGS);
}

static bool
redefined(reader_t *reader, core_string_t key, core_diag_location_t at)
{
    core_buffer_t full = {0};
    core_buffer_append_string(&full, reader->section);
    core_buffer_putc(&full, '.');
    core_buffer_append_string(&full, key);
    size_t found = SIZE_MAX;
    for (size_t i = 0; i < reader->keys.len && found == SIZE_MAX; i++)
        if (core_string_eq(reader->keys.data[i], core_buffer_string(&full)))
            found = i;
    if (found == SIZE_MAX) {
        core_list_push(reader->keys,
                       core_string_dup(core_buffer_string(&full)));
        core_list_push(reader->key_locations, at);
    }
    core_buffer_release(&full);
    if (found == SIZE_MAX) return false;
    if (report(N00B_DIAG_CONFIG_REDEFINED, reader->path, at,
        N00B_TEXT_IS_ALREADY_DEFINED,
        N00BB_ARGS(ARG_TEXT(key)))) {
        core_string_t label = n00bb_text(N00B_TEXT_FIRST_DEFINED_HERE,
                                         N00BB_NO_ARGS);
        core_diag_secondary_path(config.diagnostics, reader->path,
                                 reader->key_locations.data[found], label);
        core_free(label.data);
    }
    return true;
}

static void
apply_setting(reader_t *reader, parse_tree_t *node)
{
    parse_token_info_t *name = leaf(node, 0);
    core_string_t key = token_text(reader, name);
    core_diag_location_t at = token_location(reader, name);
    parse_tree_t *value = core_tree_child(node, 2);
    if (!reader->in_section) {
        if (report(N00B_DIAG_CONFIG_UNKNOWN_SETTING, reader->path, at,
            N00B_TEXT_IS_NOT_IN_A_SECTION,
            N00BB_ARGS(ARG_TEXT(key))))
            help_owner(key);
        return;
    }
    if (!known_section(reader->section) || redefined(reader, key, at)) return;
    setting_t *setting = find_setting(reader->section, key);
    if (!setting) {
        if (!report(N00B_DIAG_CONFIG_UNKNOWN_SETTING, reader->path, at,
            N00B_TEXT_THERE_IS_NO_SETTING_NAMED,
            N00BB_ARGS(ARG_TEXT(key), ARG_TEXT(reader->section))))
            return;
        core_string_t names[32];
        size_t count = names_in_section(reader->section, names, 32);
        if (!n00bb_diag_did_you_mean(config.diagnostics, key, names, count))
            help_owner(key);
        return;
    }
    setting_value_t read = {0};
    if (!read_value(reader, setting, value, &read)) {
        release_value(&read);
        return;
    }
    core_string_t origin = file_origin(reader->path, at);
    assign(setting, read, reader->source, origin);
    core_free(origin.data);
}

static void
apply_document(reader_t *reader, parse_tree_t *document)
{
    while (document) {
        parse_tree_t *item = core_tree_child(core_tree_child(document, 0), 0);
        if (is(reader, item, CORE_STRING_STATIC("section"))) open_section(reader, item);
        else apply_setting(reader, item);
        document = core_tree_num_children(document) > 1
            ? core_tree_child(document, 1) : nullptr;
    }
}

static void
report_syntax(reader_t *reader, parse_scanner_t *scanner,
              parse_token_stream_t *stream, int32_t position)
{
    scan_diagnostic_t lexical;
    if (scanner && scan_status(scanner) == SCAN_STATUS_INVALID
        && scan_diagnostic(scanner, &lexical)) {
        report(N00B_DIAG_CONFIG_SYNTAX, reader->path,
            core_diag_location_span(reader->text, lexical.start_byte,
                                       lexical.end_byte),
            n00bb_scan_text(lexical.problem.kind),
            N00BB_ARGS(ARG_TEXT(lexical.problem.detail)));
        return;
    }
    scan_stream_reset(stream);
    parse_token_info_t *token = position >= 0
        ? scan_stream_get(stream, position) : nullptr;
    if (!token) {
        report(N00B_DIAG_CONFIG_SYNTAX, reader->path,
            core_diag_location_span(reader->text, reader->text.u8_bytes,
                                       reader->text.u8_bytes),
                                       N00B_TEXT_THE_FILE_ENDS_IN_THE,
            N00BB_NO_ARGS);
        return;
    }
    core_string_t text = token_text(reader, token);
    report(N00B_DIAG_CONFIG_SYNTAX, reader->path, token_location(reader, token),
        N00B_TEXT_UNEXPECTED,
        N00BB_ARGS(ARG_TEXT(text)));
}

static void
read_document(reader_t *reader)
{
    core_string_t grammar_text = core_string_borrow(config_grammar_text,
        sizeof(config_grammar_text) - 1);
    reader->grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(reader->grammar, false);
    parse_bnf_declare_terminals(reader->grammar, grammar_text);
    if (!parse_bnf_load(grammar_text, CORE_STRING_STATIC("config"),
                        reader->grammar)
        || !scan_grammar_set_lex(reader->grammar, &config_lexer)) {
        n00bb_internal_error(config.diagnostics, nullptr,
            (core_diag_location_t){}, "reading the configuration",
            "the configuration grammar does not load");
        parse_grammar_free(reader->grammar);
        return;
    }
    parse_grammar_finalize(reader->grammar);
    core_buffer_t *buffer = nullptr;
    parse_scanner_t *scanner = nullptr;
    parse_token_stream_t *stream = scan_tokenize(reader->grammar,
        reader->text, &buffer, &scanner);
    parse_nonterm_t *start = parse_find_nonterm(reader->grammar,
        CORE_STRING_STATIC("config"));
    parse_pwz_parser_t *parser = parse_pwz_new_for_start(reader->grammar,
        parse_nonterm_id(start));
    bool empty = !scan_stream_get(stream, 0)
        && !(scanner && scan_status(scanner) == SCAN_STATUS_INVALID);
    scan_stream_reset(stream);
    if (!empty && !parse_pwz_parse(parser, stream))
        report_syntax(reader, scanner, stream, parse_pwz_error_pos(parser));
    else if (!empty) {
        parse_forest_t forest = parse_pwz_get_forest(parser);
        apply_document(reader, parse_forest_tree(&forest, 0));
        parse_forest_free(&forest);
    }
    parse_pwz_free(parser);
    scan_token_stream_free(stream);
    scan_scanner_free(scanner);
    core_buffer_free(buffer);
    parse_grammar_free(reader->grammar);
}

static bool
earlier(const core_diagnostic_t *a, const core_diagnostic_t *b)
{
    if (a->location.line != b->location.line)
        return a->location.line < b->location.line;
    return a->location.column < b->location.column;
}

static void
append_in_source_order(core_diagnostics_t *destination,
                       const core_diagnostics_t *source)
{
    size_t count = core_diagnostics_count(source);
    const core_diagnostic_t **ordered = core_alloc_array(
        const core_diagnostic_t *, count);
    for (size_t i = 0; i < count; i++) {
        const core_diagnostic_t *item = core_diagnostics_at(source, i);
        size_t at = i;
        while (at > 0 && earlier(item, ordered[at - 1])) {
            ordered[at] = ordered[at - 1];
            at--;
        }
        ordered[at] = item;
    }
    for (size_t i = 0; i < count; i++)
        core_diagnostics_append(destination, ordered[i]);
    core_free(ordered);
}

static bool
load_file(core_string_t path, n00bb_setting_source_t source)
{
    if (!path.data) return false;
    if (core_os_is_directory(path)) {
        report(N00B_DIAG_CONFIG_UNREADABLE, (core_string_t){0},
            (core_diag_location_t){},
            N00B_TEXT_CANNOT_READ_THE_CONFIGURATION_FILE,
            N00BB_ARGS(ARG_TEXT(path)));
        return true;
    }
    if (!core_os_is_regular_file(path)) return false;
    char *bytes = nullptr;
    size_t size = 0;
    if (!core_os_read_file(path, &bytes, &size)) {
        report(N00B_DIAG_CONFIG_UNREADABLE, (core_string_t){0},
            (core_diag_location_t){},
            N00B_TEXT_CANNOT_READ_THE_CONFIGURATION_FILE_2,
            N00BB_ARGS(ARG_TEXT(path), ARG_ERRNO(errno ? errno : EIO)));
        return true;
    }
    core_diagnostics_t *all = config.diagnostics;
    config.diagnostics = core_diagnostics_new();
    reader_t reader = {
        .path = path,
        .text = core_string_borrow(bytes, size),
        .keys = core_list_new(core_string_t),
        .key_locations = core_list_new(core_diag_location_t),
        .source = source,
    };
    read_document(&reader);
    for (size_t i = 0; i < reader.keys.len; i++)
        core_free(reader.keys.data[i].data);
    core_list_free(reader.keys);
    core_list_free(reader.key_locations);
    core_free(bytes);
    append_in_source_order(all, config.diagnostics);
    core_diagnostics_free(config.diagnostics);
    config.diagnostics = all;
    return true;
}

static core_string_t
user_config_path(void)
{
#ifdef _WIN32
    core_string_t base = core_os_environment(CORE_STRING_STATIC("APPDATA"));
    core_string_t suffix = CORE_STRING_STATIC("n00b/config.cfg");
#else
    core_string_t base = core_os_environment(CORE_STRING_STATIC("XDG_CONFIG_HOME"));
    core_string_t suffix = CORE_STRING_STATIC("n00b/config.cfg");
    if (!base.u8_bytes || !core_os_is_absolute(base)) {
        core_free(base.data);
        base = core_os_environment(CORE_STRING_STATIC("HOME"));
        suffix = CORE_STRING_STATIC(".config/n00b/config.cfg");
    }
#endif
    if (!base.u8_bytes) {
        core_free(base.data);
        return (core_string_t){0};
    }
    core_string_t path = core_os_path_join(base, suffix);
    core_free(base.data);
    return path;
}

static void
split_list(core_string_t text, setting_value_t *value)
{
    size_t start = 0;
    for (size_t i = 0; i <= text.u8_bytes; i++) {
        if (i < text.u8_bytes && text.data[i] != CORE_OS_LIST_SEPARATOR) continue;
        if (i > start)
            core_list_push(value->items, core_string_dup(
                core_string_slice(text, start, i - start)));
        start = i + 1;
    }
}

static bool
environment_truth(core_string_t text)
{
    static const core_string_t truths[] = {
        CORE_STRING_STATIC("1"), CORE_STRING_STATIC("true"),
        CORE_STRING_STATIC("yes"), CORE_STRING_STATIC("on"),
    };
    for (size_t i = 0; i < sizeof(truths) / sizeof(truths[0]); i++)
        if (core_string_eq(text, truths[i])) return true;
    return false;
}

static void
apply_environment(setting_t *setting)
{
    const n00bb_setting_spec_t *spec = setting->spec;
    if (!spec->environment.u8_bytes) return;
    core_string_t text = core_os_environment(spec->environment);
    if (!text.u8_bytes) {
        core_free(text.data);
        return;
    }
    setting_value_t value = {0};
    bool usable = true;
    switch (spec->type) {
    case N00BB_SETTING_BOOL:
        value.boolean = spec->environment_match.u8_bytes
            ? core_string_eq(text, spec->environment_match)
            : environment_truth(text);
        break;
    case N00BB_SETTING_INT:
        usable = core_string_parse_i64(text, 10, &value.integer, nullptr)
            && value.integer >= spec->minimum
            && value.integer <= spec->maximum;
        break;
    case N00BB_SETTING_CHOICE:
        value.text = core_string_dup(spec->environment_match.u8_bytes
                                         ? spec->environment_match : text);
        usable = valid_choice(spec, value.text);
        break;
    case N00BB_SETTING_STRINGS:
    case N00BB_SETTING_PATHS:
        split_list(text, &value);
        break;
    case N00BB_SETTING_PATH:
    case N00BB_SETTING_PROGRAM:
        value.text = core_string_dup(text);
        break;
    }
    if (usable)
        assign(setting, value, N00BB_SETTING_FROM_ENVIRONMENT,
               spec->environment);
    else release_value(&value);
    core_free(text.data);
}

static void
invalid_flag_choice(const parse_cmdr_flag_t *flag, core_string_t word)
{
    const n00bb_flag_setting_t *mapping = flag->setting;
    config.flag_failed = true;
    core_buffer_t choices = {0};
    describe_choices(&n00bb_setting_specs[mapping->id], &choices);
    if (report(N00B_DIAG_INVALID_OPTION_VALUE, (core_string_t){0},
        (core_diag_location_t){}, N00B_TEXT_TAKES_NOT,
        N00BB_ARGS(ARG_TEXT(flag->name), ARG_TEXT(core_buffer_string(&choices)),
                   ARG_TEXT(word))))
        suggest_choice(&n00bb_setting_specs[mapping->id], word);
    core_buffer_release(&choices);
}

static void
apply_flag(parse_cmdr_result_t *parsed, const parse_cmdr_flag_t *flag)
{
    const n00bb_flag_setting_t *mapping = flag->setting;
    if (!mapping || !parse_cmdr_flag_present(parsed, flag->name)) return;
    setting_t *setting = &config.settings.data[mapping->id];
    const n00bb_setting_spec_t *spec = setting->spec;
    setting_value_t value = {0};
    switch (spec->type) {
    case N00BB_SETTING_BOOL:
        value.boolean = mapping->value;
        break;
    case N00BB_SETTING_INT:
        value.integer = parse_cmdr_flag_int(parsed, flag->name,
                                            setting->value.integer);
        break;
    case N00BB_SETTING_CHOICE:
        value.text = core_string_dup(parse_cmdr_flag_str(parsed, flag->name));
        if (!valid_choice(spec, value.text)) {
            invalid_flag_choice(flag, value.text);
            release_value(&value);
            return;
        }
        break;
    case N00BB_SETTING_STRINGS:
    case N00BB_SETTING_PATHS:
        for (int32_t i = 0; i < parse_cmdr_flag_value_count(parsed, flag->name);
             i++)
            core_list_push(value.items, core_string_dup(
                parse_cmdr_flag_str_at(parsed, flag->name, i)));
        break;
    case N00BB_SETTING_PATH:
    case N00BB_SETTING_PROGRAM:
        value.text = core_string_dup(parse_cmdr_flag_str(parsed, flag->name));
        break;
    }
    assign(setting, value, N00BB_SETTING_FROM_FLAG, flag->name);
}

static void
apply_flags(parse_cmdr_result_t *parsed)
{
    if (!parse_cmdr_result_ok(parsed)) return;
    for (const parse_cmdr_flag_t *flag = n00bb_cli_global_flags();
         flag->name.data; flag++)
        apply_flag(parsed, flag);
    const parse_cmdr_flag_t *own = n00bb_cli_command_flags(
        parse_cmdr_result_command(parsed));
    for (const parse_cmdr_flag_t *flag = own; flag && flag->name.data; flag++)
        apply_flag(parsed, flag);
}

void
n00bb_config_load(const n00bb_invocation_t *invocation)
{
    if (config.loaded) return;
    config.loaded = true;
    config.diagnostics = core_diagnostics_new();
    config.quiet = n00bb_invocation_flag(invocation,
        CORE_STRING_STATIC("--build-time"), (core_string_t){});
    collect_settings();
    config.user_path = user_config_path();
    config.user_found = load_file(config.user_path,
                                  N00BB_SETTING_FROM_USER_FILE);
    config.project_path = core_os_find_upward(
        CORE_STRING_STATIC(PROJECT_FILE));
    load_file(config.project_path, N00BB_SETTING_FROM_PROJECT_FILE);
    for (size_t i = 0; i < config.settings.len; i++)
        apply_environment(&config.settings.data[i]);
    if (invocation && invocation->end > 1) apply_flags(invocation->parsed);
    n00bb_cache_set_root(n00bb_setting_string(
        N00BB_KEY_PATHS_CACHE));
}

static void
ensure_loaded(void)
{
    if (config.loaded) return;
    n00bb_config_load(nullptr);
    config.reported = true;
}

int
n00bb_config_report(void)
{
    ensure_loaded();
    int status = config.flag_failed ? 2
        : core_diagnostics_failed(config.diagnostics) ? 1 : 0;
    if (config.reported || config.quiet) return status;
    config.reported = true;
    n00bb_print_diagnostics(config.diagnostics);
    return status;
}

static const setting_t *
lookup(n00bb_setting_id_t id)
{
    ensure_loaded();
    return &config.settings.data[id];
}

bool
n00bb_setting_bool(n00bb_setting_id_t id)
{
    return lookup(id)->value.boolean;
}

int64_t
n00bb_setting_int(n00bb_setting_id_t id)
{
    return lookup(id)->value.integer;
}

core_string_t
n00bb_setting_string(n00bb_setting_id_t id)
{
    return lookup(id)->value.text;
}

size_t
n00bb_setting_count(n00bb_setting_id_t id)
{
    return lookup(id)->value.items.len;
}

core_string_t
n00bb_setting_at(n00bb_setting_id_t id, size_t index)
{
    const setting_t *setting = lookup(id);
    return index < setting->value.items.len ? setting->value.items.data[index]
                                            : (core_string_t){0};
}

n00bb_setting_source_t
n00bb_setting_source(n00bb_setting_id_t id)
{
    return lookup(id)->source;
}

core_string_t
n00bb_setting_origin(n00bb_setting_id_t id)
{
    const setting_t *setting = lookup(id);
    return setting->source == N00BB_SETTING_FROM_DEFAULT
        ? n00bb_text_form(N00B_TEXT_THE_DEFAULT) : setting->origin;
}

core_string_t
n00bb_setting_key(n00bb_setting_id_t id)
{
    return lookup(id)->key;
}

static void
print_quoted(core_buffer_t *out, core_string_t text)
{
    core_buffer_putc(out, '"');
    for (size_t i = 0; i < text.u8_bytes; i++) {
        char c = text.data[i];
        if (c == '"' || c == '\\') core_buffer_putc(out, '\\');
        core_buffer_putc(out, c);
    }
    core_buffer_putc(out, '"');
}

static void
print_value(core_buffer_t *out, const setting_t *setting)
{
    const setting_value_t *value = &setting->value;
    switch (setting->spec->type) {
    case N00BB_SETTING_BOOL:
        core_buffer_printf(out, "%s", value->boolean ? "true" : "false");
        break;
    case N00BB_SETTING_INT:
        core_buffer_printf(out, "%lld", (long long)value->integer);
        break;
    case N00BB_SETTING_STRINGS:
    case N00BB_SETTING_PATHS:
        core_buffer_putc(out, '[');
        for (size_t i = 0; i < value->items.len; i++) {
            if (i) core_buffer_puts(out, ", ");
            print_quoted(out, value->items.data[i]);
        }
        core_buffer_putc(out, ']');
        break;
    default:
        print_quoted(out, value->text);
        break;
    }
}

static void
print_source(core_buffer_t *out, const setting_t *setting)
{
    switch (setting->source) {
    case N00BB_SETTING_FROM_DEFAULT:
        core_buffer_puts(out, "default");
        if (is_text(setting->spec->type) && !setting->value.text.u8_bytes)
            core_buffer_puts(out, ", chosen automatically");
        break;
    case N00BB_SETTING_FROM_USER_FILE:
    case N00BB_SETTING_FROM_PROJECT_FILE:
        core_buffer_append_string(out, setting->origin);
        break;
    case N00BB_SETTING_FROM_ENVIRONMENT:
        core_buffer_puts(out, "environment ");
        core_buffer_append_string(out, setting->origin);
        break;
    case N00BB_SETTING_FROM_FLAG:
        core_buffer_puts(out, "flag ");
        core_buffer_append_string(out, setting->origin);
        break;
    }
}

static void
print_file_line(const char *label, core_string_t path, bool found)
{
    if (!path.data) printf("# %s: none\n", label);
    else
        printf("# %s: %.*s%s\n", label, (int)path.u8_bytes, path.data,
               found ? "" : " (not found)");
}

int
n00bb_config_command(void)
{
    ensure_loaded();
    print_file_line("user file", config.user_path, config.user_found);
    print_file_line("project file", config.project_path,
                    config.project_path.data != nullptr);
    core_string_t sections[32];
    size_t section_count = section_names(sections, 32);
    for (size_t s = 0; s < section_count; s++) {
        printf("\n[%.*s]\n", (int)sections[s].u8_bytes, sections[s].data);
        for (size_t i = 0; i < config.settings.len; i++) {
            const setting_t *setting = &config.settings.data[i];
            if (!core_string_eq(setting->spec->section, sections[s])) continue;
            core_buffer_t line = {0};
            core_buffer_append_string(&line, setting->spec->name);
            core_buffer_puts(&line, " = ");
            print_value(&line, setting);
            while (line.byte_len < 40) core_buffer_putc(&line, ' ');
            core_buffer_puts(&line, " # ");
            print_source(&line, setting);
            printf("%.*s\n", (int)line.byte_len, line.data);
            core_buffer_release(&line);
        }
    }
    return core_diagnostics_failed(config.diagnostics) ? 1 : 0;
}
