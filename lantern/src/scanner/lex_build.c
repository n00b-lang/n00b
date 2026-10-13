#include "scanner/lex.h"
#include "scanner/tokenizer.h"
#include "private/scanner/lex_decl.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

core_list_decl(scan_lex_comment_t);
core_list_decl(scan_lex_region_t);
core_list_decl(scan_lex_string_t);
core_list_decl(scan_lex_escape_t);
core_list_decl(scan_lex_sigil_t);
core_list_decl(scan_lex_regex_t);
core_list_decl(scan_lex_heredoc_header_t);
core_list_decl(scan_lex_heredoc_t);
core_list_decl(scan_lex_embed_t);
core_list_decl(scan_lex_fence_t);
core_list_decl(scan_lex_directive_t);
core_list_decl(scan_lex_literal_hook_t);
core_list_decl(scan_lex_recovery_t);
core_list_decl(scan_lex_declaration_t);
core_list_decl(scan_lex_identifier_profile_t);
core_list_decl(scan_lex_radix_t);
core_list_decl(scan_lex_suffix_t);
core_list_decl(scan_lex_number_form_t);

typedef scan_lex_escape_t *lex_escape_rules_ptr_t;
core_list_decl(lex_escape_rules_ptr_t);
typedef scan_lex_suffix_t *lex_suffix_rules_ptr_t;
core_list_decl(lex_suffix_rules_ptr_t);
typedef core_string_t *lex_string_array_ptr_t;
core_list_decl(lex_string_array_ptr_t);

typedef struct {
    core_string_t              name;
    core_string_t              origin;
    core_list_t(core_string_t) operators;
    core_list_t(core_string_t) origins;
} lex_operator_set_t;

core_list_decl(lex_operator_set_t);

typedef struct {
    core_string_t                    name;
    core_string_t                    origin;
    core_list_t(scan_lex_suffix_t)   suffixes;
} lex_number_suffix_set_t;

core_list_decl(lex_number_suffix_set_t);

typedef struct {
    core_string_t                  name;
    core_string_t                  origin;
    core_string_t                  simple;
    core_list_t(scan_lex_escape_t) rules;
} lex_string_escape_set_t;

core_list_decl(lex_string_escape_set_t);

typedef struct {
    scan_lex_spec_t                    spec;
    core_list_t(scan_lex_decl_t)       decls;
    core_list_t(scan_lex_comment_t)    comments;
    core_list_t(scan_lex_region_t)     regions;
    core_list_t(scan_lex_string_t)     strings;
    core_list_t(scan_lex_sigil_t)      sigils;
    core_list_t(scan_lex_regex_t)      regexes;
    core_list_t(scan_lex_heredoc_header_t) heredoc_headers;
    core_list_t(scan_lex_heredoc_t)    heredocs;
    core_list_t(scan_lex_embed_t)      embeds;
    core_list_t(scan_lex_fence_t)      fences;
    core_list_t(scan_lex_directive_t)  directives;
    core_list_t(scan_lex_literal_hook_t) literal_hooks;
    core_list_t(scan_lex_recovery_t)     recoveries;
    core_list_t(scan_lex_declaration_t)  declarations;
    core_list_t(scan_lex_identifier_profile_t) identifier_profiles;
    core_list_t(scan_lex_radix_t)      radixes;
    core_list_t(scan_lex_suffix_t)     suffixes;
    core_list_t(scan_lex_number_form_t) number_forms;
    core_list_t(lex_operator_set_t)    operator_sets;
    core_list_t(lex_number_suffix_set_t) number_suffix_sets;
    core_list_t(lex_string_escape_set_t) string_escape_sets;
    core_list_t(core_string_t)         operators;
    core_list_t(core_string_t)         operator_origins;
    core_list_t(core_string_t)         soft_keywords;
    core_list_t(core_string_t)         generated_strings;
    core_list_t(lex_escape_rules_ptr_t) escape_rule_sets;
    core_list_t(lex_suffix_rules_ptr_t) owned_number_suffixes;
    core_list_t(lex_string_array_ptr_t) owned_string_arrays;
    core_dict_t                        operator_set_index;
    core_dict_t                        operator_set_declarations;
    core_dict_t                        operator_state;
    core_dict_t                        operator_index;
    core_dict_t                        soft_keyword_index;
    core_dict_t                        string_openers;
    core_dict_t                        number_prefixes;
    core_dict_t                        number_suffix_set_index;
    core_dict_t                        number_suffix_set_declarations;
    core_dict_t                        string_escape_set_index;
    core_dict_t                        string_escape_set_declarations;
    core_dict_t                        heredoc_header_index;
    core_dict_t                        identifier_profile_index;

    char err[192];
} lex_build_t;

static_assert(offsetof(lex_build_t, spec) == 0,
              "scan_lex_spec_free frees the build through its spec");

static _Thread_local char lex_build_err[192];

enum {
    OPERATOR_ACTIVE = 1,
    OPERATOR_REMOVED = 2,
};

static core_hash_value_t
string_key_hash(void *key)
{
    return core_string_hash(*(core_string_t *)key);
}

static bool
string_key_eq(const void *a, const void *b)
{
    return core_string_eq(*(const core_string_t *)a,
                          *(const core_string_t *)b);
}

static void
string_map_free(core_dict_t *map)
{
    core_dict_foreach(map, item) {
        core_free(item->key);
    }
    core_dict_free(map);
}

static void
string_map_put_new(core_dict_t *map, core_string_t key, uintptr_t value)
{
    core_string_t *stored = core_alloc(core_string_t);
    *stored = key;
    core_dict_put(map, stored, value);
}

static uintptr_t
string_map_get(core_dict_t *map, core_string_t key)
{
    bool found = false;
    void *value = core_dict_get(map, &key, &found);
    return found ? (uintptr_t)value : 0;
}

static void
append_error_origin(lex_build_t *b, core_string_t origin, const char *label)
{
    if (!b->err[0] || !origin.data || !origin.u8_bytes) {
        return;
    }

    size_t used = strlen(b->err);

    snprintf(b->err + used, sizeof(b->err) - used, "; %s '%.*s'",
             label, (int)origin.u8_bytes, origin.data);
}

static void
record_declaration(lex_build_t *b, scan_lex_decl_kind_t kind,
                   core_string_t spelling, core_string_t origin)
{
    if (!spelling.data) {
        return;
    }
    core_list_push(b->declarations,
                   ((scan_lex_declaration_t){
                       .kind = kind,
                       .spelling = spelling,
                       .origin = origin,
                   }));
}

static void
lex_build_free(lex_build_t *b)
{
    for (size_t i = 0; i < core_list_len(b->escape_rule_sets); i++) {
        core_free(b->escape_rule_sets.data[i]);
    }
    for (size_t i = 0; i < core_list_len(b->owned_number_suffixes); i++) {
        core_free(b->owned_number_suffixes.data[i]);
    }
    for (size_t i = 0; i < core_list_len(b->owned_string_arrays); i++) {
        core_free(b->owned_string_arrays.data[i]);
    }

    core_list_free(b->comments);
    core_list_free(b->regions);
    core_list_free(b->strings);
    core_list_free(b->sigils);
    core_list_free(b->regexes);
    core_list_free(b->heredoc_headers);
    core_list_free(b->heredocs);
    core_list_free(b->embeds);
    core_list_free(b->fences);
    core_list_free(b->directives);
    core_list_free(b->literal_hooks);
    core_list_free(b->recoveries);
    core_list_free(b->declarations);
    core_list_free(b->identifier_profiles);
    core_list_free(b->radixes);
    core_list_free(b->suffixes);
    core_list_free(b->number_forms);

    for (size_t i = 0; i < core_list_len(b->operator_sets); i++) {
        core_list_free(b->operator_sets.data[i].operators);
        core_list_free(b->operator_sets.data[i].origins);
    }
    for (size_t i = 0; i < core_list_len(b->number_suffix_sets); i++) {
        core_list_free(b->number_suffix_sets.data[i].suffixes);
    }
    for (size_t i = 0; i < core_list_len(b->string_escape_sets); i++) {
        core_list_free(b->string_escape_sets.data[i].rules);
    }

    core_list_free(b->operator_sets);
    core_list_free(b->owned_number_suffixes);
    core_list_free(b->owned_string_arrays);
    core_list_free(b->operators);
    core_list_free(b->operator_origins);
    core_list_free(b->soft_keywords);
    for (size_t i = 0; i < core_list_len(b->generated_strings); i++) {
        core_free(b->generated_strings.data[i].data);
    }
    core_list_free(b->generated_strings);
    core_list_free(b->escape_rule_sets);
    core_list_free(b->number_suffix_sets);
    core_list_free(b->string_escape_sets);
    string_map_free(&b->operator_set_index);
    string_map_free(&b->operator_set_declarations);
    string_map_free(&b->operator_state);
    string_map_free(&b->operator_index);
    string_map_free(&b->soft_keyword_index);
    string_map_free(&b->string_openers);
    string_map_free(&b->number_prefixes);
    string_map_free(&b->number_suffix_set_index);
    string_map_free(&b->number_suffix_set_declarations);
    string_map_free(&b->string_escape_set_index);
    string_map_free(&b->string_escape_set_declarations);
    string_map_free(&b->heredoc_header_index);
    string_map_free(&b->identifier_profile_index);
    scan_lex_decls_free(&b->decls);
    core_free(b);
}

const char *
scan_lex_build_error(void)
{
    return lex_build_err[0] ? lex_build_err : scan_lex_compile_error();
}

void
scan_lex_note_unresolved_import(void)
{
    snprintf(lex_build_err, sizeof(lex_build_err),
             "@lexer import was not resolved; load the grammar with "
             "parse_bnf_load_file");
}

static bool
named(const scan_lex_opt_t *o, const char *what)
{
    return o->name.data && o->name.u8_bytes == strlen(what)
        && !memcmp(o->name.data, what, o->name.u8_bytes);
}

static core_string_t
arg(const scan_lex_opt_t *o, size_t i)
{
    return i < core_list_len(o->args) ? o->args.data[i]
                                     : (core_string_t){0};
}

static bool
string_is(core_string_t value, const char *expected)
{
    size_t len = strlen(expected);

    return value.data && value.u8_bytes == len
        && !memcmp(value.data, expected, len);
}

static bool
layout_policy(lex_build_t *b, const scan_lex_opt_t *o,
              scan_lex_indent_policy_t *out)
{
    core_string_t value = arg(o, 0);

    if (core_list_len(o->args) != 1) {
        if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-layout: %.*s expects one policy",
                     (int)o->name.u8_bytes, o->name.data);
        }

        return false;
    }

    if (string_is(value, "allow")) {
        *out = SCAN_LEX_INDENT_ALLOW;
        return true;
    }

    if (string_is(value, "reject")) {
        *out = SCAN_LEX_INDENT_REJECT;
        return true;
    }

    if (!b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-layout: %.*s expects 'allow' or 'reject'",
                 (int)o->name.u8_bytes, o->name.data);
    }

    return false;
}

static bool
layout_line_policy(lex_build_t *b, const scan_lex_opt_t *o,
                   scan_lex_indent_line_policy_t *out)
{
    core_string_t value = arg(o, 0);

    if (core_list_len(o->args) != 1) {
        if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-layout: %.*s expects one policy",
                     (int)o->name.u8_bytes, o->name.data);
        }

        return false;
    }

    if (string_is(value, "ignore")) {
        *out = SCAN_LEX_INDENT_IGNORE;
        return true;
    }

    if (string_is(value, "apply")) {
        *out = SCAN_LEX_INDENT_APPLY;
        return true;
    }

    if (!b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-layout: %.*s expects 'ignore' or 'apply'",
                 (int)o->name.u8_bytes, o->name.data);
    }

    return false;
}

static bool
layout_tab_width(lex_build_t *b, const scan_lex_opt_t *o, uint8_t *out)
{
    core_string_t value = arg(o, 0);
    uint32_t      width = 0;
    bool          valid = core_list_len(o->args) == 1;

    for (size_t i = 0; valid && i < value.u8_bytes; i++) {
        char c = value.data[i];

        if (c < '0' || c > '9') {
            valid = false;
            continue;
        }

        width = width * 10 + (uint32_t)(c - '0');

        if (width > 32) {
            valid = false;
        }
    }

    if (valid && value.u8_bytes && width >= 1) {
        *out = (uint8_t)width;
        return true;
    }

    if (!b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-layout: tab_width expects an integer from 1 to 32");
    }

    return false;
}

static core_string_t
positional(const core_list_t(scan_lex_opt_t) *opts, size_t which)
{
    size_t seen = 0;

    for (size_t i = 0; i < core_list_len(*opts); i++) {
        const scan_lex_opt_t *o = &opts->data[i];

        if (o->name.data) {
            continue;
        }

        if (seen++ == which) {
            return arg(o, 0);
        }
    }

    return (core_string_t){0};
}

static const core_string_t *
own_string_array(lex_build_t *b, const core_list_t(core_string_t) *items)
{
    if (!core_list_len(*items)) {
        return nullptr;
    }

    core_string_t *owned = core_alloc_array(core_string_t,
                                             core_list_len(*items) + 1);

    memcpy(owned, items->data,
           core_list_len(*items) * sizeof(core_string_t));
    core_list_push(b->owned_string_arrays, owned);

    return owned;
}

static void
oops(lex_build_t *b, const char *kind, core_string_t name)
{
    if (b->err[0]) {
        return;
    }

    snprintf(b->err,
             sizeof(b->err),
             "@lex-%s: unknown option '%.*s'",
             kind,
             name.data ? (int)name.u8_bytes : 1,
             name.data ? name.data : "?");
}

static void
build_comment(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_comment_t item = {0};
    scan_lex_comment_t *c   = &item;

    c->opener = positional(&a->lex_opts, 0);
    c->closer = positional(&a->lex_opts, 1);
    c->kind   = c->closer.data ? SCAN_LEX_BLOCK : SCAN_LEX_LINE;

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "nested")) {
            c->nested = true;
        }
        else if (named(o, "fill")) {
            c->fill = arg(o, 0);
            if (core_list_len(o->args) != 1 && !b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-comment: fill() expects one string");
            }
        }
        else if (named(o, "as")) {
            c->tok = arg(o, 0);
        }
        else {
            oops(b, "comment", o->name);
        }
    }

    if (!c->opener.data) {
        return;
    }

    if (c->fill.data && !c->fill.u8_bytes && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-comment: fill() cannot be empty");
    }
    if (c->fill.data && (c->kind != SCAN_LEX_BLOCK || c->nested)
        && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-comment: fill() requires a non-nested block comment");
    }

    core_list_push(b->comments, item);
}

static void
build_region(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_region_t region = {
        .opener = positional(&a->lex_opts, 0),
        .closer = positional(&a->lex_opts, 1),
        .terminator = positional(&a->lex_opts, 2),
    };
    if (!region.opener.data || !region.closer.data
        || !region.terminator.data) {
        if (!b->err[0])
            snprintf(b->err, sizeof(b->err),
                     "@lex-region expects open, close, and terminator strings");
        return;
    }
    if (!region.opener.u8_bytes || !region.closer.u8_bytes
        || !region.terminator.u8_bytes) {
        if (!b->err[0])
            snprintf(b->err, sizeof(b->err),
                     "@lex-region delimiters cannot be empty");
        return;
    }
    core_list_push(b->regions, region);
}

static lex_string_escape_set_t *
string_escape_set(lex_build_t *b, core_string_t name)
{
    uintptr_t index = string_map_get(&b->string_escape_set_index, name);
    return index ? &b->string_escape_sets.data[index - 1] : nullptr;
}

static void
merge_simple_escapes(lex_build_t *b, scan_lex_string_t *s,
                     core_string_t extra)
{
    bool present[128] = {0};

    for (size_t i = 0; i < s->simple_escapes.u8_bytes; i++) {
        unsigned char c = (unsigned char)s->simple_escapes.data[i];
        if (c < 128) {
            present[c] = true;
        }
    }
    for (size_t i = 0; i < extra.u8_bytes; i++) {
        unsigned char c = (unsigned char)extra.data[i];
        if (c >= 128) {
            if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-string: simple escapes must be ASCII");
            }
            return;
        }
        present[c] = true;
    }

    size_t length = 0;
    for (size_t i = 0; i < 128; i++) {
        length += present[i];
    }
    char *data = core_alloc_size(1, length + 1);
    size_t at = 0;
    for (size_t i = 0; i < 128; i++) {
        if (present[i]) {
            data[at++] = (char)i;
        }
    }
    core_string_t merged = {.data = data, .u8_bytes = length};
    core_list_push(b->generated_strings, merged);
    s->simple_escapes = merged;
}

static void
package_add_escape(lex_build_t *b, lex_string_escape_set_t *set,
                   scan_lex_escape_t rule)
{
    for (size_t i = 0; i < set->rules.len; i++) {
        scan_lex_escape_t prior = set->rules.data[i];

        if (!core_string_eq(prior.prefix, rule.prefix)) {
            continue;
        }
        if ((!core_string_eq(prior.digits, rule.digits)
             || prior.width != rule.width)
            && !b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-string-escapes: package '%.*s' has duplicate "
                     "escape prefix '%.*s' with conflicting definitions",
                     (int)set->name.u8_bytes, set->name.data,
                     (int)rule.prefix.u8_bytes, rule.prefix.data);
            append_error_origin(b, set->origin, "declared in");
        }
        return;
    }

    core_list_push(set->rules, rule);
}

static bool
build_string_option(lex_build_t *b, scan_lex_string_t *s,
                    core_list_t(scan_lex_escape_t) *escapes,
                    const scan_lex_opt_t *o, const char *kind)
{
    if (named(o, "escape")) {
        s->escape = arg(o, 0);
    }
    else if (named(o, "raw")) {
        s->flags |= SCAN_LEX_STR_RAW;
    }
    else if (named(o, "multiline")) {
        s->flags |= SCAN_LEX_STR_MULTILINE;
    }
    else if (named(o, "interp")) {
        s->interp_open  = arg(o, 0);
        s->interp_close = arg(o, 1);
    }
    else if (named(o, "hole_tokens")) {
        s->flags |= SCAN_LEX_STR_HOLE_TOKENS;
    }
    else if (named(o, "scalar")) {
        s->flags |= SCAN_LEX_STR_ONE_SCALAR;
    }
    else if (named(o, "nested")) {
        s->flags |= SCAN_LEX_STR_NESTED;
        s->nest_open = arg(o, 0);
    }
    else if (named(o, "escapes")) {
        merge_simple_escapes(b, s, arg(o, 0));
    }
    else if (named(o, "escape_literal")) {
        core_string_t prefix = arg(o, 0);
        if (!prefix.data || !prefix.u8_bytes) {
            if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-%s: escape_literal() needs a nonempty prefix",
                         kind);
            }
        }
        else {
            core_list_push(*escapes,
                ((scan_lex_escape_t){.prefix = prefix, .width = 0}));
        }
    }
    else if (named(o, "escape_digits")) {
        core_string_t width_text = arg(o, 2);
        uint32_t      width      = 0;
        bool          valid      = width_text.u8_bytes > 0;

        for (size_t j = 0; j < width_text.u8_bytes; j++) {
            unsigned char c = (unsigned char)width_text.data[j];

            if (c < '0' || c > '9') {
                valid = false;
                break;
            }

            uint32_t digit = (uint32_t)(c - '0');
            if (width > (UINT32_MAX - digit) / 10u) {
                valid = false;
                break;
            }

            width = width * 10u + digit;
        }

        if (!arg(o, 0).data || arg(o, 0).u8_bytes != 1
            || (unsigned char)arg(o, 0).data[0] >= 128
            || !arg(o, 1).data || !valid || !width || width > 32) {
            if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-%s: escape_digits() needs a one-byte ASCII "
                         "prefix and a width from 1 through 32", kind);
            }
        }
        else {
            core_list_push(*escapes,
                           ((scan_lex_escape_t){.prefix = arg(o, 0),
                                                .digits = arg(o, 1),
                                                .width  = width}));
        }
    }
    else if (named(o, "as")) {
        s->tok     = arg(o, 0);
        s->tok_mid = arg(o, 1);
        s->tok_end = arg(o, 2);
    }
    else if (named(o, "use_escapes")) {
        for (size_t i = 0; i < core_list_len(o->args); i++) {
            lex_string_escape_set_t *set
                = string_escape_set(b, o->args.data[i]);

            if (!set) {
                if (!b->err[0]) {
                    snprintf(b->err, sizeof(b->err),
                             "@lex-%s: unknown escape set '%.*s'", kind,
                             (int)o->args.data[i].u8_bytes,
                             o->args.data[i].data);
                }
                continue;
            }
            merge_simple_escapes(b, s, set->simple);
            for (size_t j = 0; j < core_list_len(set->rules); j++) {
                core_list_push(*escapes, set->rules.data[j]);
            }
        }
    }
    else {
        return false;
    }

    return true;
}

static void
build_string_escapes(lex_build_t *b, const scan_lex_decl_t *a)
{
    core_string_t name = positional(&a->lex_opts, 0);

    if (!name.data || !name.u8_bytes) {
        oops(b, "string-escapes", CORE_STRING_STATIC("name"));
        return;
    }
    if (string_escape_set(b, name)) {
        if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-string-escapes: duplicate set '%.*s'",
                     (int)name.u8_bytes, name.data);
        }
        return;
    }

    scan_lex_string_t prototype = {0};
    lex_string_escape_set_t set = {
        .name = name,
        .origin = a->source_origin,
        .rules = core_list_new(scan_lex_escape_t),
    };

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }
        if (named(o, "use")) {
            for (size_t j = 0; j < o->args.len; j++) {
                lex_string_escape_set_t *base
                    = string_escape_set(b, o->args.data[j]);
                if (!base) {
                    continue;
                }
                merge_simple_escapes(b, &prototype, base->simple);
                for (size_t k = 0; k < base->rules.len; k++) {
                    package_add_escape(b, &set, base->rules.data[k]);
                }
            }
        }
        else if (named(o, "remove")) {
            for (size_t j = 0; j < o->args.len; j++) {
                core_string_t prefix = o->args.data[j];
                bool removed = false;
                bool keep[128] = {0};
                for (size_t k = 0; k < prototype.simple_escapes.u8_bytes;
                     k++) {
                    unsigned char c
                        = (unsigned char)prototype.simple_escapes.data[k];
                    if (prefix.u8_bytes == 1
                        && c == (unsigned char)prefix.data[0]) {
                        removed = true;
                    }
                    else if (c < 128) {
                        keep[c] = true;
                    }
                }
                for (size_t k = 0; k < set.rules.len;) {
                    if (core_string_eq(set.rules.data[k].prefix, prefix)) {
                        core_list_delete(set.rules, k);
                        removed = true;
                    }
                    else {
                        k++;
                    }
                }
                if (!removed && !b->err[0]) {
                    snprintf(b->err, sizeof(b->err),
                             "@lex-string-escapes: package '%.*s' cannot "
                             "remove absent escape '%.*s'",
                             (int)name.u8_bytes, name.data,
                             (int)prefix.u8_bytes, prefix.data);
                    append_error_origin(b, a->source_origin, "declared in");
                }
                if (removed) {
                    size_t length = 0;
                    for (size_t k = 0; k < 128; k++) {
                        length += keep[k];
                    }
                    char *data = core_alloc_size(1, length + 1);
                    size_t at = 0;
                    for (size_t k = 0; k < 128; k++) {
                        if (keep[k]) {
                            data[at++] = (char)k;
                        }
                    }
                    core_string_t simple = {.data = data,
                                            .u8_bytes = length};
                    core_list_push(b->generated_strings, simple);
                    prototype.simple_escapes = simple;
                }
            }
        }
        else if (!named(o, "escapes") && !named(o, "escape_literal")
                 && !named(o, "escape_digits")) {
            oops(b, "string-escapes", o->name);
            continue;
        }
        else {
            size_t before = set.rules.len;

            build_string_option(b, &prototype, &set.rules, o,
                                "string-escapes");
            if (named(o, "escape_digits") && set.rules.len > before) {
                scan_lex_escape_t added = set.rules.data[set.rules.len - 1];

                set.rules.len--;
                package_add_escape(b, &set, added);
            }
        }
    }
    set.simple = prototype.simple_escapes;

    core_dict_t seen;
    core_dict_init(&seen, string_key_hash, string_key_eq);
    for (size_t i = 0; i < core_list_len(set.rules); i++) {
        core_string_t prefix = set.rules.data[i].prefix;

        if (string_map_get(&seen, prefix) && !b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-string-escapes: duplicate escape prefix '%.*s'",
                     (int)prefix.u8_bytes, prefix.data);
        }
        else if (!string_map_get(&seen, prefix)) {
            string_map_put_new(&seen, prefix, 1);
        }
    }
    string_map_free(&seen);
    if (!set.simple.u8_bytes && !core_list_len(set.rules) && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-string-escapes: set '%.*s' cannot be empty",
                 (int)name.u8_bytes, name.data);
    }

    core_list_push(b->string_escape_sets, set);
    string_map_put_new(&b->string_escape_set_index, name,
                       b->string_escape_sets.len);
}

static void
push_string(lex_build_t *b, scan_lex_string_t item,
            const scan_lex_escape_t *escape_rules, const char *kind)
{
    if (!item.opener.data || !item.opener.u8_bytes) {
        if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-%s: every string needs a nonempty opener", kind);
        }
        return;
    }

    if ((item.flags & SCAN_LEX_STR_ONE_SCALAR) && item.interp_open.data) {
        if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-%s: scalar strings cannot interpolate", kind);
        }
        return;
    }
    if ((item.flags & SCAN_LEX_STR_NESTED) && !item.nest_open.data) {
        item.nest_open = item.opener;
    }
    if ((item.flags & SCAN_LEX_STR_NESTED)
        && ((item.flags & SCAN_LEX_STR_ONE_SCALAR)
            || core_string_eq(item.nest_open, item.closer))) {
        if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-%s: nested strings need distinct paired "
                     "delimiters and cannot be scalar", kind);
        }
        return;
    }

    if (string_map_get(&b->string_openers, item.opener)) {
        if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-%s: duplicate string opener '%.*s'", kind,
                     (int)item.opener.u8_bytes, item.opener.data);
        }
        return;
    }
    string_map_put_new(&b->string_openers, item.opener, 1);

    item.escape_rules = escape_rules;

    core_list_push(b->strings, item);
}

static const scan_lex_escape_t *
own_escape_rules(lex_build_t *b,
                 const core_list_t(scan_lex_escape_t) *escapes)
{
    if (!core_list_len(*escapes)) {
        return nullptr;
    }

    core_dict_t seen;
    core_dict_init(&seen, string_key_hash, string_key_eq);
    for (size_t i = 0; i < core_list_len(*escapes); i++) {
        core_string_t prefix = escapes->data[i].prefix;

        if (string_map_get(&seen, prefix)) {
            if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-string: duplicate escape prefix '%.*s'",
                         (int)prefix.u8_bytes, prefix.data);
            }
            continue;
        }
        string_map_put_new(&seen, prefix, 1);
    }
    string_map_free(&seen);

    if (b->err[0]) {
        return nullptr;
    }

    scan_lex_escape_t *rules
        = core_alloc_array(scan_lex_escape_t, escapes->len + 1);

    memcpy(rules, escapes->data, escapes->len * sizeof(*rules));
    core_list_push(b->escape_rule_sets, rules);
    return rules;
}

static core_string_t
joined_opener(lex_build_t *b, core_string_t prefix, core_string_t quote)
{
    size_t length = prefix.u8_bytes + quote.u8_bytes;
    char  *data   = core_alloc_size(1, length + 1);

    memcpy(data, prefix.data, prefix.u8_bytes);
    memcpy(data + prefix.u8_bytes, quote.data, quote.u8_bytes);

    core_string_t result = {.data = data, .u8_bytes = length};
    core_list_push(b->generated_strings, result);
    return result;
}

static void
build_string(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_string_t item = {0};
    core_list_t(scan_lex_escape_t) escapes
        = core_list_new(scan_lex_escape_t);

    item.opener = positional(&a->lex_opts, 0);
    item.closer = positional(&a->lex_opts, 1);

    if (!item.closer.data) {
        item.closer = item.opener;
    }

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }
        if (!build_string_option(b, &item, &escapes, o, "string")) {
            oops(b, "string", o->name);
        }
    }

    if (item.opener.data) {
        push_string(b, item, own_escape_rules(b, &escapes), "string");
    }

    core_list_free(escapes);
}

static void
build_string_family(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_string_t prototype = {0};
    core_list_t(scan_lex_escape_t) escapes
        = core_list_new(scan_lex_escape_t);
    core_list_t(core_string_t) prefixes = core_list_new(core_string_t);
    core_list_t(core_string_t) quotes   = core_list_new(core_string_t);
    core_list_t(core_string_t) pair_opens = core_list_new(core_string_t);
    core_list_t(core_string_t) pair_closes = core_list_new(core_string_t);
    bool plain = false;

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-string-family: use prefixes() and quotes()");
            }
        }
        else if (named(o, "plain")) {
            plain = true;
        }
        else if (named(o, "prefixes")) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                core_list_push(prefixes, o->args.data[j]);
            }
        }
        else if (named(o, "quotes")) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                core_list_push(quotes, o->args.data[j]);
            }
        }
        else if (named(o, "pair")) {
            if (core_list_len(o->args) != 2
                || !arg(o, 0).u8_bytes || !arg(o, 1).u8_bytes) {
                if (!b->err[0]) {
                    snprintf(b->err, sizeof(b->err),
                             "@lex-string-family: pair() needs an opener "
                             "and closer");
                }
            }
            else {
                core_list_push(pair_opens, arg(o, 0));
                core_list_push(pair_closes, arg(o, 1));
            }
        }
        else if (!build_string_option(b, &prototype, &escapes, o,
                                      "string-family")) {
            oops(b, "string-family", o->name);
        }
    }

    if (!core_list_len(quotes) && !core_list_len(pair_opens) && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-string-family: specify quotes() or pair()");
    }
    if (!plain && !core_list_len(prefixes) && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-string-family: specify plain or prefixes()");
    }

    const scan_lex_escape_t *escape_rules = b->err[0]
        ? nullptr : own_escape_rules(b, &escapes);

    for (size_t p = 0; !b->err[0] && p < core_list_len(prefixes); p++) {
        for (size_t q = 0; q < core_list_len(quotes); q++) {
            scan_lex_string_t item = prototype;

            item.opener = joined_opener(b, prefixes.data[p], quotes.data[q]);
            item.closer = quotes.data[q];
            item.nest_open = quotes.data[q];
            push_string(b, item, escape_rules, "string-family");
        }
        for (size_t q = 0; q < core_list_len(pair_opens); q++) {
            scan_lex_string_t item = prototype;

            item.opener = joined_opener(b, prefixes.data[p],
                                        pair_opens.data[q]);
            item.closer = pair_closes.data[q];
            item.nest_open = pair_opens.data[q];
            push_string(b, item, escape_rules, "string-family");
        }
    }

    if (plain) {
        for (size_t q = 0; !b->err[0] && q < core_list_len(quotes); q++) {
            scan_lex_string_t item = prototype;

            item.opener = quotes.data[q];
            item.closer = quotes.data[q];
            item.nest_open = quotes.data[q];
            push_string(b, item, escape_rules, "string-family");
        }
        for (size_t q = 0; !b->err[0] && q < core_list_len(pair_opens); q++) {
            scan_lex_string_t item = prototype;

            item.opener = pair_opens.data[q];
            item.closer = pair_closes.data[q];
            item.nest_open = pair_opens.data[q];
            push_string(b, item, escape_rules, "string-family");
        }
    }

    core_list_free(prefixes);
    core_list_free(quotes);
    core_list_free(pair_opens);
    core_list_free(pair_closes);
    core_list_free(escapes);
}

static void
build_legacy_number(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_number_t *n = &b->spec.number;

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "radix")) {
            core_list_push(b->radixes,
                           ((scan_lex_radix_t){.prefix = arg(o, 0),
                                               .digits = arg(o, 1),
                                               .tok    = arg(o, 2)}));
        }
        else if (named(o, "suffix")) {
            core_list_push(b->suffixes,
                           ((scan_lex_suffix_t){.suffix = arg(o, 0),
                                                .tok    = arg(o, 1)}));
        }
        else if (named(o, "sep")) {
            n->sep = arg(o, 0);
        }
        else if (named(o, "sign")) {
            n->sign = arg(o, 0);
        }
        else if (named(o, "exp")) {
            n->exponent = arg(o, 0);
        }
        else if (named(o, "int")) {
            n->tok_int = arg(o, 0);
        }
        else if (named(o, "float")) {
            n->tok_float = arg(o, 0);
        }
        else {
            oops(b, "number", o->name);
        }
    }
}

static bool
number_form_syntax(const scan_lex_decl_t *a)
{
    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (named(o, "prefix") || named(o, "digits")
            || named(o, "point") || named(o, "exponent_digits")) {
            return true;
        }
    }

    return false;
}

static const scan_lex_suffix_t *
own_suffix_rules(lex_build_t *b,
                 const core_list_t(scan_lex_suffix_t) *suffixes)
{
    if (!core_list_len(*suffixes)) {
        return nullptr;
    }

    scan_lex_suffix_t *rules
        = core_alloc_array(scan_lex_suffix_t, suffixes->len + 1);

    memcpy(rules, suffixes->data, suffixes->len * sizeof(*rules));
    core_list_push(b->owned_number_suffixes, rules);
    return rules;
}

static scan_lex_suffix_t
number_suffix_option(lex_build_t *b, const scan_lex_opt_t *o)
{
    uint8_t forms = SCAN_LEX_SUFFIX_INTEGER | SCAN_LEX_SUFFIX_FLOAT;

    if (core_list_len(o->args) > 2) {
        core_string_t applies = arg(o, 2);

        if (core_string_eq(applies, CORE_STRING_STATIC("integer"))) {
            forms = SCAN_LEX_SUFFIX_INTEGER;
        }
        else if (core_string_eq(applies, CORE_STRING_STATIC("float"))) {
            forms = SCAN_LEX_SUFFIX_FLOAT;
        }
        else if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-number: unknown suffix form '%.*s'",
                     (int)applies.u8_bytes, applies.data);
        }
    }

    return (scan_lex_suffix_t){.suffix = arg(o, 0),
                               .tok = arg(o, 1), .forms = forms};
}

static lex_number_suffix_set_t *
number_suffix_set(lex_build_t *b, core_string_t name)
{
    uintptr_t index = string_map_get(&b->number_suffix_set_index, name);
    return index ? &b->number_suffix_sets.data[index - 1] : nullptr;
}

static void
build_number_suffixes(lex_build_t *b, const scan_lex_decl_t *a)
{
    core_string_t name = positional(&a->lex_opts, 0);

    if (!name.data || !name.u8_bytes) {
        oops(b, "number-suffixes", CORE_STRING_STATIC("name"));
        return;
    }
    if (number_suffix_set(b, name)) {
        if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-number-suffixes: duplicate set '%.*s'",
                     (int)name.u8_bytes, name.data);
        }
        return;
    }

    lex_number_suffix_set_t set = {
        .name = name,
        .origin = a->source_origin,
        .suffixes = core_list_new(scan_lex_suffix_t),
    };

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }
        if (named(o, "suffix")) {
            scan_lex_suffix_t suffix = number_suffix_option(b, o);
            bool found = false;
            for (size_t j = 0; j < set.suffixes.len; j++) {
                if (!core_string_eq(set.suffixes.data[j].suffix,
                                    suffix.suffix)) {
                    continue;
                }
                found = true;
                if ((!core_string_eq(set.suffixes.data[j].tok, suffix.tok)
                     || set.suffixes.data[j].forms != suffix.forms)
                    && !b->err[0]) {
                    snprintf(b->err, sizeof(b->err),
                             "@lex-number-suffixes: package '%.*s' "
                             "redefines suffix '%.*s' incompatibly",
                             (int)name.u8_bytes, name.data,
                             (int)suffix.suffix.u8_bytes,
                             suffix.suffix.data);
                }
                break;
            }
            if (!found) {
                core_list_push(set.suffixes, suffix);
            }
        }
        else if (named(o, "use")) {
            for (size_t j = 0; j < o->args.len; j++) {
                lex_number_suffix_set_t *base
                    = number_suffix_set(b, o->args.data[j]);
                for (size_t k = 0; base && k < base->suffixes.len; k++) {
                    scan_lex_suffix_t suffix = base->suffixes.data[k];
                    bool found = false;
                    for (size_t m = 0; m < set.suffixes.len; m++) {
                        if (!core_string_eq(set.suffixes.data[m].suffix,
                                            suffix.suffix)) {
                            continue;
                        }
                        found = true;
                        if ((!core_string_eq(set.suffixes.data[m].tok,
                                             suffix.tok)
                             || set.suffixes.data[m].forms != suffix.forms)
                            && !b->err[0]) {
                            snprintf(b->err, sizeof(b->err),
                                     "@lex-number-suffixes: package '%.*s' "
                                     "inherits conflicting suffix '%.*s'",
                                     (int)name.u8_bytes, name.data,
                                     (int)suffix.suffix.u8_bytes,
                                     suffix.suffix.data);
                        }
                        break;
                    }
                    if (!found) {
                        core_list_push(set.suffixes, suffix);
                    }
                }
            }
        }
        else if (named(o, "remove")) {
            for (size_t j = 0; j < o->args.len; j++) {
                bool removed = false;
                for (size_t k = 0; k < set.suffixes.len; k++) {
                    if (core_string_eq(set.suffixes.data[k].suffix,
                                       o->args.data[j])) {
                        core_list_delete(set.suffixes, k);
                        removed = true;
                        break;
                    }
                }
                if (!removed && !b->err[0]) {
                    snprintf(b->err, sizeof(b->err),
                             "@lex-number-suffixes: package '%.*s' cannot "
                             "remove absent suffix '%.*s'",
                             (int)name.u8_bytes, name.data,
                             (int)o->args.data[j].u8_bytes,
                             o->args.data[j].data);
                    append_error_origin(b, a->source_origin, "declared in");
                }
            }
        }
        else {
            oops(b, "number-suffixes", o->name);
        }
    }

    core_list_push(b->number_suffix_sets, set);
    string_map_put_new(&b->number_suffix_set_index, name,
                       b->number_suffix_sets.len);
}

static void
build_number_form(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_number_form_t form = {0};
    core_list_t(scan_lex_suffix_t) suffixes
        = core_list_new(scan_lex_suffix_t);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            oops(b, "number", (core_string_t){0});
        }
        else if (named(o, "prefix")) {
            form.prefix = arg(o, 0);
        }
        else if (named(o, "digits")) {
            form.digits = arg(o, 0);
        }
        else if (named(o, "sep")) {
            form.separator = arg(o, 0);
            for (size_t j = 1; j < core_list_len(o->args); j++) {
                core_string_t value = o->args.data[j];

                if (core_string_eq(
                        value, CORE_STRING_STATIC("after-prefix"))) {
                    form.flags |= SCAN_LEX_NUM_SEPARATOR_AFTER_PREFIX;
                }
                else if (!b->err[0]) {
                    snprintf(b->err, sizeof(b->err),
                             "@lex-number: unknown separator policy '%.*s'",
                             (int)value.u8_bytes, value.data);
                }
            }
        }
        else if (named(o, "sign")) {
            form.sign = arg(o, 0);
        }
        else if (named(o, "exp")) {
            form.exponent = arg(o, 0);
        }
        else if (named(o, "exponent_digits")) {
            form.exponent_digits = arg(o, 0);
        }
        else if (named(o, "int")) {
            form.tok_int = arg(o, 0);
        }
        else if (named(o, "float")) {
            form.tok_float = arg(o, 0);
        }
        else if (named(o, "suffix")) {
            core_list_push(suffixes, number_suffix_option(b, o));
        }
        else if (named(o, "use_suffixes")) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                lex_number_suffix_set_t *set
                    = number_suffix_set(b, o->args.data[j]);

                if (!set) {
                    if (!b->err[0]) {
                        snprintf(b->err, sizeof(b->err),
                                 "@lex-number: unknown suffix set '%.*s'",
                                 (int)o->args.data[j].u8_bytes,
                                 o->args.data[j].data);
                    }
                    continue;
                }
                for (size_t k = 0; k < core_list_len(set->suffixes); k++) {
                    core_list_push(suffixes, set->suffixes.data[k]);
                }
            }
        }
        else if (named(o, "point")) {
            form.flags |= SCAN_LEX_NUM_POINT;
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                core_string_t value = o->args.data[j];

                if (core_string_eq(value, CORE_STRING_STATIC("leading"))) {
                    form.flags |= SCAN_LEX_NUM_LEADING_POINT;
                }
                else if (core_string_eq(value,
                                        CORE_STRING_STATIC("trailing"))) {
                    form.flags |= SCAN_LEX_NUM_TRAILING_POINT;
                }
                else if (core_string_eq(
                             value,
                             CORE_STRING_STATIC("exponent-required"))) {
                    form.flags
                        |= SCAN_LEX_NUM_EXPONENT_REQUIRED_FOR_POINT;
                }
                else if (!b->err[0]) {
                    snprintf(b->err, sizeof(b->err),
                             "@lex-number: unknown point policy '%.*s'",
                             (int)value.u8_bytes, value.data);
                }
            }
        }
        else if (named(o, "radix")) {
            if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-number: radix() cannot be mixed with a "
                         "self-contained number form");
            }
        }
        else {
            oops(b, "number", o->name);
        }
    }

    if (!form.digits.data && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-number: a number form requires digits()");
    }
    if ((form.flags & SCAN_LEX_NUM_EXPONENT_REQUIRED_FOR_POINT)
        && !form.exponent.data && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-number: an exponent-required point needs exp()");
    }

    core_dict_t seen_suffixes;
    core_dict_init(&seen_suffixes, string_key_hash, string_key_eq);
    for (size_t i = 0; i < core_list_len(suffixes); i++) {
        core_string_t suffix = suffixes.data[i].suffix;

        if (!suffix.data || !suffix.u8_bytes) {
            if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-number: suffixes cannot be empty");
            }
            continue;
        }
        if (string_map_get(&seen_suffixes, suffix)) {
            if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-number: duplicate suffix '%.*s'",
                         (int)suffix.u8_bytes, suffix.data);
            }
            continue;
        }
        string_map_put_new(&seen_suffixes, suffix, 1);
    }
    string_map_free(&seen_suffixes);

    if (string_map_get(&b->number_prefixes, form.prefix) && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-number: duplicate number prefix '%.*s'",
                 (int)form.prefix.u8_bytes,
                 form.prefix.data ? form.prefix.data : "");
    }

    if (!b->err[0]) {
        string_map_put_new(&b->number_prefixes, form.prefix, 1);
        form.suffixes = own_suffix_rules(b, &suffixes);
        core_list_push(b->number_forms, form);
    }
    core_list_free(suffixes);
}

static void
build_number(lex_build_t *b, const scan_lex_decl_t *a)
{
    if (number_form_syntax(a)) {
        build_number_form(b, a);
    }
    else {
        build_legacy_number(b, a);
    }
}

static void
build_layout(lex_build_t *b, const scan_lex_decl_t *a)
{
    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "indent")) {
            b->spec.layout |= SCAN_LEX_LAYOUT_INDENT;

            if (core_list_len(o->args)) {
                b->spec.tok_indent = arg(o, 0);
                b->spec.tok_dedent = arg(o, 1);
            }
        }
        else if (named(o, "newline")) {
            b->spec.layout |= SCAN_LEX_LAYOUT_NEWLINE;

            if (core_list_len(o->args)) {
                b->spec.tok_newline = arg(o, 0);
            }
            if (core_list_len(o->args) > 1) {
                if (core_list_len(o->args) != 2 ||
                    !string_is(arg(o, 1), "each")) {
                    oops(b, "layout newline mode", arg(o, 1));
                }
                else b->spec.newline_each_line = true;
            }
        }
        else if (named(o, "eof")) {
            b->spec.tok_eof = arg(o, 0);
        }
        else if (named(o, "terminator")) {
            b->spec.tok_terminator   = arg(o, 0);
            b->spec.terminator_after = arg(o, 1);
        }
        else if (named(o, "continue")) {
            b->spec.line_continue = arg(o, 0);
        }
        else if (named(o, "brackets")) {
            b->spec.brackets = arg(o, 0);
        }
        else if (named(o, "tab_width")) {
            layout_tab_width(b, o, &b->spec.indent_tab_width);
        }
        else if (named(o, "tabs")) {
            layout_policy(b, o, &b->spec.indent_tabs);
        }
        else if (named(o, "mixed_indent")) {
            layout_policy(b, o, &b->spec.indent_mixed);
        }
        else if (named(o, "alignment")) {
            layout_policy(b, o, &b->spec.indent_alignment);
        }
        else if (named(o, "unmatched_dedent")) {
            layout_policy(b, o, &b->spec.indent_unmatched);
        }
        else if (named(o, "blank_lines")) {
            layout_line_policy(b, o, &b->spec.indent_blank_lines);
        }
        else if (named(o, "comment_lines")) {
            layout_line_policy(b, o, &b->spec.indent_comment_lines);
        }
        else {
            oops(b, "layout", o->name);
        }
    }
}

static void
build_sigil(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_sigil_t item = {0};
    scan_lex_sigil_t *g   = &item;

    g->prefix = positional(&a->lex_opts, 0);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "as")) {
            g->tok = arg(o, 0);
        }
        else if (named(o, "digits")) {
            g->digits_only = true;
        }
        else if (named(o, "before_string")) {
            g->before_string = true;
        }
        else if (named(o, "unless_closed")) {
            g->unless_closed = true;
        }
        else if (named(o, "attach_previous")) {
            g->attach_previous = true;
        }
        else {
            oops(b, "sigil", o->name);
        }
    }

    if (!g->prefix.data) {
        return;
    }

    core_list_push(b->sigils, item);
}

static void
build_regex(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_regex_t item = {0};
    scan_lex_regex_t *r   = &item;
    core_list_t(core_string_t) not_after = core_list_new(core_string_t);

    r->opener = positional(&a->lex_opts, 0);
    r->closer = positional(&a->lex_opts, 1);

    if (!r->closer.data) {
        r->closer = r->opener;
    }

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "as")) {
            r->tok = arg(o, 0);
        }
        else if (named(o, "flags")) {
            r->suffix_chars = arg(o, 0);
        }
        else if (named(o, "not_after")) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                core_list_push(not_after, o->args.data[j]);
            }
        }
        else {
            oops(b, "regex", o->name);
        }
    }

    if (!r->opener.data) {
        core_list_free(not_after);
        return;
    }

    r->not_after = own_string_array(b, &not_after);
    core_list_free(not_after);
    core_list_push(b->regexes, item);
}

static void
build_heredoc_header(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_heredoc_header_t header = {
        .name = positional(&a->lex_opts, 0),
        .quotes = SCAN_LEX_HEREDOC_UNQUOTED,
        .trailing_text = true,
    };

    if (!header.name.data || !header.name.u8_bytes) {
        oops(b, "heredoc-header", header.name);
        return;
    }
    if (string_map_get(&b->heredoc_header_index, header.name)) {
        if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-heredoc-header: duplicate profile '%.*s'",
                     (int)header.name.u8_bytes, header.name.data);
        }
        return;
    }

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }
        if (named(o, "quotes") && core_list_len(o->args)) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                core_string_t quote = o->args.data[j];
                if (string_is(quote, "single")) {
                    header.quotes |= SCAN_LEX_HEREDOC_SINGLE_QUOTED;
                }
                else if (string_is(quote, "double")) {
                    header.quotes |= SCAN_LEX_HEREDOC_DOUBLE_QUOTED;
                }
                else if (!b->err[0]) {
                    snprintf(b->err, sizeof(b->err),
                             "@lex-heredoc-header: quotes() expects single or double");
                }
            }
        }
        else if (named(o, "quoted_only") && !core_list_len(o->args)) {
            header.quotes &= (uint8_t)~SCAN_LEX_HEREDOC_UNQUOTED;
        }
        else if (named(o, "leading_space") && !core_list_len(o->args)) {
            header.leading_space = true;
        }
        else if (named(o, "strict_trailing") && !core_list_len(o->args)) {
            header.trailing_text = false;
        }
        else if (named(o, "tag_start") && core_list_len(o->args) == 1) {
            header.tag_start_extra = arg(o, 0);
        }
        else if (named(o, "tag_continue") && core_list_len(o->args) == 1) {
            header.tag_continue_extra = arg(o, 0);
        }
        else {
            oops(b, "heredoc-header", o->name);
        }
    }

    if (!(header.quotes & (SCAN_LEX_HEREDOC_UNQUOTED
                           | SCAN_LEX_HEREDOC_SINGLE_QUOTED
                           | SCAN_LEX_HEREDOC_DOUBLE_QUOTED))
        && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-heredoc-header: profile '%.*s' permits no tag form",
                 (int)header.name.u8_bytes, header.name.data);
    }

    core_list_push(b->heredoc_headers, header);
    string_map_put_new(&b->heredoc_header_index, header.name,
                       core_list_len(b->heredoc_headers));
}

static scan_lex_heredoc_header_t *
heredoc_header(lex_build_t *b, core_string_t name)
{
    uintptr_t index = string_map_get(&b->heredoc_header_index, name);
    return index ? &b->heredoc_headers.data[index - 1] : nullptr;
}

static uint8_t
heredoc_quote_mask(lex_build_t *b, const scan_lex_opt_t *o)
{
    uint8_t result = 0;

    for (size_t i = 0; i < core_list_len(o->args); i++) {
        core_string_t value = o->args.data[i];
        if (string_is(value, "unquoted")) {
            result |= SCAN_LEX_HEREDOC_UNQUOTED;
        }
        else if (string_is(value, "single")) {
            result |= SCAN_LEX_HEREDOC_SINGLE_QUOTED;
        }
        else if (string_is(value, "double")) {
            result |= SCAN_LEX_HEREDOC_DOUBLE_QUOTED;
        }
        else if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-heredoc: interpolate() expects unquoted, single, or double");
        }
    }
    return result;
}

static void
build_heredoc(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_heredoc_t item = {0};
    scan_lex_heredoc_t *h   = &item;

    h->opener = positional(&a->lex_opts, 0);
    h->header = (scan_lex_heredoc_header_t){
        .quotes = SCAN_LEX_HEREDOC_UNQUOTED
                | SCAN_LEX_HEREDOC_SINGLE_QUOTED
                | SCAN_LEX_HEREDOC_DOUBLE_QUOTED,
        .trailing_text = true,
    };

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "as")) {
            h->tok = arg(o, 0);
        }
        else if (named(o, "interpolated_as")) {
            h->interpolated_tok = arg(o, 0);
        }
        else if (named(o, "header") && core_list_len(o->args) == 1) {
            scan_lex_heredoc_header_t *profile = heredoc_header(b, arg(o, 0));
            if (profile) {
                h->header = *profile;
            }
            else if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-heredoc: unknown header profile '%.*s'",
                         (int)arg(o, 0).u8_bytes, arg(o, 0).data);
            }
        }
        else if (named(o, "interpolate")) {
            h->interpolate = heredoc_quote_mask(b, o);
        }
        else if (named(o, "terminator_indent")
                 && core_list_len(o->args) == 1) {
            core_string_t policy = arg(o, 0);
            if (string_is(policy, "allow")) {
                h->terminator_indent = true;
            }
            else if (string_is(policy, "exact")) {
                h->terminator_indent = false;
            }
            else if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-heredoc: terminator_indent() expects allow or exact");
            }
        }
        else if (named(o, "body_indent")
                 && core_list_len(o->args) == 1) {
            core_string_t policy = arg(o, 0);
            if (string_is(policy, "strip")) {
                h->body_indent_strip = true;
            }
            else if (string_is(policy, "keep")) {
                h->body_indent_strip = false;
            }
            else if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-heredoc: body_indent() expects strip or keep");
            }
        }
        else if (named(o, "queue") && !core_list_len(o->args)) {
            h->queued = true;
        }
        else if (named(o, "strip")) {
            h->terminator_indent = true;
            h->body_indent_strip = true;
        }
        else {
            oops(b, "heredoc", o->name);
        }
    }

    if (!h->opener.data) {
        return;
    }
    if (h->queued && !h->header.trailing_text && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-heredoc: queue is incompatible with a strict_trailing header");
    }

    core_list_push(b->heredocs, item);
}

static void
build_embed(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_embed_t item = {0};
    scan_lex_embed_t *e   = &item;

    e->open  = positional(&a->lex_opts, 0);
    e->close = positional(&a->lex_opts, 1);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "as")) {
            e->tok = arg(o, 0);

            if (core_list_len(o->args) != 1 && !b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-embed: as() expects one token name");
            }
        }
        else if (named(o, "open_as")) {
            e->open_tok = arg(o, 0);

            if (core_list_len(o->args) != 1 && !b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-embed: open_as() expects one token name");
            }
        }
        else if (named(o, "close_as")) {
            e->close_tok = arg(o, 0);

            if (core_list_len(o->args) != 1 && !b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-embed: close_as() expects one token name");
            }
        }
        else if (named(o, "lexer")) {
            e->lexer = arg(o, 0);

            if (core_list_len(o->args) != 1 && !b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-embed: lexer() expects one lexer name");
            }
        }
        else if (named(o, "tokens")) {
            e->tokens = true;

            if (core_list_len(o->args) && !b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-embed: tokens does not take arguments");
            }
        }
        else {
            oops(b, "embed", o->name);
        }
    }

    if (!e->open.data) {
        return;
    }

    if (e->lexer.data && !e->close.data && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-embed: a delegated embed requires a closing delimiter");
    }
    else if (e->lexer.data && e->tokens
             && (!e->open_tok.data || !e->close_tok.data) && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-embed: tokens mode requires open_as() and close_as()");
    }
    else if (e->lexer.data && !e->tokens && !e->tok.data && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-embed: single-token mode requires as()");
    }
    else if (!e->lexer.data && e->tokens && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-embed: tokens mode requires lexer()");
    }

    core_list_push(b->embeds, item);
}

static void
build_fence(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_fence_t item = {0};
    scan_lex_fence_t *f   = &item;

    f->outer_open  = positional(&a->lex_opts, 0);
    f->outer_close = positional(&a->lex_opts, 1);
    f->minimum     = 1;

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "fill")) {
            f->fill = arg(o, 0);
        }
        else if (named(o, "body")) {
            f->body_open  = arg(o, 0);
            f->body_close = arg(o, 1);
        }
        else if (named(o, "pad")) {
            core_string_t policy = arg(o, 0);
            if (string_is(policy, "required")) {
                f->pad = SCAN_LEX_FENCE_PAD_REQUIRED;
            }
            else if (string_is(policy, "optional")) {
                f->pad = SCAN_LEX_FENCE_PAD_OPTIONAL;
            }
            else if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-fence: pad() expects required or optional");
            }
            if (core_list_len(o->args) == 2) {
                f->pad_chars = arg(o, 1);
            }
            else if (core_list_len(o->args) != 1 && !b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-fence: pad() expects a policy and optional characters");
            }
            if (core_list_len(o->args) == 2 && !f->pad_chars.u8_bytes
                && !b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-fence: pad characters cannot be empty");
            }
        }
        else if (named(o, "minimum")) {
            core_string_t value = arg(o, 0);
            uint32_t      n     = 0;
            bool          valid = value.u8_bytes > 0;

            for (size_t k = 0; k < value.u8_bytes; k++) {
                if (value.data[k] < '0' || value.data[k] > '9') {
                    oops(b, "fence", o->name);
                    valid = false;
                    break;
                }

                uint32_t digit = (uint32_t)(value.data[k] - '0');

                if (n > (UINT32_MAX - digit) / 10u) {
                    oops(b, "fence", o->name);
                    valid = false;
                    break;
                }

                n = n * 10u + digit;
            }

            if (valid) {
                f->minimum = n;
            }
        }
        else if (named(o, "label")) {
            f->label = true;
        }
        else if (named(o, "as")) {
            f->tok = arg(o, 0);
        }
        else {
            oops(b, "fence", o->name);
        }
    }

    if (!f->outer_open.data) {
        return;
    }

    if (!f->body_open.data) {
        f->body_open = CORE_STRING_STATIC("");
    }
    if (!f->body_close.data) {
        f->body_close = CORE_STRING_STATIC("");
    }

    if (!f->outer_close.data || !f->fill.data || !f->tok.data) {
        oops(b, "fence", CORE_STRING_STATIC("required option"));
        return;
    }

    core_list_push(b->fences, item);
}

static void
build_directive(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_directive_t item = {0};
    scan_lex_directive_t *d   = &item;

    d->open = positional(&a->lex_opts, 0);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "eol")) {
            d->tok_eol = arg(o, 0);
        }
        else if (named(o, "as")) {
            d->tok_other = arg(o, 0);
        }
        else {
            oops(b, "directive", o->name);
        }
    }

    if (!d->open.data) {
        return;
    }

    core_list_push(b->directives, item);
}

static void
build_literal_hook(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_literal_hook_t item = {
        .token = positional(&a->lex_opts, 0),
    };
    core_list_t(core_string_t) outputs = core_list_new(core_string_t);
    core_list_t(core_string_t) types   = core_list_new(core_string_t);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }
        if (named(o, "hook") && core_list_len(o->args) == 1) {
            item.hook = arg(o, 0);
        }
        else if (named(o, "modifier") && core_list_len(o->args) == 1) {
            item.modifier = arg(o, 0);
        }
        else if (named(o, "outputs") && core_list_len(o->args)) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                core_list_push(outputs, o->args.data[j]);
            }
        }
        else if (named(o, "types") && core_list_len(o->args)) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                core_list_push(types, o->args.data[j]);
            }
        }
        else {
            oops(b, "literal-hook", o->name);
        }
    }

    if ((!item.token.data || !item.hook.data) && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-literal-hook: requires a token and hook()");
    }
    item.outputs = own_string_array(b, &outputs);
    item.types   = own_string_array(b, &types);
    core_list_free(outputs);
    core_list_free(types);
    core_list_push(b->literal_hooks, item);
}

static void
build_recovery(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_recovery_t item = {
        .token = positional(&a->lex_opts, 0),
    };
    core_list_t(core_string_t) synchronize = core_list_new(core_string_t);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }
        if (named(o, "as") && core_list_len(o->args) == 1) {
            item.replacement = arg(o, 0);
        }
        else if (named(o, "sync") && core_list_len(o->args)) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                if (!o->args.data[j].u8_bytes && !b->err[0]) {
                    snprintf(b->err, sizeof(b->err),
                             "@lex-recover: synchronization delimiters "
                             "cannot be empty");
                    break;
                }
                core_list_push(synchronize, o->args.data[j]);
            }
        }
        else if (named(o, "consume") && !core_list_len(o->args)) {
            item.consume = true;
        }
        else {
            oops(b, "recover", o->name);
        }
    }

    if ((!item.token.data || !item.replacement.data) && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-recover: requires an input token and as()");
    }
    item.synchronize = own_string_array(b, &synchronize);
    core_list_free(synchronize);
    core_list_push(b->recoveries, item);
}

static void
build_identifier_profile(lex_build_t *b, const scan_lex_decl_t *a)
{
    scan_lex_identifier_profile_t profile = {
        .name = positional(&a->lex_opts, 0),
        .emoji = SCAN_LEX_EMOJI_REJECT,
    };
    core_list_t(core_string_t) scripts = core_list_new(core_string_t);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];
        if (!o->name.data) {
            continue;
        }
        if (named(o, "unicode") && core_list_len(o->args) == 1) {
            profile.unicode_version = arg(o, 0);
        }
        else if (named(o, "normalize") && core_list_len(o->args) == 1) {
            core_string_t value = arg(o, 0);
            if (string_is(value, "none")) {
                profile.normalization = SCAN_LEX_NORMALIZE_NONE;
            }
            else if (string_is(value, "nfc")) {
                profile.normalization = SCAN_LEX_NORMALIZE_NFC;
            }
            else if (string_is(value, "nfkc")) {
                profile.normalization = SCAN_LEX_NORMALIZE_NFKC;
            }
            else {
                oops(b, "ident-profile", value);
            }
        }
        else if (named(o, "scripts") && core_list_len(o->args)) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                core_list_push(scripts, o->args.data[j]);
            }
        }
        else if (named(o, "mixed_scripts")
                 && core_list_len(o->args) == 1) {
            core_string_t value = arg(o, 0);
            if (string_is(value, "diagnose")) {
                profile.diagnose_mixed_scripts = true;
            }
            else if (!string_is(value, "allow")) {
                oops(b, "ident-profile", value);
            }
        }
        else if (named(o, "confusables")
                 && core_list_len(o->args) == 1) {
            core_string_t value = arg(o, 0);
            if (string_is(value, "diagnose")) {
                profile.diagnose_confusables = true;
            }
            else if (!string_is(value, "allow")) {
                oops(b, "ident-profile", value);
            }
        }
        else if (named(o, "emoji") && core_list_len(o->args) == 1) {
            core_string_t value = arg(o, 0);
            if (string_is(value, "allow")) {
                profile.emoji = SCAN_LEX_EMOJI_ALLOW;
            }
            else if (string_is(value, "diagnose")) {
                profile.emoji = SCAN_LEX_EMOJI_DIAGNOSE;
            }
            else if (!string_is(value, "reject")) {
                oops(b, "ident-profile", value);
            }
        }
        else {
            oops(b, "ident-profile", o->name);
        }
    }

    if ((!profile.name.data || !profile.name.u8_bytes) && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-ident-profile: requires a name");
    }
    if (string_map_get(&b->identifier_profile_index, profile.name)
        && !b->err[0]) {
        snprintf(b->err, sizeof(b->err),
                 "@lex-ident-profile: duplicate profile '%.*s'",
                 (int)profile.name.u8_bytes, profile.name.data);
    }
    profile.scripts = own_string_array(b, &scripts);
    core_list_free(scripts);
    core_list_push(b->identifier_profiles, profile);
    string_map_put_new(&b->identifier_profile_index, profile.name,
                       core_list_len(b->identifier_profiles));
}

static scan_lex_identifier_profile_t *
identifier_profile(lex_build_t *b, core_string_t name)
{
    uintptr_t index = string_map_get(&b->identifier_profile_index, name);
    return index ? &b->identifier_profiles.data[index - 1] : nullptr;
}

static void
build_ident(lex_build_t *b, const scan_lex_decl_t *a)
{
    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "as")) {
            b->spec.tok_ident = arg(o, 0);
        }
        else if (named(o, "start")) {
            b->spec.ident_start_extra = arg(o, 0);
        }
        else if (named(o, "extra")) {
            b->spec.ident_extra = arg(o, 0);
        }
        else if (named(o, "upper")) {
            b->spec.tok_ident_upper = arg(o, 0);
        }
        else if (named(o, "profile") && core_list_len(o->args) == 1) {
            scan_lex_identifier_profile_t *profile
                = identifier_profile(b, arg(o, 0));
            if (profile) {
                b->spec.identifier_profile = *profile;
            }
            else if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-ident: unknown profile '%.*s'",
                         (int)arg(o, 0).u8_bytes, arg(o, 0).data);
            }
        }
        else {
            oops(b, "ident", o->name);
        }
    }
}

static void
build_fallback(lex_build_t *b, const scan_lex_decl_t *a)
{
    core_string_t mode = positional(&a->lex_opts, 0);

    if (core_string_eq(mode, CORE_STRING_STATIC("codepoint"))) {
        b->spec.fallback = SCAN_LEX_FALLBACK_CODEPOINT;
    }
    else if (core_string_eq(mode, CORE_STRING_STATIC("reject"))) {
        b->spec.fallback = SCAN_LEX_FALLBACK_REJECT;
    }
    else {
        oops(b, "fallback", mode);
    }
}

static void
build_whitespace(lex_build_t *b, const scan_lex_decl_t *a)
{
    core_string_t mode = positional(&a->lex_opts, 0);

    if (core_string_eq(mode, CORE_STRING_STATIC("skip"))) {
        b->spec.whitespace = SCAN_LEX_WHITESPACE_SKIP;
    }
    else if (core_string_eq(mode, CORE_STRING_STATIC("codepoint"))) {
        b->spec.whitespace = SCAN_LEX_WHITESPACE_CODEPOINT;
    }
    else {
        oops(b, "whitespace", mode);
    }
}

static lex_operator_set_t *
operator_set(lex_build_t *b, core_string_t name)
{
    uintptr_t index = string_map_get(&b->operator_set_index, name);
    return index ? &b->operator_sets.data[index - 1] : nullptr;
}

static void
operator_add(lex_build_t *b, core_string_t op, core_string_t origin)
{
    if (!op.data || !op.u8_bytes) {
        return;
    }

    uintptr_t state = string_map_get(&b->operator_state, op);

    if (!state) {
        string_map_put_new(&b->operator_state, op, OPERATOR_ACTIVE);
        core_list_push(b->operators, op);
        core_list_push(b->operator_origins, origin);
        string_map_put_new(&b->operator_index, op,
                           core_list_len(b->operators));
    }
    else if (state == OPERATOR_REMOVED) {
        core_dict_put(&b->operator_state, &op, OPERATOR_ACTIVE);
        size_t index = string_map_get(&b->operator_index, op) - 1;
        b->operator_origins.data[index] = origin;
    }
}

static void
operator_remove(lex_build_t *b, core_string_t op, core_string_t origin)
{
    if (!op.data || !op.u8_bytes) {
        return;
    }

    if (!string_map_get(&b->operator_state, op)) {
        string_map_put_new(&b->operator_state, op, OPERATOR_REMOVED);
        core_list_push(b->operators, op);
        core_list_push(b->operator_origins, origin);
        string_map_put_new(&b->operator_index, op,
                           core_list_len(b->operators));
    }
    else {
        core_dict_put(&b->operator_state, &op, OPERATOR_REMOVED);
    }
}

static void
build_operator_set(lex_build_t *b, const scan_lex_decl_t *a)
{
    core_string_t name = positional(&a->lex_opts, 0);

    if (!name.data || !name.u8_bytes) {
        oops(b, "operator-set", name);

        return;
    }

    if (operator_set(b, name)) {
        if (!b->err[0]) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-operator-set: duplicate set '%.*s'",
                     (int)name.u8_bytes, name.data);
        }

        return;
    }

    lex_operator_set_t set = {
        .name = name,
        .origin = a->source_origin,
    };
    size_t             positional_ix = 0;
    core_dict_t        seen;
    core_dict_init(&seen, string_key_hash, string_key_eq);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            core_string_t value = arg(o, 0);
            uintptr_t state = string_map_get(&seen, value);
            if (positional_ix++ && value.data && state != OPERATOR_ACTIVE) {
                core_list_push(set.operators, value);
                core_list_push(set.origins, a->source_origin);
                if (state) {
                    core_dict_put(&seen, &value, OPERATOR_ACTIVE);
                }
                else {
                    string_map_put_new(&seen, value, OPERATOR_ACTIVE);
                }
            }
        }
        else if (named(o, "use")) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                lex_operator_set_t *base = operator_set(b, o->args.data[j]);
                for (size_t k = 0; base && k < base->operators.len; k++) {
                    core_string_t value = base->operators.data[k];
                    uintptr_t state = string_map_get(&seen, value);
                    if (state != OPERATOR_ACTIVE) {
                        core_list_push(set.operators, value);
                        core_list_push(set.origins, base->origins.data[k]);
                        if (state) {
                            core_dict_put(&seen, &value, OPERATOR_ACTIVE);
                        }
                        else {
                            string_map_put_new(&seen, value, OPERATOR_ACTIVE);
                        }
                    }
                }
            }
        }
        else if (named(o, "add")) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                core_string_t value = o->args.data[j];
                uintptr_t state = string_map_get(&seen, value);
                if (state != OPERATOR_ACTIVE) {
                    core_list_push(set.operators, value);
                    core_list_push(set.origins, a->source_origin);
                    if (state) {
                        core_dict_put(&seen, &value, OPERATOR_ACTIVE);
                    }
                    else {
                        string_map_put_new(&seen, value, OPERATOR_ACTIVE);
                    }
                }
            }
        }
        else if (named(o, "remove")) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                core_string_t value = o->args.data[j];
                if (string_map_get(&seen, value) != OPERATOR_ACTIVE) {
                    if (!b->err[0]) {
                        snprintf(b->err, sizeof(b->err),
                                 "@lex-operator-set: package '%.*s' cannot "
                                 "remove absent operator '%.*s'",
                                 (int)name.u8_bytes, name.data,
                                 (int)value.u8_bytes, value.data);
                        append_error_origin(b, a->source_origin,
                                            "declared in");
                    }
                    continue;
                }
                for (size_t k = 0; k < set.operators.len; k++) {
                    if (core_string_eq(set.operators.data[k], value)) {
                        core_list_delete(set.operators, k);
                        core_list_delete(set.origins, k);
                        break;
                    }
                }
                core_dict_put(&seen, &value, OPERATOR_REMOVED);
            }
        }
        else {
            oops(b, "operator-set", o->name);
        }
    }

    string_map_free(&seen);
    core_list_push(b->operator_sets, set);
    string_map_put_new(&b->operator_set_index, name,
                       core_list_len(b->operator_sets));
}

static void
build_operators(lex_build_t *b, const scan_lex_decl_t *a)
{
    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (named(o, "use")) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                lex_operator_set_t *set = operator_set(b, o->args.data[j]);

                if (!set) {
                    if (!b->err[0]) {
                        snprintf(b->err, sizeof(b->err),
                                 "@lex-operators: unknown set '%.*s'",
                                 (int)o->args.data[j].u8_bytes,
                                 o->args.data[j].data);
                    }

                    continue;
                }

                for (size_t k = 0; k < core_list_len(set->operators); k++) {
                    operator_add(b, set->operators.data[k],
                                 set->origins.data[k]);
                }
            }
        }
        else if (named(o, "add")) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                operator_add(b, o->args.data[j], a->source_origin);
            }
        }
        else if (named(o, "remove")) {
            for (size_t j = 0; j < core_list_len(o->args); j++) {
                operator_remove(b, o->args.data[j], a->source_origin);
            }
        }
        else {
            oops(b, "operators", o->name);
        }
    }
}

static void
build_soft_keywords(lex_build_t *b, parse_grammar_t *g,
                    const scan_lex_decl_t *a)
{
    for (size_t i = 0; i < a->lex_opts.len; i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];

        if (o->name.data || o->args.len != 1) {
            oops(b, "soft-keywords", o->name);
            continue;
        }

        core_string_t spelling = o->args.data[0];
        int64_t terminal = parse_lookup_terminal(g, spelling);

        if (!terminal || terminal < PARSE_TOK_START_ID) {
            if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-soft-keywords: '%.*s' is not a named grammar "
                         "terminal",
                         (int)spelling.u8_bytes, spelling.data);
                append_error_origin(b, a->source_origin, "declared in");
            }
            continue;
        }
        if (string_map_get(&b->soft_keyword_index, spelling)) {
            if (!b->err[0]) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-soft-keywords: duplicate spelling '%.*s'",
                         (int)spelling.u8_bytes, spelling.data);
                append_error_origin(b, a->source_origin, "declared in");
            }
            continue;
        }

        string_map_put_new(&b->soft_keyword_index, spelling, 1);
        core_list_push(b->soft_keywords, spelling);
    }
}

static bool
grammar_uses_operator(parse_grammar_t *g, core_string_t op)
{
    int64_t named_id = parse_lookup_terminal(g, op);

    for (size_t i = 0; i < core_list_len(g->rules); i++) {
        const parse_rule_t *rule = &g->rules.data[i];

        for (size_t j = 0; j < core_list_len(rule->contents); j++) {
            const parse_match_t *m = &rule->contents.data[j];

            if (m->kind == PARSE_MATCH_TERMINAL && named_id
                && m->terminal_id == named_id) {
                return true;
            }

            if (m->kind == PARSE_MATCH_TERMINAL && op.u8_bytes == 1
                && m->terminal_id == (unsigned char)op.data[0]) {
                return true;
            }
        }
    }

    return false;
}

static void
validate_regex_contexts(lex_build_t *b, parse_grammar_t *g)
{
    for (size_t i = 0; !b->err[0] && i < core_list_len(b->regexes); i++) {
        const scan_lex_regex_t *r = &b->regexes.data[i];

        for (const core_string_t *name = r->not_after;
             name && name->data; name++) {
            if (parse_terminal_id(g, *name)) {
                continue;
            }

            snprintf(b->err, sizeof(b->err),
                     "@lex-regex: not_after() names unknown token '%.*s'",
                     (int)name->u8_bytes, name->data);
            return;
        }
    }
}

static core_dict_t *
package_declarations(lex_build_t *b, scan_lex_kind_t kind)
{
    switch (kind) {
    case SCAN_LEX_OPERATOR_SET:
        return &b->operator_set_declarations;
    case SCAN_LEX_NUMBER_SUFFIXES:
        return &b->number_suffix_set_declarations;
    case SCAN_LEX_STRING_ESCAPES:
        return &b->string_escape_set_declarations;
    default:
        return nullptr;
    }
}

static bool
package_built(lex_build_t *b, scan_lex_kind_t kind, core_string_t name)
{
    switch (kind) {
    case SCAN_LEX_OPERATOR_SET:
        return operator_set(b, name) != nullptr;
    case SCAN_LEX_NUMBER_SUFFIXES:
        return number_suffix_set(b, name) != nullptr;
    case SCAN_LEX_STRING_ESCAPES:
        return string_escape_set(b, name) != nullptr;
    default:
        return true;
    }
}

static bool
package_dependencies_ready(lex_build_t *b, const scan_lex_decl_t *a)
{
    for (size_t i = 0; i < a->lex_opts.len; i++) {
        const scan_lex_opt_t *o = &a->lex_opts.data[i];
        if (!named(o, "use")) {
            continue;
        }
        for (size_t j = 0; j < o->args.len; j++) {
            if (!package_built(b, a->kind, o->args.data[j])) {
                return false;
            }
        }
    }
    return true;
}

static void
build_package(lex_build_t *b, const scan_lex_decl_t *a)
{
    switch (a->kind) {
    case SCAN_LEX_OPERATOR_SET: build_operator_set(b, a); break;
    case SCAN_LEX_NUMBER_SUFFIXES:
        build_number_suffixes(b, a);
        break;
    case SCAN_LEX_STRING_ESCAPES: build_string_escapes(b, a); break;
    default: break;
    }
}

static void
resolve_packages(lex_build_t *b, core_list_t(scan_lex_decl_t) *decls)
{
    size_t n = decls->len;
    size_t unresolved = 0;

    for (size_t i = 0; i < n; i++) {
        scan_lex_decl_t *a = &decls->data[i];
        core_dict_t *declarations = a ? package_declarations(b, a->kind)
                                      : nullptr;
        if (!declarations) {
            continue;
        }
        core_string_t name = positional(&a->lex_opts, 0);
        if (!name.data || !name.u8_bytes) {
            oops(b, scan_lex_kind_name(a->kind) + 4,
                 CORE_STRING_STATIC("name"));
            continue;
        }
        if (string_map_get(declarations, name)) {
            if (!b->err[0]) {
                size_t prior_index = string_map_get(declarations, name) - 1;
                scan_lex_decl_t *prior
                    = &decls->data[prior_index];

                snprintf(b->err, sizeof(b->err),
                         "@%s: duplicate set/package '%.*s'",
                         scan_lex_kind_name(a->kind),
                         (int)name.u8_bytes, name.data);
                if (prior) {
                    append_error_origin(b, prior->source_origin,
                                        "first declared in");
                }
                append_error_origin(b, a->source_origin,
                                    "declared again in");
            }
            continue;
        }
        string_map_put_new(declarations, name, i + 1);
        unresolved++;
    }

    while (!b->err[0] && unresolved) {
        size_t before = unresolved;
        for (size_t i = 0; i < n; i++) {
            scan_lex_decl_t *a = &decls->data[i];
            if (!a || !package_declarations(b, a->kind)) {
                continue;
            }
            core_string_t name = positional(&a->lex_opts, 0);
            if (package_built(b, a->kind, name)
                || !package_dependencies_ready(b, a)) {
                continue;
            }
            build_package(b, a);
            unresolved--;
        }
        if (unresolved == before) {
            break;
        }
    }

    if (!b->err[0] && unresolved) {
        for (size_t i = 0; i < n; i++) {
            scan_lex_decl_t *a = &decls->data[i];
            core_dict_t *decls = a ? package_declarations(b, a->kind)
                                   : nullptr;
            if (!decls) {
                continue;
            }
            core_string_t name = positional(&a->lex_opts, 0);
            if (package_built(b, a->kind, name)) {
                continue;
            }
            for (size_t j = 0; j < a->lex_opts.len; j++) {
                const scan_lex_opt_t *o = &a->lex_opts.data[j];
                if (!named(o, "use")) {
                    continue;
                }
                for (size_t k = 0; k < o->args.len; k++) {
                    if (!string_map_get(decls, o->args.data[k])) {
                        snprintf(b->err, sizeof(b->err),
                                 "@%s: package '%.*s' uses unknown package '%.*s'",
                                 scan_lex_kind_name(a->kind),
                                 (int)name.u8_bytes, name.data,
                                 (int)o->args.data[k].u8_bytes,
                                 o->args.data[k].data);
                        append_error_origin(b, a->source_origin,
                                            "declared in");
                        return;
                    }
                }
            }
            snprintf(b->err, sizeof(b->err),
                     "@%s: package dependency cycle reaches '%.*s'",
                     scan_lex_kind_name(a->kind),
                     (int)name.u8_bytes, name.data);
            append_error_origin(b, a->source_origin, "declared in");
            return;
        }
    }
}

const scan_lex_spec_t *
scan_lex_spec_from_decls(parse_grammar_t *g,
                         core_list_t(scan_lex_decl_t) *decls)
{
    size_t n = decls ? decls->len : 0;

    lex_build_err[0] = '\0';
    scan_lex_compile_error_reset();

    if (!n) {
        return nullptr;
    }

    lex_build_t *b = core_alloc(lex_build_t);

    core_dict_init(&b->operator_set_index, string_key_hash, string_key_eq);
    core_dict_init(&b->operator_set_declarations,
                   string_key_hash, string_key_eq);
    core_dict_init(&b->operator_state, string_key_hash, string_key_eq);
    core_dict_init(&b->operator_index, string_key_hash, string_key_eq);
    core_dict_init(&b->soft_keyword_index, string_key_hash, string_key_eq);
    core_dict_init(&b->string_openers, string_key_hash, string_key_eq);
    core_dict_init(&b->number_prefixes, string_key_hash, string_key_eq);
    core_dict_init(&b->number_suffix_set_index,
                   string_key_hash, string_key_eq);
    core_dict_init(&b->number_suffix_set_declarations,
                   string_key_hash, string_key_eq);
    core_dict_init(&b->string_escape_set_index,
                   string_key_hash, string_key_eq);
    core_dict_init(&b->string_escape_set_declarations,
                   string_key_hash, string_key_eq);
    core_dict_init(&b->heredoc_header_index,
                   string_key_hash, string_key_eq);
    core_dict_init(&b->identifier_profile_index,
                   string_key_hash, string_key_eq);

    b->spec.name = scan_grammar_tokenizer(g);

    resolve_packages(b, decls);

    for (size_t i = 0; i < n; i++) {
        scan_lex_decl_t *a = &decls->data[i];

        if (a && a->kind == SCAN_LEX_HEREDOC_HEADER) {
            build_heredoc_header(b, a);
        }
        else if (a && a->kind == SCAN_LEX_IDENT_PROFILE) {
            build_identifier_profile(b, a);
        }
    }

    for (size_t i = 0; i < n; i++) {
        scan_lex_decl_t *a = &decls->data[i];

        if (!a) {
            continue;
        }

        size_t comments_before = b->comments.len;
        size_t strings_before = b->strings.len;
        size_t sigils_before = b->sigils.len;
        size_t regexes_before = b->regexes.len;
        size_t heredocs_before = b->heredocs.len;
        size_t embeds_before = b->embeds.len;
        size_t fences_before = b->fences.len;
        size_t directives_before = b->directives.len;
        size_t hooks_before = b->literal_hooks.len;
        size_t recoveries_before = b->recoveries.len;
        size_t radixes_before = b->radixes.len;
        size_t numbers_before = b->number_forms.len;
        size_t soft_before = b->soft_keywords.len;

        switch (a->kind) {
        case SCAN_LEX_COMMENT: build_comment(b, a); break;
        case SCAN_LEX_REGION: build_region(b, a); break;
        case SCAN_LEX_STRING:  build_string(b, a);  break;
        case SCAN_LEX_STRING_FAMILY:
            build_string_family(b, a);
            break;
        case SCAN_LEX_STRING_ESCAPES: break;
        case SCAN_LEX_NUMBER:  build_number(b, a);  break;
        case SCAN_LEX_NUMBER_SUFFIXES: break;
        case SCAN_LEX_LAYOUT:  build_layout(b, a);  break;
        case SCAN_LEX_SIGIL:   build_sigil(b, a);   break;
        case SCAN_LEX_REGEX:   build_regex(b, a);   break;
        case SCAN_LEX_HEREDOC_HEADER: break;
        case SCAN_LEX_HEREDOC: build_heredoc(b, a); break;
        case SCAN_LEX_EMBED:   build_embed(b, a);   break;
        case SCAN_LEX_FENCE:   build_fence(b, a);   break;
        case SCAN_LEX_OPERATOR_SET: break;
        case SCAN_LEX_OPERATORS: build_operators(b, a); break;
        case SCAN_LEX_WHITESPACE: build_whitespace(b, a); break;
        case SCAN_LEX_FALLBACK: build_fallback(b, a); break;
        case SCAN_LEX_DIRECTIVE: build_directive(b, a); break;
        case SCAN_LEX_LITERAL_HOOK: build_literal_hook(b, a); break;
        case SCAN_LEX_RECOVER: build_recovery(b, a); break;
        case SCAN_LEX_SOFT_KEYWORDS:
            build_soft_keywords(b, g, a);
            break;
        case SCAN_LEX_IDENT:   build_ident(b, a);   break;
        case SCAN_LEX_IDENT_PROFILE: break;
        default: break;
        }

        for (size_t j = comments_before; j < b->comments.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_COMMENT,
                               b->comments.data[j].opener, a->source_origin);
        }
        for (size_t j = strings_before; j < b->strings.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_STRING,
                               b->strings.data[j].opener, a->source_origin);
        }
        for (size_t j = sigils_before; j < b->sigils.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_SIGIL,
                               b->sigils.data[j].prefix, a->source_origin);
        }
        for (size_t j = regexes_before; j < b->regexes.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_REGEX,
                               b->regexes.data[j].opener, a->source_origin);
        }
        for (size_t j = heredocs_before; j < b->heredocs.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_HEREDOC,
                               b->heredocs.data[j].opener, a->source_origin);
        }
        for (size_t j = embeds_before; j < b->embeds.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_EMBED,
                               b->embeds.data[j].open, a->source_origin);
        }
        for (size_t j = fences_before; j < b->fences.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_FENCE,
                               b->fences.data[j].outer_open,
                               a->source_origin);
        }
        for (size_t j = directives_before; j < b->directives.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_DIRECTIVE,
                               b->directives.data[j].open, a->source_origin);
        }
        for (size_t j = hooks_before; j < b->literal_hooks.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_LITERAL_HOOK,
                               b->literal_hooks.data[j].token,
                               a->source_origin);
        }
        for (size_t j = recoveries_before; j < b->recoveries.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_RECOVERY,
                               b->recoveries.data[j].token,
                               a->source_origin);
        }
        for (size_t j = radixes_before; j < b->radixes.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_NUMBER,
                               b->radixes.data[j].prefix, a->source_origin);
        }
        for (size_t j = numbers_before; j < b->number_forms.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_NUMBER,
                               b->number_forms.data[j].prefix,
                               a->source_origin);
        }
        for (size_t j = soft_before; j < b->soft_keywords.len; j++) {
            record_declaration(b, SCAN_LEX_DECL_SOFT_KEYWORD,
                               b->soft_keywords.data[j], a->source_origin);
        }
    }

    validate_regex_contexts(b, g);

    size_t active_operators = 0;
    for (size_t i = 0; !b->err[0] && i < core_list_len(b->operators); i++) {
        core_string_t op = b->operators.data[i];

        if (string_map_get(&b->operator_state, op) == OPERATOR_ACTIVE) {
            if (string_map_get(&b->soft_keyword_index, op)) {
                snprintf(b->err, sizeof(b->err),
                         "@lex-soft-keywords: '%.*s' is also an active "
                         "operator",
                         (int)op.u8_bytes, op.data);
                break;
            }
            b->operators.data[active_operators]
                = b->operators.data[i];
            b->operator_origins.data[active_operators]
                = b->operator_origins.data[i];
            active_operators++;
            continue;
        }

        if (grammar_uses_operator(g, op)) {
            snprintf(b->err, sizeof(b->err),
                     "@lex-operators: cannot remove '%.*s'; the grammar uses "
                     "it as a terminal",
                     (int)op.u8_bytes, op.data);
        }
    }
    b->operators.len = active_operators;
    b->operator_origins.len = active_operators;

    if (b->err[0]) {
        snprintf(lex_build_err, sizeof(lex_build_err), "%s", b->err);
        lex_build_free(b);

        return nullptr;
    }

    for (size_t i = 0; i < core_list_len(b->operators); i++) {
        parse_register_terminal(g, b->operators.data[i]);
        record_declaration(b, SCAN_LEX_DECL_OPERATOR,
                           b->operators.data[i], b->operator_origins.data[i]);
    }

    if (core_list_len(b->comments)) {
        core_list_push(b->comments, ((scan_lex_comment_t){0}));
        b->spec.comments = b->comments.data;
    }
    if (core_list_len(b->regions)) {
        core_list_push(b->regions, ((scan_lex_region_t){0}));
        b->spec.regions = b->regions.data;
    }
    if (core_list_len(b->strings)) {
        core_list_push(b->strings, ((scan_lex_string_t){0}));
        b->spec.strings = b->strings.data;
    }
    if (core_list_len(b->sigils)) {
        core_list_push(b->sigils, ((scan_lex_sigil_t){0}));
        b->spec.sigils = b->sigils.data;
    }
    if (core_list_len(b->regexes)) {
        core_list_push(b->regexes, ((scan_lex_regex_t){0}));
        b->spec.regexes = b->regexes.data;
    }
    if (core_list_len(b->heredocs)) {
        core_list_push(b->heredocs, ((scan_lex_heredoc_t){0}));
        b->spec.heredocs = b->heredocs.data;
    }
    if (core_list_len(b->heredoc_headers)) {
        core_list_push(b->heredoc_headers,
                       ((scan_lex_heredoc_header_t){0}));
        b->spec.heredoc_headers = b->heredoc_headers.data;
    }
    if (core_list_len(b->embeds)) {
        core_list_push(b->embeds, ((scan_lex_embed_t){0}));
        b->spec.embeds = b->embeds.data;
    }
    if (core_list_len(b->fences)) {
        core_list_push(b->fences, ((scan_lex_fence_t){0}));
        b->spec.fences = b->fences.data;
    }
    if (core_list_len(b->operators)) {
        core_list_push(b->operators, ((core_string_t){0}));
        b->spec.operators = b->operators.data;
    }
    if (core_list_len(b->soft_keywords)) {
        core_list_push(b->soft_keywords, ((core_string_t){0}));
        b->spec.soft_keywords = b->soft_keywords.data;
    }
    if (core_list_len(b->directives)) {
        core_list_push(b->directives, ((scan_lex_directive_t){0}));
        b->spec.directives = b->directives.data;
    }
    if (core_list_len(b->literal_hooks)) {
        core_list_push(b->literal_hooks, ((scan_lex_literal_hook_t){0}));
        b->spec.literal_hooks = b->literal_hooks.data;
    }
    if (core_list_len(b->recoveries)) {
        core_list_push(b->recoveries, ((scan_lex_recovery_t){0}));
        b->spec.recoveries = b->recoveries.data;
    }
    if (core_list_len(b->radixes)) {
        core_list_push(b->radixes, ((scan_lex_radix_t){0}));
        b->spec.number.radixes = b->radixes.data;
    }
    if (core_list_len(b->suffixes)) {
        core_list_push(b->suffixes, ((scan_lex_suffix_t){0}));
        b->spec.number.suffixes = b->suffixes.data;
    }
    if (core_list_len(b->number_forms)) {
        core_list_push(b->number_forms, ((scan_lex_number_form_t){0}));
        b->spec.number_forms = b->number_forms.data;
    }
    if (core_list_len(b->declarations)) {
        core_list_push(b->declarations, ((scan_lex_declaration_t){0}));
        b->spec.declarations = b->declarations.data;
    }

    b->decls = *decls;
    *decls = (core_list_t(scan_lex_decl_t)){0};
    return &b->spec;
}

void
scan_lex_spec_free(const scan_lex_spec_t *spec)
{
    if (!spec) {
        return;
    }

    lex_build_free((lex_build_t *)(uintptr_t)(const void *)spec);
}
