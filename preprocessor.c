#define _GNU_SOURCE

#include "preprocessor.h"

#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/diagnostic.h"
#include "lib/list.h"
#include "lib/tree.h"
#include "parse/rewrite.h"
#include "parse/token.h"
#include "parse/tree_util.h"
#include "unicode/encoding.h"

typedef enum {
    PP_INVALID,
    PP_BOOL,
    PP_INT,
    PP_FLOAT,
    PP_STRING,
    PP_ANY,
} pp_kind_t;

typedef struct {
    pp_kind_t kind;
    union {
        bool boolean;
        __int128 integer;
        double real;
        core_string_t string;
    } as;
} pp_value_t;

typedef struct {
    core_string_t name;
    core_string_t module;
    parse_tree_t *declaration;
    pp_value_t value;
    bool eligible;
} pp_binding_t;
core_list_decl(pp_binding_t);

typedef enum {
    PP_MODULE_VISITING,
    PP_MODULE_DONE,
} pp_module_visit_t;

typedef struct {
    core_translation_unit_t *unit;
    parse_tree_t *tree;
    pp_module_visit_t visit;
    bool cyclic;
    bool succeeded;
} pp_module_t;
core_list_decl(pp_module_t);
core_list_decl(size_t);

typedef struct {
    parse_tree_t *parent;
    parse_tree_t *statement;
    int32_t depth;
    int32_t child_index;
} pp_deferred_stmt_t;
core_list_decl(pp_deferred_stmt_t);

typedef struct {
    parse_grammar_t *grammar;
    core_translation_unit_t *unit;
    core_list_t(pp_binding_t) bindings;
    core_list_t(core_string_t) owned_strings;
    core_list_t(pp_module_t) modules;
    core_list_t(size_t) module_stack;
    const n00bb_preprocessor_options_t *options;
    bool cycle_warning_emitted;
    bool type_only;
    struct tm utc_time;
    bool time_initialized;
    bool failed;
} pp_context_t;

struct n00bb_preprocessor_t {
    pp_context_t context;
};

static void pp_error(pp_context_t *ctx, parse_tree_t *at,
                     const char *message);
static void pp_warn_cycle_dependency(pp_context_t *ctx,
                                    parse_tree_t *at,
                                    core_string_t module);
bool n00bb_preprocessor_builtin_name(core_string_t name);
static bool validate_expr_forms(pp_context_t *ctx, parse_tree_t *node);
static bool evaluate_when_condition(pp_context_t *ctx,
                                    parse_tree_t *condition,
                                    bool *selected);
static core_string_t builtin_name(pp_context_t *ctx, parse_tree_t *node);
static bool process_module(pp_context_t *ctx, core_translation_unit_t *unit,
                           parse_tree_t *tree);

static core_string_t
own_string(pp_context_t *ctx, core_string_t value)
{
    if (value.data) core_list_push(ctx->owned_strings, value);
    return value;
}

static int
hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool
n00bb_decode_string_literal(core_string_t raw, core_buffer_t *buffer)
{
    if (!buffer || !raw.data || raw.u8_bytes < 2 || raw.data[0] != '"'
        || raw.data[raw.u8_bytes - 1] != '"') return false;
    for (size_t i = 1; i + 1 < raw.u8_bytes; i++) {
        unsigned char c = raw.data[i];
        if (c != '\\') {
            core_buffer_putc(buffer, (char)c);
            continue;
        }
        if (++i + 1 >= raw.u8_bytes) goto invalid;
        c = raw.data[i];
        switch (c) {
        case 'a': core_buffer_putc(buffer, '\a'); break;
        case 'b': core_buffer_putc(buffer, '\b'); break;
        case 'f': core_buffer_putc(buffer, '\f'); break;
        case 'n': core_buffer_putc(buffer, '\n'); break;
        case 'r': core_buffer_putc(buffer, '\r'); break;
        case 't': core_buffer_putc(buffer, '\t'); break;
        case 'v': core_buffer_putc(buffer, '\v'); break;
        case '0': core_buffer_putc(buffer, '\0'); break;
        case '\\': core_buffer_putc(buffer, '\\'); break;
        case '"': core_buffer_putc(buffer, '"'); break;
        case '\'': core_buffer_putc(buffer, '\''); break;
        case 'x':
        case 'u':
        case 'U': {
            int count = c == 'x' ? 2 : c == 'u' ? 4 : 8;
            uint32_t cp = 0;
            if (i + (size_t)count + 1 >= raw.u8_bytes) goto invalid;
            for (int k = 0; k < count; k++) {
                int digit = hex_digit((unsigned char)raw.data[++i]);
                if (digit < 0) goto invalid;
                cp = (cp << 4) | (uint32_t)digit;
            }
            char encoded[4];
            if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                goto invalid;
            uint32_t length = unicode_utf8_encode(cp, encoded);
            core_buffer_append(buffer, encoded, length);
            break;
        }
        default: goto invalid;
        }
    }
    return true;
invalid:
    return false;
}

static bool
decode_quoted_string(pp_context_t *ctx, core_string_t raw,
                     core_string_t *decoded)
{
    core_buffer_t *buffer = core_buffer_empty();
    if (!n00bb_decode_string_literal(raw, buffer)) {
        core_buffer_free(buffer);
        pp_error(ctx, nullptr, "invalid escape in preprocessor string literal");
        return false;
    }
    size_t length = buffer->byte_len;
    char *data = core_buffer_take(buffer);
    *decoded = own_string(ctx, core_string_borrow(data, length));
    return true;
}

static core_string_t
node_name(parse_grammar_t *g, parse_tree_t *node)
{
    return node && !core_tree_is_leaf(node)
        ? parse_tree_nt_name(g, node) : (core_string_t){0};
}

static bool
is_name(parse_grammar_t *g, parse_tree_t *node, const char *name)
{
    return core_string_eq(node_name(g, node), core_string_from_cstr(name));
}

static parse_tree_t *
child_named(parse_grammar_t *g, parse_tree_t *node, const char *name)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (is_name(g, child, name)) return child;
    }
    return nullptr;
}

static parse_tree_t *
first_node(parse_tree_t *node)
{
    if (!node) return nullptr;
    if (core_tree_is_leaf(node)) return node;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *found = first_node(core_tree_child(node, i));
        if (found) return found;
    }
    return nullptr;
}

static parse_tree_t *
last_node(parse_tree_t *node)
{
    if (!node) return nullptr;
    if (core_tree_is_leaf(node)) return node;
    for (size_t i = core_tree_num_children(node); i > 0; i--) {
        parse_tree_t *found = last_node(core_tree_child(node, i - 1));
        if (found) return found;
    }
    return nullptr;
}

static pp_kind_t
primitive_annotation_kind(pp_context_t *ctx, parse_tree_t *type)
{
    parse_tree_t *first = first_node(type);
    parse_tree_t *last = last_node(type);
    parse_token_info_t *a = parse_node_token(first);
    parse_token_info_t *b = parse_node_token(last);
    if (!a || !b || a->start_byte > b->end_byte) return PP_INVALID;
    const char *source = core_source_file_contents(&ctx->unit->source);
    size_t source_len = core_source_file_size(&ctx->unit->source);
    if (b->end_byte > source_len) return PP_INVALID;
    core_buffer_t *compact = core_buffer_empty();
    for (size_t i = a->start_byte; i < b->end_byte; i++)
        if (!isspace((unsigned char)source[i])) core_buffer_putc(compact, source[i]);
    core_string_t name = core_string_borrow(compact->data, compact->byte_len);
    pp_kind_t kind = PP_INVALID;
    if (core_string_eq(name, CORE_STRING_STATIC("bool"))) kind = PP_BOOL;
    else if (core_string_eq(name, CORE_STRING_STATIC("string"))) kind = PP_STRING;
    else if (core_string_eq(name, CORE_STRING_STATIC("float"))
             || core_string_eq(name, CORE_STRING_STATIC("f64"))) kind = PP_FLOAT;
    else if (core_string_eq(name, CORE_STRING_STATIC("int"))
             || core_string_eq(name, CORE_STRING_STATIC("uint")))
        kind = PP_INT;
    if (kind == PP_INVALID && name.u8_bytes >= 2
        && (name.data[0] == 'i' || name.data[0] == 'u')) {
        size_t digit = name.data[1] == ':' ? 2 : 1;
        bool valid = digit < name.u8_bytes;
        for (size_t i = digit; valid && i < name.u8_bytes; i++)
            valid = name.data[i] >= '0' && name.data[i] <= '9';
        if (valid) kind = PP_INT;
    }
    core_buffer_free(compact);
    return kind;
}

static core_string_t
leaf_text(pp_context_t *ctx, parse_tree_t *node)
{
    parse_token_info_t *token = parse_node_token(node);
    if (!token) return (core_string_t){0};
    if (core_option_is_set(token->value))
        return core_option_get(token->value);
    const char *source = core_source_file_contents(&ctx->unit->source);
    size_t size = core_source_file_size(&ctx->unit->source);
    if (token->start_byte > token->end_byte || token->end_byte > size)
        return (core_string_t){0};
    return core_string_from_raw(source + token->start_byte,
                                (int64_t)(token->end_byte - token->start_byte));
}

static core_string_t
first_leaf_text(pp_context_t *ctx, parse_tree_t *node)
{
    parse_tree_t *leaf = first_node(node);
    return leaf ? leaf_text(ctx, leaf) : (core_string_t){0};
}

static void
pp_error(pp_context_t *ctx, parse_tree_t *at, const char *message)
{
    parse_token_info_t *token = parse_tree_find_first_terminal(at);
    char text[320];
    if (token) {
        snprintf(text, sizeof(text), "%s at line %u, column %u", message,
                 token->line, token->column);
    }
    else snprintf(text, sizeof(text), "%s", message);
    core_diagnostics_report(core_translation_unit_diagnostics(ctx->unit),
                            ctx->unit, CORE_DIAG_ERROR,
                            CORE_STRING_STATIC("N00BB-PP0001"),
                            core_string_from_cstr(text));
    ctx->failed = true;
}

static bool
parse_integer(core_string_t text, __int128 *out)
{
    if (!text.data || !text.u8_bytes) return false;
    size_t i = 0;
    bool negative = false;
    if (text.data[i] == '+' || text.data[i] == '-') {
        negative = text.data[i++] == '-';
        if (i == text.u8_bytes) return false;
    }
    unsigned base = 10;
    if (i + 2 <= text.u8_bytes && text.data[i] == '0') {
        char prefix = text.data[i + 1];
        if (prefix == 'x' || prefix == 'X') base = 16, i += 2;
        else if (prefix == 'b' || prefix == 'B') base = 2, i += 2;
        else if (prefix == 'o' || prefix == 'O') base = 8, i += 2;
    }
    if (i == text.u8_bytes) return false;
    unsigned __int128 value = 0;
    unsigned __int128 limit = ((unsigned __int128)1 << 127)
        - (negative ? 0 : 1);
    bool digit_seen = false;
    for (; i < text.u8_bytes; i++) {
        unsigned char c = (unsigned char)text.data[i];
        if (c == '_') continue;
        unsigned digit = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : base;
        if (digit >= base) return false;
        digit_seen = true;
        if (value > (limit - digit) / base) return false;
        value = value * base + digit;
    }
    if (!digit_seen) return false;
    *out = negative ? -(__int128)(value - (value == ((unsigned __int128)1 << 127)))
                    : (__int128)value;
    if (negative && value == ((unsigned __int128)1 << 127))
        *out = -((__int128)(((unsigned __int128)1 << 127) - 1)) - 1;
    return true;
}

static bool
integer_syntax(core_string_t text)
{
    if (!text.data || !text.u8_bytes) return false;
    size_t i = 0;
    if (text.data[i] == '+' || text.data[i] == '-') i++;
    if (i == text.u8_bytes) return false;
    unsigned base = 10;
    if (i + 2 <= text.u8_bytes && text.data[i] == '0') {
        char prefix = text.data[i + 1];
        if (prefix == 'x' || prefix == 'X') base = 16, i += 2;
        else if (prefix == 'b' || prefix == 'B') base = 2, i += 2;
        else if (prefix == 'o' || prefix == 'O') base = 8, i += 2;
    }
    bool digit_seen = false;
    for (; i < text.u8_bytes; i++) {
        unsigned char c = (unsigned char)text.data[i];
        if (c == '_') continue;
        int digit = c >= '0' && c <= '9' ? c - '0' : hex_digit(c);
        if (digit < 0 || (unsigned)digit >= base) return false;
        digit_seen = true;
    }
    return digit_seen;
}

static __int128
int128_minimum(void)
{
    return -(__int128)(((unsigned __int128)1 << 127) - 1) - 1;
}

static bool
checked_left_shift(__int128 value, unsigned shift, __int128 *out)
{
    if (!shift) {
        *out = value;
        return true;
    }
    unsigned __int128 limit = (unsigned __int128)1 << 127;
    unsigned __int128 magnitude = value < 0
        ? (unsigned __int128)0 - (unsigned __int128)value
        : (unsigned __int128)value;
    if (value >= 0) limit--;
    if (magnitude > (limit >> shift)) return false;
    magnitude <<= shift;
    if (value < 0)
        *out = magnitude == ((unsigned __int128)1 << 127)
            ? int128_minimum() : -(__int128)magnitude;
    else *out = (__int128)magnitude;
    return true;
}

typedef enum {
    PP_LOOKUP_NONE,
    PP_LOOKUP_FOUND,
    PP_LOOKUP_INELIGIBLE,
    PP_LOOKUP_AMBIGUOUS,
} pp_lookup_t;

static bool
path_matches_module(core_string_t path, core_string_t module)
{
    if (core_string_eq(path, module)) return true;
    if (path.u8_bytes > module.u8_bytes) {
        size_t offset = path.u8_bytes - module.u8_bytes;
        return path.data[offset - 1] == '.'
            && !memcmp(path.data + offset, module.data, module.u8_bytes);
    }
    if (module.u8_bytes > path.u8_bytes) {
        size_t offset = module.u8_bytes - path.u8_bytes;
        return module.data[offset - 1] == '.'
            && !memcmp(module.data + offset, path.data, path.u8_bytes);
    }
    return false;
}

static pp_lookup_t
local_binding_lookup(pp_context_t *ctx, core_string_t name,
                     pp_value_t *value)
{
    for (size_t i = ctx->bindings.len; i > 0; i--) {
        pp_binding_t *binding = &ctx->bindings.data[i - 1];
        if (core_string_eq(binding->name, name)
            && core_string_eq(binding->module, ctx->unit->module.identifier)) {
            if (!binding->eligible) return PP_LOOKUP_INELIGIBLE;
            *value = binding->value;
            return PP_LOOKUP_FOUND;
        }
    }
    return PP_LOOKUP_NONE;
}

static void
pp_warn_cycle_dependency(pp_context_t *ctx, parse_tree_t *at,
                         core_string_t module)
{
    if (ctx->type_only || ctx->cycle_warning_emitted) return;
    for (size_t i = 0; i < ctx->modules.len; i++) {
        if (!ctx->modules.data[i].cyclic
            || !core_string_eq(module,
                               ctx->modules.data[i].unit->module.identifier))
            continue;
        ctx->cycle_warning_emitted = true;
        core_buffer_t *message = core_buffer_empty();
        parse_token_info_t *token = parse_tree_find_first_terminal(at);
        if (token)
            core_buffer_printf(message, "line %u, column %u: ",
                               token->line, token->column);
        core_buffer_printf(message,
            "preprocessor value depends on cyclic import '%.*s'; results follow deterministic import order",
            (int)module.u8_bytes, module.data);
        core_diagnostics_report(core_translation_unit_diagnostics(ctx->unit),
            ctx->unit, CORE_DIAG_WARNING, CORE_STRING_STATIC("N00BB-PPW0002"),
            core_string_borrow(message->data, message->byte_len));
        core_buffer_free(message);
        return;
    }
}

static pp_lookup_t
imported_binding_lookup(pp_context_t *ctx, parse_tree_t *node,
                        core_string_t name,
                        pp_value_t *value)
{
    size_t matches = 0;
    pp_binding_t *match = nullptr;
    for (size_t m = 0; m < core_translation_unit_import_count(ctx->unit); m++) {
        core_translation_unit_t *imported = core_translation_unit_import(
            ctx->unit, m);
        core_string_t module = imported->module.identifier;
        pp_binding_t *latest = nullptr;
        for (size_t i = ctx->bindings.len; i > 0; i--) {
            pp_binding_t *binding = &ctx->bindings.data[i - 1];
            if (core_string_eq(binding->module, module)
                && core_string_eq(binding->name, name)) {
                latest = binding;
                break;
            }
        }
        if (!latest) continue;
        matches++;
        match = latest;
    }
    if (matches > 1) return PP_LOOKUP_AMBIGUOUS;
    if (!match) return PP_LOOKUP_NONE;
    if (!match->eligible) return PP_LOOKUP_INELIGIBLE;
    *value = match->value;
    pp_warn_cycle_dependency(ctx, node, match->module);
    return PP_LOOKUP_FOUND;
}

static bool
collect_name_chain(pp_context_t *ctx, parse_tree_t *node, core_buffer_t *name)
{
    if (is_name(ctx->grammar, node, "primary")
        && core_tree_num_children(node) == 1) {
        parse_tree_t *child = core_tree_child(node, 0);
        if (!core_tree_is_leaf(child)) return false;
        parse_token_info_t *token = parse_node_token(child);
        if (!token || !core_string_eq(parse_terminal_name(ctx->grammar, token->tid),
                                      CORE_STRING_STATIC("IDENTIFIER")))
            return false;
        core_string_t text = leaf_text(ctx, child);
        core_buffer_append(name, text.data, text.u8_bytes);
        return true;
    }
    if (!is_name(ctx->grammar, node, "postfix-expr")) return false;
    parse_tree_t *dot = nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *part = core_tree_child(node, i);
        if (core_tree_is_leaf(part)
            && parse_tree_leaf_is(part, CORE_STRING_STATIC("."))) {
            dot = part;
            break;
        }
    }
    if (!dot && core_tree_num_children(node) == 1)
        return collect_name_chain(ctx, core_tree_child(node, 0), name);
    if (dot && core_tree_num_children(node) == 3) {
        parse_tree_t *member = core_tree_child(node, 2);
        parse_token_info_t *token = parse_node_token(member);
        if (!token || !core_string_eq(parse_terminal_name(ctx->grammar, token->tid),
                                      CORE_STRING_STATIC("IDENTIFIER")))
            return false;
        if (!collect_name_chain(ctx, core_tree_child(node, 0), name)) return false;
        core_buffer_putc(name, '.');
        core_string_t text = leaf_text(ctx, member);
        core_buffer_append(name, text.data, text.u8_bytes);
        return true;
    }
    return false;
}

static bool
postfix_has_token(parse_tree_t *node, const char *token)
{
    if (!node || core_tree_is_leaf(node)) return false;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *part = core_tree_child(node, i);
        if (core_tree_is_leaf(part)
            && parse_tree_leaf_is(part, core_string_from_cstr(token)))
            return true;
    }
    return false;
}

static pp_lookup_t
qualified_binding_lookup(pp_context_t *ctx, parse_tree_t *node,
                          pp_value_t *value)
{
    core_buffer_t *chain = core_buffer_empty();
    if (!collect_name_chain(ctx, node, chain)) {
        core_buffer_free(chain);
        return PP_LOOKUP_NONE;
    }
    char *last_dot = strrchr(chain->data, '.');
    if (!last_dot) {
        core_buffer_free(chain);
        return PP_LOOKUP_NONE;
    }
    core_string_t module = core_string_borrow(chain->data,
        (size_t)(last_dot - chain->data));
    core_string_t member = core_string_borrow(last_dot + 1,
        chain->byte_len - (size_t)(last_dot + 1 - chain->data));
    core_string_t target_module = {0};
    bool ambiguous = false;
    core_string_t current = ctx->unit->module.identifier;
    if (path_matches_module(module, current)) target_module = current;
    if (!target_module.data) {
        for (size_t i = 0;
             i < core_translation_unit_import_count(ctx->unit); i++) {
            core_translation_unit_t *imported_unit =
                core_translation_unit_import(ctx->unit, i);
            core_string_t imported = imported_unit->module.identifier;
            if (!path_matches_module(module, imported)) continue;
            if (target_module.data
                && !core_string_eq(target_module, imported)) {
                ambiguous = true;
                break;
            }
            target_module = imported;
        }
    }
    pp_lookup_t result = PP_LOOKUP_NONE;
    if (ambiguous) result = PP_LOOKUP_AMBIGUOUS;
    else if (target_module.data) {
        for (size_t i = ctx->bindings.len; i > 0; i--) {
            pp_binding_t *binding = &ctx->bindings.data[i - 1];
            if (!core_string_eq(target_module, binding->module)
                || !core_string_eq(member, binding->name)) continue;
            if (binding->eligible) {
                *value = binding->value;
                if (!core_string_eq(target_module, current))
                    pp_warn_cycle_dependency(ctx, node, target_module);
                result = PP_LOOKUP_FOUND;
            }
            else result = PP_LOOKUP_INELIGIBLE;
            break;
        }
    }
    core_buffer_free(chain);
    return result;
}

static bool eval_expr(pp_context_t *ctx, parse_tree_t *node,
                      pp_value_t *out);

static size_t
count_expressions(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return 0;
    if (is_name(grammar, node, "expression")) return 1;
    size_t count = 0;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        count += count_expressions(grammar, core_tree_child(node, i));
    return count;
}

static size_t
builtin_argument_count(pp_context_t *ctx, parse_tree_t *call)
{
    parse_tree_t *args = child_named(ctx->grammar, call, "call-args");
    if (!args && is_name(ctx->grammar, call, "tagged-lit")) {
        parse_tree_t *payload = child_named(ctx->grammar, call,
                                             "literal-payload");
        parse_tree_t *tuple = payload
            ? child_named(ctx->grammar, payload, "tuple-or-paren") : nullptr;
        if (!tuple) return SIZE_MAX;
        if (core_tree_node_value(tuple).rule_index == 1) return 1;
        return SIZE_MAX;
    }
    if (!args) return 0;
    if (child_named(ctx->grammar, args, "keyword-args")) return SIZE_MAX;
    return count_expressions(ctx->grammar,
        child_named(ctx->grammar, args, "positional-args"));
}

static bool
find_external_definition(pp_context_t *ctx, core_string_t name,
                         core_string_t *raw)
{
    for (size_t i = ctx->options ? ctx->options->definition_count : 0;
         i > 0; i--) {
        const char *definition = ctx->options->definitions[i - 1];
        const char *equals = definition ? strchr(definition, '=') : nullptr;
        if (!equals || (size_t)(equals - definition) != name.u8_bytes
            || (name.u8_bytes
                && memcmp(definition, name.data, name.u8_bytes))) continue;
        *raw = core_string_borrow(equals + 1, strlen(equals + 1));
        return true;
    }
    return false;
}

static pp_kind_t
external_definition_kind(core_string_t raw)
{
    if (core_string_eq(raw, CORE_STRING_STATIC("true"))
        || core_string_eq(raw, CORE_STRING_STATIC("false"))) return PP_BOOL;
    __int128 integer;
    if (parse_integer(raw, &integer)) return PP_INT;
    if (integer_syntax(raw)) return PP_INT;
    char *copy = core_strndup(raw.data, raw.u8_bytes);
    char *end = nullptr;
    errno = 0;
    (void)strtod(copy, &end);
    bool float_value = end && end != copy && *end == '\0'
        && (memchr(copy, '.', raw.u8_bytes)
            || memchr(copy, 'e', raw.u8_bytes)
            || memchr(copy, 'E', raw.u8_bytes));
    core_free(copy);
    return float_value ? PP_FLOAT : PP_STRING;
}

static bool
parse_literal(pp_context_t *ctx, parse_tree_t *node, pp_value_t *out)
{
    if (is_name(ctx->grammar, node, "simple-lit")) {
        parse_tree_t *leaf = first_node(node);
        parse_token_info_t *token = leaf ? parse_node_token(leaf) : nullptr;
        if (!token) return false;
        core_string_t terminal = parse_terminal_name(ctx->grammar, token->tid);
        core_string_t text = leaf_text(ctx, leaf);
        if (core_string_eq(terminal, CORE_STRING_STATIC("INT_LIT"))
            || core_string_eq(terminal, CORE_STRING_STATIC("HEX_LIT"))) {
            __int128 integer;
            if (parse_integer(text, &integer)) {
                *out = (pp_value_t){.kind = PP_INT, .as.integer = integer};
                return true;
            }
            pp_error(ctx, node, "integer literal is outside the preprocessor i128 range");
            return false;
        }
        if (core_string_eq(terminal, CORE_STRING_STATIC("FLOAT_LIT"))) {
            char *copy = core_strndup(text.data, text.u8_bytes);
            char *end = nullptr;
            errno = 0;
            double value = strtod(copy, &end);
            bool valid = end && *end == '\0' && end != copy && errno != ERANGE;
            core_free(copy);
            if (valid) {
                *out = (pp_value_t){.kind = PP_FLOAT, .as.real = value};
                return true;
            }
            pp_error(ctx, node, "invalid floating-point literal in preprocessor expression");
            return false;
        }
        if (core_string_eq(terminal, CORE_STRING_STATIC("STRING_LIT"))) {
            core_string_t decoded;
            if (decode_quoted_string(ctx, text, &decoded)) {
                *out = (pp_value_t){.kind = PP_STRING, .as.string = decoded};
                return true;
            }
            pp_error(ctx, node, "invalid simple string literal in preprocessor expression");
            return false;
        }
        if (core_string_eq(terminal, CORE_STRING_STATIC("true"))
            || core_string_eq(terminal, CORE_STRING_STATIC("false"))) {
            *out = (pp_value_t){.kind = PP_BOOL,
                .as.boolean = core_string_eq(terminal, CORE_STRING_STATIC("true"))};
            return true;
        }
        return false;
    }
    if (is_name(ctx->grammar, node, "bool-lit")) {
        core_string_t value = first_leaf_text(ctx, node);
        if (core_string_eq(value, CORE_STRING_STATIC("true"))) {
            *out = (pp_value_t){.kind = PP_BOOL, .as.boolean = true};
            return true;
        }
        if (core_string_eq(value, CORE_STRING_STATIC("false"))) {
            *out = (pp_value_t){.kind = PP_BOOL, .as.boolean = false};
            return true;
        }
    }
    if (is_name(ctx->grammar, node, "int-lit")) {
        core_string_t text = first_leaf_text(ctx, node);
        __int128 integer;
        if (parse_integer(text, &integer)) {
            *out = (pp_value_t){.kind = PP_INT, .as.integer = integer};
            return true;
        }
        pp_error(ctx, node, "integer literal is outside the preprocessor i128 range");
        return false;
    }
    if (is_name(ctx->grammar, node, "float-lit")) {
        core_string_t text = first_leaf_text(ctx, node);
        char *copy = core_strndup(text.data, text.u8_bytes);
        char *end = nullptr;
        errno = 0;
        double value = strtod(copy, &end);
        bool valid = end && *end == '\0' && end != copy && errno != ERANGE;
        core_free(copy);
        if (valid) {
            *out = (pp_value_t){.kind = PP_FLOAT, .as.real = value};
            return true;
        }
        pp_error(ctx, node, "invalid floating-point literal in preprocessor expression");
        return false;
    }
    if (is_name(ctx->grammar, node, "string-lit")) {
        parse_tree_t *leaf = first_node(node);
        parse_token_info_t *token = leaf ? parse_node_token(leaf) : nullptr;
        if (token && token->has_content_span) {
            const char *source = core_source_file_contents(&ctx->unit->source);
            size_t size = core_source_file_size(&ctx->unit->source);
            if (token->content_start_byte <= token->content_end_byte
                && token->content_end_byte <= size) {
                *out = (pp_value_t){.kind = PP_STRING,
                    .as.string = core_string_from_raw(
                        source + token->content_start_byte,
                        (int64_t)(token->content_end_byte - token->content_start_byte))};
                return true;
            }
        }
    }
    return false;
}

static bool
literal_initializer_shape(pp_context_t *ctx, parse_tree_t *node)
{
    if (is_name(ctx->grammar, node, "simple-lit")) return true;
    if (!node || core_tree_is_leaf(node)) return false;
    if (is_name(ctx->grammar, node, "unary-expr")
        && core_tree_num_children(node) == 2) {
        core_string_t op = leaf_text(ctx, core_tree_child(node, 0));
        return (core_string_eq(op, CORE_STRING_STATIC("+"))
                || core_string_eq(op, CORE_STRING_STATIC("-")))
            && literal_initializer_shape(ctx, core_tree_child(node, 1));
    }
    return core_tree_num_children(node) == 1
        && literal_initializer_shape(ctx, core_tree_child(node, 0));
}

static bool
integer_literal_text(pp_context_t *ctx, parse_tree_t *node,
                     core_string_t *text)
{
    while (node && !core_tree_is_leaf(node)
           && !is_name(ctx->grammar, node, "simple-lit")
           && core_tree_num_children(node) == 1)
        node = core_tree_child(node, 0);
    if (!is_name(ctx->grammar, node, "simple-lit")) return false;
    parse_tree_t *leaf = first_node(node);
    parse_token_info_t *token = parse_node_token(leaf);
    if (!token) return false;
    core_string_t terminal = parse_terminal_name(ctx->grammar, token->tid);
    if (!core_string_eq(terminal, CORE_STRING_STATIC("INT_LIT"))
        && !core_string_eq(terminal, CORE_STRING_STATIC("HEX_LIT")))
        return false;
    *text = leaf_text(ctx, leaf);
    return true;
}

static bool
parse_negated_integer_literal(core_string_t text, __int128 *out)
{
    core_buffer_t *signed_text = core_buffer_empty();
    core_buffer_putc(signed_text, '-');
    core_buffer_append(signed_text, text.data, text.u8_bytes);
    core_string_t value = core_string_borrow(signed_text->data,
                                              signed_text->byte_len);
    bool parsed = parse_integer(value, out);
    core_buffer_free(signed_text);
    return parsed;
}

static bool
const_integer_representable(pp_context_t *ctx, parse_tree_t *node)
{
    core_string_t text;
    parse_tree_t *literal_node = node;
    while (literal_node && !core_tree_is_leaf(literal_node)
           && core_tree_num_children(literal_node) == 1
           && !is_name(ctx->grammar, literal_node, "unary-expr"))
        literal_node = core_tree_child(literal_node, 0);
    if (is_name(ctx->grammar, literal_node, "unary-expr")
        && core_tree_num_children(literal_node) == 2
        && parse_tree_leaf_is(core_tree_child(literal_node, 0),
                              CORE_STRING_STATIC("-"))) {
        if (!integer_literal_text(ctx, core_tree_child(literal_node, 1), &text))
            return true;
        __int128 ignored;
        return parse_negated_integer_literal(text, &ignored);
    }
    if (!integer_literal_text(ctx, literal_node, &text)) return true;
    __int128 ignored;
    return parse_integer(text, &ignored);
}

static bool
is_literal_node(parse_grammar_t *g, parse_tree_t *node)
{
    return is_name(g, node, "simple-lit") || is_name(g, node, "bool-lit")
        || is_name(g, node, "int-lit") || is_name(g, node, "float-lit")
        || is_name(g, node, "string-lit");
}

static bool
is_binary_operator_node(parse_grammar_t *g, parse_tree_t *node)
{
    static const char *names[] = {
        "or-expr", "and-expr", "equality-expr", "compare-expr",
        "bitor-expr", "bitxor-expr", "bitand-expr", "shift-expr",
        "add-expr", "mul-expr", nullptr,
    };
    for (size_t i = 0; names[i]; i++)
        if (is_name(g, node, names[i])) return true;
    return false;
}

static pp_kind_t
binary_result_kind(core_string_t op, pp_kind_t left, pp_kind_t right)
{
    bool any = left == PP_ANY || right == PP_ANY;
    bool same = left == right || any;
    bool numeric = (left == PP_INT || left == PP_FLOAT || left == PP_ANY)
        && (right == PP_INT || right == PP_FLOAT || right == PP_ANY);
    if ((core_string_eq(op, CORE_STRING_STATIC("and"))
         || core_string_eq(op, CORE_STRING_STATIC("or")))
        && (left == PP_BOOL || left == PP_ANY)
        && (right == PP_BOOL || right == PP_ANY)) return PP_BOOL;
    if ((core_string_eq(op, CORE_STRING_STATIC("=="))
         || core_string_eq(op, CORE_STRING_STATIC("!=")))
        && same && (left == PP_BOOL || left == PP_INT || left == PP_FLOAT
                    || left == PP_STRING || left == PP_ANY)) return PP_BOOL;
    if ((core_string_eq(op, CORE_STRING_STATIC("<"))
         || core_string_eq(op, CORE_STRING_STATIC("<="))
         || core_string_eq(op, CORE_STRING_STATIC(">"))
         || core_string_eq(op, CORE_STRING_STATIC(">=")))
        && same && (left == PP_INT || left == PP_FLOAT || left == PP_ANY))
        return PP_BOOL;
    if ((core_string_eq(op, CORE_STRING_STATIC("+"))
         || core_string_eq(op, CORE_STRING_STATIC("-"))
         || core_string_eq(op, CORE_STRING_STATIC("*"))
         || core_string_eq(op, CORE_STRING_STATIC("/")))
        && same && numeric)
        return left == PP_ANY ? right : left;
    if ((core_string_eq(op, CORE_STRING_STATIC("%"))
         || core_string_eq(op, CORE_STRING_STATIC("&"))
         || core_string_eq(op, CORE_STRING_STATIC("^"))
         || core_string_eq(op, CORE_STRING_STATIC("|"))
         || core_string_eq(op, CORE_STRING_STATIC("<<"))
         || core_string_eq(op, CORE_STRING_STATIC(">>")))
        && (left == PP_INT || left == PP_ANY)
        && (right == PP_INT || right == PP_ANY)) return PP_INT;
    return PP_INVALID;
}

static pp_kind_t
const_literal_kind(pp_context_t *ctx, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return PP_INVALID;
    if (is_name(ctx->grammar, node, "simple-lit")) {
        parse_tree_t *leaf = first_node(node);
        parse_token_info_t *token = parse_node_token(leaf);
        if (!token) return PP_INVALID;
        core_string_t terminal = parse_terminal_name(ctx->grammar, token->tid);
        if (core_string_eq(terminal, CORE_STRING_STATIC("INT_LIT"))
            || core_string_eq(terminal, CORE_STRING_STATIC("HEX_LIT")))
            return PP_INT;
        if (core_string_eq(terminal, CORE_STRING_STATIC("FLOAT_LIT")))
            return PP_FLOAT;
        if (core_string_eq(terminal, CORE_STRING_STATIC("STRING_LIT")))
            return PP_STRING;
        if (core_string_eq(terminal, CORE_STRING_STATIC("true"))
            || core_string_eq(terminal, CORE_STRING_STATIC("false")))
            return PP_BOOL;
        return PP_INVALID;
    }
    if (is_name(ctx->grammar, node, "unary-expr")
        && core_tree_num_children(node) == 2) {
        core_string_t op = leaf_text(ctx, core_tree_child(node, 0));
        if (!core_string_eq(op, CORE_STRING_STATIC("+"))
            && !core_string_eq(op, CORE_STRING_STATIC("-")))
            return PP_INVALID;
        pp_kind_t operand = const_literal_kind(
            ctx, core_tree_child(node, 1));
        return operand == PP_INT || operand == PP_FLOAT
            ? operand : PP_INVALID;
    }
    return core_tree_num_children(node) == 1
        ? const_literal_kind(ctx, core_tree_child(node, 0)) : PP_INVALID;
}

static bool
eval_leaf_identifier(pp_context_t *ctx, parse_tree_t *node, pp_value_t *out)
{
    core_string_t name = leaf_text(ctx, node);
    pp_lookup_t lookup = local_binding_lookup(ctx, name, out);
    if (lookup == PP_LOOKUP_FOUND) return true;
    if (lookup == PP_LOOKUP_NONE) {
        lookup = imported_binding_lookup(ctx, node, name, out);
        if (lookup == PP_LOOKUP_FOUND) return true;
    }
    if (lookup == PP_LOOKUP_AMBIGUOUS) {
        pp_error(ctx, node,
            "imported const name is ambiguous; qualify it with its module path");
        return false;
    }
    pp_error(ctx, node, "preprocessor expression refers to an ineligible const variable");
    return false;
}

static bool
eval_builtin(pp_context_t *ctx, parse_tree_t *call, pp_value_t *out)
{
    while (call && !core_tree_is_leaf(call)
           && core_tree_num_children(call) == 1
           && !is_name(ctx->grammar, call, "tagged-lit")
           && !is_name(ctx->grammar, call, "postfix-expr"))
        call = core_tree_child(call, 0);
    bool tagged_call = is_name(ctx->grammar, call, "tagged-lit");
    parse_tree_t *callee = core_tree_num_children(call)
        ? core_tree_child(call, 0) : nullptr;
    if (tagged_call) callee = child_named(ctx->grammar, call,
                                           "literal-prefix");
    while (callee && !core_tree_is_leaf(callee)
           && core_tree_num_children(callee) == 1)
        callee = core_tree_child(callee, 0);
    core_string_t name = callee && core_tree_is_leaf(callee)
        ? leaf_text(ctx, callee) : (core_string_t){0};
    size_t argument_count = builtin_argument_count(ctx, call);
    bool needs_string = core_string_eq(name, CORE_STRING_STATIC("preproc_env"))
        || core_string_eq(name, CORE_STRING_STATIC("preproc_env_defines"))
        || core_string_eq(name, CORE_STRING_STATIC("preproc_defines"))
        || core_string_eq(name, CORE_STRING_STATIC("preproc_value"))
        || core_string_eq(name, CORE_STRING_STATIC("compile_warning"))
        || core_string_eq(name, CORE_STRING_STATIC("compile_error"));
    if (argument_count != (needs_string ? 1u : 0u)) {
        pp_error(ctx, call, "processor-time builtin called with the wrong number of arguments");
        return false;
    }
    parse_tree_t *arg = nullptr;
    parse_tree_t *args = child_named(ctx->grammar, call, "call-args");
    if (tagged_call) {
        parse_tree_t *payload = child_named(ctx->grammar, call,
                                             "literal-payload");
        parse_tree_t *tuple = payload
            ? child_named(ctx->grammar, payload, "tuple-or-paren") : nullptr;
        arg = tuple && core_tree_node_value(tuple).rule_index == 1
            ? child_named(ctx->grammar, tuple, "expression") : nullptr;
    }
    else {
        parse_tree_t *positional = args
            ? child_named(ctx->grammar, args, "positional-args") : nullptr;
        if (positional && core_tree_num_children(positional))
            arg = core_tree_child(positional, 0);
    }
    core_string_t argument = {0};
    if (arg) {
        pp_value_t arg_value;
        if (!eval_expr(ctx, arg, &arg_value)
            || (arg_value.kind != PP_STRING
                && !(ctx->type_only && arg_value.kind == PP_ANY))) {
            pp_error(ctx, call, "preprocessor builtin argument must have type string");
            return false;
        }
        if (arg_value.kind == PP_STRING) argument = arg_value.as.string;
    }
    if (ctx->type_only) {
        pp_kind_t kind = PP_INVALID;
        if (core_string_eq(name, CORE_STRING_STATIC("compile_warning"))
            || core_string_eq(name, CORE_STRING_STATIC("compile_error"))) {
            *out = (pp_value_t){.kind = PP_INVALID};
            return true;
        }
        if (core_string_eq(name, CORE_STRING_STATIC("target_os"))
            || core_string_eq(name, CORE_STRING_STATIC("target_arch"))
            || core_string_eq(name, CORE_STRING_STATIC("n00b_impl"))
            || core_string_eq(name, CORE_STRING_STATIC("preproc_env")))
            kind = PP_STRING;
        else if (core_string_eq(name, CORE_STRING_STATIC("target_is_little_endian"))
                 || core_string_eq(name, CORE_STRING_STATIC("debug_enabled"))
                 || core_string_eq(name, CORE_STRING_STATIC("preproc_env_defines"))
                 || core_string_eq(name, CORE_STRING_STATIC("preproc_defines")))
            kind = PP_BOOL;
        else if (core_string_eq(name, CORE_STRING_STATIC("preproc_value"))) {
            core_string_t raw = {0};
            kind = argument.data
                ? (find_external_definition(ctx, argument, &raw)
                    ? external_definition_kind(raw) : PP_ANY)
                : PP_ANY;
        }
        else if (n00bb_preprocessor_builtin_name(name)) kind = PP_INT;
        else {
            pp_error(ctx, call,
                     "unsupported processor-time builtin in preprocessor expression");
            return false;
        }
        *out = (pp_value_t){.kind = kind};
        return true;
    }
    if (core_string_eq(name, CORE_STRING_STATIC("compile_warning"))
        || core_string_eq(name, CORE_STRING_STATIC("compile_error"))) {
        if (!arg) {
            pp_error(ctx, call, "compile-time diagnostic requires one string argument");
            return false;
        }
        bool fatal = core_string_eq(name, CORE_STRING_STATIC("compile_error"));
        parse_token_info_t *token = parse_tree_find_first_terminal(call);
        core_buffer_t *message = core_buffer_empty();
        if (token)
            core_buffer_printf(message, "line %u, column %u: ",
                               token->line, token->column);
        core_buffer_append(message, argument.data, argument.u8_bytes);
        core_diagnostics_report(core_translation_unit_diagnostics(ctx->unit),
            ctx->unit, fatal ? CORE_DIAG_FATAL : CORE_DIAG_WARNING,
            fatal ? CORE_STRING_STATIC("N00BB-PP0002")
                  : CORE_STRING_STATIC("N00BB-PPW0001"),
            core_string_borrow(message->data, message->byte_len));
        core_buffer_free(message);
        if (fatal) ctx->failed = true;
        *out = (pp_value_t){.kind = PP_INVALID};
        return true;
    }
#define PP_INT(v) do { *out = (pp_value_t){.kind = PP_INT, .as.integer = (v)}; return true; } while (0)
#define PP_STRING(v) do { *out = (pp_value_t){.kind = PP_STRING, .as.string = own_string(ctx, core_string_from_cstr(v))}; return true; } while (0)
    if (core_string_eq(name, CORE_STRING_STATIC("target_os"))) {
#if defined(__APPLE__)
        PP_STRING("macos");
#elif defined(_WIN32)
        PP_STRING("windows");
#elif defined(__linux__)
        PP_STRING("linux");
#else
        pp_error(ctx, call, "unsupported target operating system");
        return false;
#endif
    }
    if (core_string_eq(name, CORE_STRING_STATIC("target_arch"))) {
#if defined(__x86_64__) || defined(_M_X64)
        PP_STRING("x86");
#elif defined(__aarch64__) || defined(_M_ARM64)
        PP_STRING("arm");
#else
        pp_error(ctx, call, "unsupported target architecture");
        return false;
#endif
    }
    if (core_string_eq(name, CORE_STRING_STATIC("target_alu"))) {
#if defined(__x86_64__) || defined(_M_X64) \
    || defined(__aarch64__) || defined(_M_ARM64)
        PP_INT(64);
#else
        pp_error(ctx, call, "target ALU is unavailable for this architecture");
        return false;
#endif
    }
    if (core_string_eq(name, CORE_STRING_STATIC("target_is_little_endian"))) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        *out = (pp_value_t){.kind = PP_BOOL, .as.boolean = false};
#else
        *out = (pp_value_t){.kind = PP_BOOL, .as.boolean = true};
#endif
        return true;
    }
    if (core_string_eq(name, CORE_STRING_STATIC("debug_enabled"))) {
        *out = (pp_value_t){.kind = PP_BOOL,
            .as.boolean = !ctx->options || !ctx->options->release_mode};
        return true;
    }
    if (core_string_eq(name, CORE_STRING_STATIC("preproc_utc_year"))) PP_INT(ctx->utc_time.tm_year + 1900);
    if (core_string_eq(name, CORE_STRING_STATIC("preproc_utc_month"))) PP_INT(ctx->utc_time.tm_mon + 1);
    if (core_string_eq(name, CORE_STRING_STATIC("preproc_utc_day"))) PP_INT(ctx->utc_time.tm_mday);
    if (core_string_eq(name, CORE_STRING_STATIC("preproc_utc_hour"))) PP_INT(ctx->utc_time.tm_hour);
    if (core_string_eq(name, CORE_STRING_STATIC("preproc_utc_minute"))) PP_INT(ctx->utc_time.tm_min);
    if (core_string_eq(name, CORE_STRING_STATIC("preproc_utc_second"))) PP_INT(ctx->utc_time.tm_sec);
    if (core_string_eq(name, CORE_STRING_STATIC("target_os_major"))
        || core_string_eq(name, CORE_STRING_STATIC("target_os_minor"))
        || core_string_eq(name, CORE_STRING_STATIC("target_os_patch"))) {
#if defined(__APPLE__) && defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
        int version = __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__;
        if (core_string_eq(name, CORE_STRING_STATIC("target_os_major"))) PP_INT(version / 10000);
        if (core_string_eq(name, CORE_STRING_STATIC("target_os_minor"))) PP_INT((version / 100) % 100);
        PP_INT(version % 100);
#else
        pp_error(ctx, call, "target OS version is unavailable for this target");
        return false;
#endif
    }
    if (core_string_eq(name, CORE_STRING_STATIC("n00b_major"))) PP_INT(0);
    if (core_string_eq(name, CORE_STRING_STATIC("n00b_minor"))) PP_INT(1);
    if (core_string_eq(name, CORE_STRING_STATIC("n00b_patch"))) PP_INT(0);
    if (core_string_eq(name, CORE_STRING_STATIC("n00b_impl"))) PP_STRING("n00bb");
#undef PP_INT
#undef PP_STRING

    if (needs_string && !arg) {
        pp_error(ctx, call, "preprocessor builtin requires one string argument");
        return false;
    }
    if (core_string_eq(name, CORE_STRING_STATIC("preproc_env"))
        || core_string_eq(name, CORE_STRING_STATIC("preproc_env_defines"))) {
        char *key = core_strndup(argument.data, argument.u8_bytes);
        const char *value = getenv(key);
        bool defines = value != nullptr;
        if (core_string_eq(name, CORE_STRING_STATIC("preproc_env_defines")))
            *out = (pp_value_t){.kind = PP_BOOL, .as.boolean = defines};
        else
            *out = (pp_value_t){.kind = PP_STRING,
                .as.string = own_string(ctx,
                    core_string_from_cstr(value ? value : ""))};
        core_free(key);
        return true;
    }
    if (core_string_eq(name, CORE_STRING_STATIC("preproc_defines"))
        || core_string_eq(name, CORE_STRING_STATIC("preproc_value"))) {
        core_string_t raw = {0};
        bool defined = find_external_definition(ctx, argument, &raw);
        if (core_string_eq(name, CORE_STRING_STATIC("preproc_defines"))) {
            *out = (pp_value_t){.kind = PP_BOOL, .as.boolean = defined};
            return true;
        }
        if (!defined) {
            pp_error(ctx, call, "preproc_value() has no externally supplied value");
            return false;
        }
        if (core_string_eq(raw, CORE_STRING_STATIC("true"))
            || core_string_eq(raw, CORE_STRING_STATIC("false"))) {
            *out = (pp_value_t){.kind = PP_BOOL,
                .as.boolean = core_string_eq(raw, CORE_STRING_STATIC("true"))};
            return true;
        }
        __int128 integer;
        if (parse_integer(raw, &integer)) {
            *out = (pp_value_t){.kind = PP_INT, .as.integer = integer};
            return true;
        }
        if (integer_syntax(raw)) {
            pp_error(ctx, call,
                     "external integer value is outside the preprocessor i128 range");
            return false;
        }
        char *copy = core_strndup(raw.data, raw.u8_bytes);
        char *end = nullptr;
        errno = 0;
        double real = strtod(copy, &end);
        bool float_value = end && end != copy && *end == '\0'
            && (memchr(copy, '.', raw.u8_bytes)
                || memchr(copy, 'e', raw.u8_bytes)
                || memchr(copy, 'E', raw.u8_bytes));
        if (float_value) {
            if (errno == ERANGE || !isfinite(real)) {
                core_free(copy);
                pp_error(ctx, call,
                         "external float value is outside the preprocessor f64 range");
                return false;
            }
            core_free(copy);
            *out = (pp_value_t){.kind = PP_FLOAT, .as.real = real};
            return true;
        }
        core_free(copy);
        if (raw.u8_bytes && raw.data[0] == '"') {
            core_string_t decoded;
            if (!decode_quoted_string(ctx, raw, &decoded)) return false;
            *out = (pp_value_t){.kind = PP_STRING, .as.string = decoded};
        }
        else *out = (pp_value_t){.kind = PP_STRING,
            .as.string = own_string(ctx, core_string_from_raw(raw.data,
                (int64_t)raw.u8_bytes))};
        return true;
    }
    pp_error(ctx, call, "unsupported processor-time builtin in preprocessor expression");
    return false;
}

static bool
eval_expr(pp_context_t *ctx, parse_tree_t *node, pp_value_t *out)
{
    if (!node) return false;
    if (core_tree_is_leaf(node)) return eval_leaf_identifier(ctx, node, out);

    core_string_t name = node_name(ctx->grammar, node);
    if (is_name(ctx->grammar, node, "tagged-lit")
        && n00bb_preprocessor_builtin_name(builtin_name(ctx, node)))
        return eval_builtin(ctx, node, out);
    if (is_literal_node(ctx->grammar, node)) {
        if (parse_literal(ctx, node, out)) return true;
        pp_error(ctx, node, "only simple primitive literals are allowed in preprocessor expressions");
        return false;
    }
    if (core_string_eq(name, CORE_STRING_STATIC("primary"))) {
        if (core_tree_num_children(node) != 1) goto invalid;
        parse_tree_t *child = core_tree_child(node, 0);
        if (core_tree_is_leaf(child)) return eval_leaf_identifier(ctx, child, out);
        return eval_expr(ctx, child, out);
    }
    if (core_string_eq(name, CORE_STRING_STATIC("tuple-or-paren"))) {
        if (core_tree_node_value(node).rule_index == 1) {
            parse_tree_t *inner = child_named(ctx->grammar, node, "expression");
            if (inner) return eval_expr(ctx, inner, out);
        }
        goto invalid;
    }
    if (core_string_eq(name, CORE_STRING_STATIC("postfix-expr"))) {
        if (core_tree_num_children(node) == 1)
            return eval_expr(ctx, core_tree_child(node, 0), out);
        if (postfix_has_token(node, ".")) {
            pp_lookup_t lookup = qualified_binding_lookup(ctx, node, out);
            if (lookup == PP_LOOKUP_FOUND) return true;
            if (lookup == PP_LOOKUP_AMBIGUOUS) {
                pp_error(ctx, node,
                    "module qualifier is ambiguous; include its package path");
                return false;
            }
            pp_error(ctx, node, "preprocessor expression refers to an ineligible imported const");
            return false;
        }
        if (postfix_has_token(node, "("))
            return eval_builtin(ctx, node, out);
        goto invalid;
    }
    if (core_tree_num_children(node) == 1) {
        parse_tree_t *child = core_tree_child(node, 0);
        if (!core_tree_is_leaf(child)) return eval_expr(ctx, child, out);
    }

    if (core_string_eq(name, CORE_STRING_STATIC("unary-expr"))) {
        if (core_tree_num_children(node) == 2) {
            core_string_t op = leaf_text(ctx, core_tree_child(node, 0));
            if (core_string_eq(op, CORE_STRING_STATIC("-"))) {
                core_string_t literal_text;
                __int128 signed_value;
                if (integer_literal_text(ctx, core_tree_child(node, 1),
                                         &literal_text)
                    && !parse_integer(literal_text, &signed_value)
                    && parse_negated_integer_literal(literal_text,
                                                     &signed_value)) {
                    *out = (pp_value_t){.kind = PP_INT,
                                        .as.integer = signed_value};
                    return true;
                }
            }
            pp_value_t operand;
            if (!eval_expr(ctx, core_tree_child(node, 1), &operand)) return false;
            if (ctx->type_only) {
                pp_kind_t result = PP_INVALID;
                if (core_string_eq(op, CORE_STRING_STATIC("!"))
                    && (operand.kind == PP_BOOL || operand.kind == PP_ANY))
                    result = PP_BOOL;
                else if ((core_string_eq(op, CORE_STRING_STATIC("+"))
                          || core_string_eq(op, CORE_STRING_STATIC("-")))
                         && (operand.kind == PP_INT
                             || operand.kind == PP_FLOAT
                             || operand.kind == PP_ANY))
                    result = operand.kind;
                if (result == PP_INVALID) {
                    pp_error(ctx, node,
                             "unary operator is not valid for this preprocessor value type");
                    return false;
                }
                *out = (pp_value_t){.kind = result};
                return true;
            }
            if (core_string_eq(op, CORE_STRING_STATIC("!")) && operand.kind == PP_BOOL)
                *out = (pp_value_t){.kind = PP_BOOL, .as.boolean = !operand.as.boolean};
            else if (core_string_eq(op, CORE_STRING_STATIC("+"))
                     && (operand.kind == PP_INT || operand.kind == PP_FLOAT))
                *out = operand;
            else if (core_string_eq(op, CORE_STRING_STATIC("-")) && operand.kind == PP_FLOAT)
                *out = (pp_value_t){.kind = PP_FLOAT, .as.real = -operand.as.real};
            else if (core_string_eq(op, CORE_STRING_STATIC("-")) && operand.kind == PP_INT) {
                if (operand.as.integer == -((__int128)(((unsigned __int128)1 << 127) - 1)) - 1) {
                    pp_error(ctx, node, "checked i128 negation overflow");
                    return false;
                }
                *out = (pp_value_t){.kind = PP_INT, .as.integer = -operand.as.integer};
            }
            else {
                pp_error(ctx, node, "unary operator is not valid for this preprocessor value type");
                return false;
            }
            return true;
        }
        if (core_tree_num_children(node) == 1)
            return eval_expr(ctx, core_tree_child(node, 0), out);
    }

    if (is_binary_operator_node(ctx->grammar, node)
        && core_tree_num_children(node) == 3) {
        pp_value_t left, right;
        parse_tree_t *operator_node = core_tree_child(node, 1);
        core_string_t op = first_leaf_text(ctx, operator_node);
        if (!eval_expr(ctx, core_tree_child(node, 0), &left)) return false;
        if (!ctx->type_only && core_string_eq(op, CORE_STRING_STATIC("and"))
            && left.kind == PP_BOOL && !left.as.boolean) {
            *out = left;
            return true;
        }
        if (!ctx->type_only && core_string_eq(op, CORE_STRING_STATIC("or"))
            && left.kind == PP_BOOL && left.as.boolean) {
            *out = left;
            return true;
        }
        if (!eval_expr(ctx, core_tree_child(node, 2), &right)) return false;
        if (ctx->type_only) {
            pp_kind_t result = binary_result_kind(op, left.kind, right.kind);
            if (result == PP_INVALID) {
                pp_error(ctx, node,
                         "operator or operand type is not supported by the preprocessor");
                return false;
            }
            *out = (pp_value_t){.kind = result};
            return true;
        }
        if (left.kind == PP_BOOL && right.kind == PP_BOOL) {
            if (core_string_eq(op, CORE_STRING_STATIC("and")))
                *out = (pp_value_t){.kind = PP_BOOL,
                    .as.boolean = left.as.boolean && right.as.boolean};
            else if (core_string_eq(op, CORE_STRING_STATIC("or")))
                *out = (pp_value_t){.kind = PP_BOOL,
                    .as.boolean = left.as.boolean || right.as.boolean};
            else if (core_string_eq(op, CORE_STRING_STATIC("==")))
                *out = (pp_value_t){.kind = PP_BOOL,
                    .as.boolean = left.as.boolean == right.as.boolean};
            else if (core_string_eq(op, CORE_STRING_STATIC("!=")))
                *out = (pp_value_t){.kind = PP_BOOL,
                    .as.boolean = left.as.boolean != right.as.boolean};
            else goto type_error;
            return true;
        }
        if (left.kind == PP_STRING && right.kind == PP_STRING
            && (core_string_eq(op, CORE_STRING_STATIC("=="))
                || core_string_eq(op, CORE_STRING_STATIC("!=")))) {
            bool equal = core_string_eq(left.as.string, right.as.string);
            *out = (pp_value_t){.kind = PP_BOOL,
                .as.boolean = core_string_eq(op, CORE_STRING_STATIC("==")) ? equal : !equal};
            return true;
        }
        if (left.kind == PP_FLOAT && right.kind == PP_FLOAT) {
            double a = left.as.real, b = right.as.real;
            if (core_string_eq(op, CORE_STRING_STATIC("=="))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a==b};
            else if (core_string_eq(op, CORE_STRING_STATIC("!="))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a!=b};
            else if (core_string_eq(op, CORE_STRING_STATIC("<"))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a<b};
            else if (core_string_eq(op, CORE_STRING_STATIC("<="))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a<=b};
            else if (core_string_eq(op, CORE_STRING_STATIC(">"))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a>b};
            else if (core_string_eq(op, CORE_STRING_STATIC(">="))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a>=b};
            else if (core_string_eq(op, CORE_STRING_STATIC("+"))) *out = (pp_value_t){.kind=PP_FLOAT,.as.real=a+b};
            else if (core_string_eq(op, CORE_STRING_STATIC("-"))) *out = (pp_value_t){.kind=PP_FLOAT,.as.real=a-b};
            else if (core_string_eq(op, CORE_STRING_STATIC("*"))) *out = (pp_value_t){.kind=PP_FLOAT,.as.real=a*b};
            else if (core_string_eq(op, CORE_STRING_STATIC("/"))) *out = (pp_value_t){.kind=PP_FLOAT,.as.real=a/b};
            else goto type_error;
            return true;
        }
        if (left.kind == PP_INT && right.kind == PP_INT) {
            __int128 a = left.as.integer, b = right.as.integer, result = 0;
            bool overflow = false;
            if (core_string_eq(op, CORE_STRING_STATIC("=="))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a==b};
            else if (core_string_eq(op, CORE_STRING_STATIC("!="))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a!=b};
            else if (core_string_eq(op, CORE_STRING_STATIC("<"))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a<b};
            else if (core_string_eq(op, CORE_STRING_STATIC("<="))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a<=b};
            else if (core_string_eq(op, CORE_STRING_STATIC(">"))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a>b};
            else if (core_string_eq(op, CORE_STRING_STATIC(">="))) *out = (pp_value_t){.kind=PP_BOOL,.as.boolean=a>=b};
            else {
                if (core_string_eq(op, CORE_STRING_STATIC("+"))) overflow = __builtin_add_overflow(a,b,&result);
                else if (core_string_eq(op, CORE_STRING_STATIC("-"))) overflow = __builtin_sub_overflow(a,b,&result);
                else if (core_string_eq(op, CORE_STRING_STATIC("*"))) overflow = __builtin_mul_overflow(a,b,&result);
                else if (core_string_eq(op, CORE_STRING_STATIC("/"))) {
                    if (!b) goto arithmetic_error;
                    if (a == -((__int128)(((unsigned __int128)1 << 127)-1))-1 && b == -1) overflow = true;
                    else result = a / b;
                }
                else if (core_string_eq(op, CORE_STRING_STATIC("%"))) {
                    if (!b) goto arithmetic_error;
                    if (a == int128_minimum() && b == -1)
                        overflow = true;
                    else result = a % b;
                }
                else if (core_string_eq(op, CORE_STRING_STATIC("&"))) result = a & b;
                else if (core_string_eq(op, CORE_STRING_STATIC("|"))) result = a | b;
                else if (core_string_eq(op, CORE_STRING_STATIC("^"))) result = a ^ b;
                else if (core_string_eq(op, CORE_STRING_STATIC("<<")) || core_string_eq(op, CORE_STRING_STATIC(">>"))) {
                    if (b < 0 || b >= 128) goto arithmetic_error;
                    unsigned shift = (unsigned)b;
                    if (core_string_eq(op, CORE_STRING_STATIC("<<"))) {
                        if (!checked_left_shift(a, shift, &result))
                            goto arithmetic_error;
                    }
                    else result = a >= 0 ? a >> shift
                        : -1 - ((-1 - a) >> shift);
                }
                else goto type_error;
                if (overflow) goto arithmetic_error;
                *out = (pp_value_t){.kind = PP_INT, .as.integer = result};
            }
            return true;
        }
        goto type_error;
    }

invalid:
    pp_error(ctx, node, "expression form is not allowed in a preprocessor condition");
    return false;
type_error:
    pp_error(ctx, node, "operator or operand type is not supported by the preprocessor");
    return false;
arithmetic_error:
    pp_error(ctx, node, "invalid or overflowing checked i128 arithmetic");
    return false;
}

static void
remember_const(pp_context_t *ctx, parse_tree_t *decl)
{
    parse_tree_t *prefix = child_named(ctx->grammar, decl, "declaration-prefix");
    core_string_t binding_kind = first_leaf_text(ctx, child_named(
        ctx->grammar, prefix, "binding-kind"));
    bool is_const = core_string_eq(binding_kind, CORE_STRING_STATIC("const"));
    parse_tree_t *list = child_named(ctx->grammar, decl, "sym-decl-list");
    if (!list) return;
    for (size_t i = 0; i < core_tree_num_children(list); i++) {
        parse_tree_t *sym = core_tree_child(list, i);
        if (!is_name(ctx->grammar, sym, "sym-decl")) continue;
        if (!core_tree_num_children(sym)) continue;
        parse_tree_t *id = core_tree_child(sym, 0);
        core_string_t name = leaf_text(ctx, id);
        if (core_string_eq(name, CORE_STRING_STATIC("_"))) continue;
        parse_tree_t *expr = nullptr;
        parse_tree_t *type = child_named(ctx->grammar, sym, "type-spec");
        for (size_t k = 1; k < core_tree_num_children(sym); k++)
            if (is_name(ctx->grammar, core_tree_child(sym, k), "expression"))
                expr = core_tree_child(sym, k);
        pp_binding_t binding = {
            .name = own_string(ctx, core_string_dup(name)),
            .module = own_string(ctx, core_string_dup(
                ctx->unit->module.identifier)),
            .declaration = sym,
        };
        if (is_const && expr) {
            pp_kind_t initializer_kind = const_literal_kind(ctx, expr);
            bool type_matches = initializer_kind != PP_INVALID;
            if (type && type_matches) {
                pp_kind_t declared_kind = primitive_annotation_kind(ctx, type);
                type_matches = declared_kind != PP_INVALID
                    && declared_kind == initializer_kind;
            }
            if (type_matches && literal_initializer_shape(ctx, expr)
                && const_integer_representable(ctx, expr)) {
                pp_value_t value;
                if (eval_expr(ctx, expr, &value)
                    && (value.kind == PP_BOOL || value.kind == PP_INT
                        || value.kind == PP_FLOAT || value.kind == PP_STRING)) {
                    binding.value = value;
                    binding.eligible = true;
                }
            }
        }
        core_list_push(ctx->bindings, binding);
    }
}

static parse_tree_t *
selected_when_body(pp_context_t *ctx, parse_tree_t *when)
{
    parse_tree_t *condition = nullptr;
    parse_tree_t *body = nullptr;
    for (size_t i = 0; i < core_tree_num_children(when); i++) {
        parse_tree_t *child = core_tree_child(when, i);
        if (is_name(ctx->grammar, child, "expression") && !condition)
            condition = child;
        else if (is_name(ctx->grammar, child, "body") && !body)
            body = child;
    }
    if (!condition || !body) return nullptr;
    bool selected;
    if (!evaluate_when_condition(ctx, condition, &selected)) return nullptr;
    if (selected) return body;

    parse_tree_t *else_when_group = core_tree_num_children(when) > 3
        ? core_tree_child(when, 3) : nullptr;
    for (size_t i = 0; else_when_group
            && i < core_tree_num_children(else_when_group); i++) {
        parse_tree_t *clause = core_tree_child(else_when_group, i);
        if (!is_name(ctx->grammar, clause, "else-when-clause")) continue;
        parse_tree_t *clause_condition = child_named(ctx->grammar, clause,
                                                      "expression");
        parse_tree_t *clause_body = child_named(ctx->grammar, clause,
                                                 "body");
        if (!clause_condition || !clause_body) continue;
        if (!evaluate_when_condition(ctx, clause_condition, &selected))
            return nullptr;
        if (selected) return clause_body;
    }

    parse_tree_t *else_group = core_tree_num_children(when) > 4
        ? core_tree_child(when, 4) : nullptr;
    parse_tree_t *else_clause = else_group
        && core_tree_num_children(else_group) == 1
            ? core_tree_child(else_group, 0) : nullptr;
    if (is_name(ctx->grammar, else_clause, "ctime-else"))
        return child_named(ctx->grammar, else_clause, "body");
    return nullptr;
}

bool
n00bb_preprocessor_builtin_name(core_string_t name)
{
    static const char *names[] = {
        "target_os", "target_os_major", "target_os_minor", "target_os_patch",
        "target_arch", "target_alu", "target_is_little_endian",
        "debug_enabled",
        "preproc_utc_year", "preproc_utc_month", "preproc_utc_day",
        "preproc_utc_hour", "preproc_utc_minute", "preproc_utc_second",
        "n00b_major", "n00b_minor", "n00b_patch", "n00b_impl",
        "preproc_env", "preproc_env_defines", "preproc_defines",
        "preproc_value", "compile_warning", "compile_error", nullptr,
    };
    for (size_t i = 0; names[i]; i++)
        if (core_string_eq(name, core_string_from_cstr(names[i]))) return true;
    return false;
}

static bool
evaluate_when_condition(pp_context_t *ctx, parse_tree_t *condition,
                        bool *selected)
{
    if (!validate_expr_forms(ctx, condition)) return false;
    pp_value_t value;
    if (!eval_expr(ctx, condition, &value) || ctx->failed) return false;

    pp_value_t inferred;
    size_t owned_mark = ctx->owned_strings.len;
    ctx->type_only = true;
    bool typed = eval_expr(ctx, condition, &inferred);
    ctx->type_only = false;
    for (size_t i = owned_mark; i < ctx->owned_strings.len; i++)
        core_free(ctx->owned_strings.data[i].data);
    ctx->owned_strings.len = owned_mark;
    if (!typed || ctx->failed) return false;
    if (value.kind != PP_BOOL) {
        pp_error(ctx, condition, "when condition must have static type bool");
        return false;
    }
    *selected = value.as.boolean;
    return true;
}

/* Validate form independently of value evaluation, including skipped operands. */
static bool
validate_expr_forms(pp_context_t *ctx, parse_tree_t *node)
{
    static const char *allowed[] = {
        "expression", "or-expr", "and-expr", "or-rhs", "and-rhs", "equality-expr",
        "compare-expr", "bitor-expr", "bitxor-expr", "bitand-expr",
        "shift-expr", "add-expr", "mul-expr", "unary-expr",
        "postfix-expr", "primary", "literal-expr", "tuple-or-paren",
        "tagged-lit", "literal-prefix", "literal-payload",
        "simple-lit", "bool-lit", "int-lit", "float-lit", "string-lit",
        "call-args",
        "positional-args", "equality-op", "compare-op", "shift-op",
        "add-op", "mul-op", nullptr,
    };
    if (!node || core_tree_is_leaf(node)) return true;
    core_string_t name = node_name(ctx->grammar, node);
    if (!name.data) {
        for (size_t i = 0; i < core_tree_num_children(node); i++)
            if (!validate_expr_forms(ctx, core_tree_child(node, i)))
                return false;
        return true;
    }
    bool permitted = false;
    for (size_t i = 0; allowed[i]; i++) {
        if (core_string_eq(name, core_string_from_cstr(allowed[i]))) {
            permitted = true;
            break;
        }
    }
    if (!permitted) {
        char message[192];
        snprintf(message, sizeof(message),
            "expression form '%.*s' is not allowed in a preprocessor condition",
            (int)name.u8_bytes, name.data);
        pp_error(ctx, node, message);
        return false;
    }

    if (is_name(ctx->grammar, node, "postfix-expr")) {
        parse_tree_t *operator = nullptr;
        for (size_t i = 0; i < core_tree_num_children(node); i++) {
            parse_tree_t *part = core_tree_child(node, i);
            if (!core_tree_is_leaf(part)) continue;
            if (parse_tree_leaf_is(part, CORE_STRING_STATIC("."))
                || parse_tree_leaf_is(part, CORE_STRING_STATIC("("))
                || parse_tree_leaf_is(part, CORE_STRING_STATIC("["))
                || parse_tree_leaf_is(part, CORE_STRING_STATIC("!"))) {
                operator = part;
                break;
            }
        }
        if (operator && core_tree_is_leaf(operator)
            && parse_tree_leaf_is(operator, CORE_STRING_STATIC("."))) {
            core_buffer_t *chain = core_buffer_empty();
            bool valid = collect_name_chain(ctx, node, chain);
            core_buffer_free(chain);
            if (!valid) {
                pp_error(ctx, node,
                         "only qualified const names are allowed as member expressions");
                return false;
            }
        }
        else if (operator && core_tree_is_leaf(operator)
                 && parse_tree_leaf_is(operator,
                                       CORE_STRING_STATIC("("))) {
            core_buffer_t *callee = core_buffer_empty();
            bool valid = core_tree_num_children(node)
                && collect_name_chain(ctx, core_tree_child(node, 0), callee)
                && n00bb_preprocessor_builtin_name(core_string_borrow(callee->data,
                                                       callee->byte_len));
            core_buffer_free(callee);
            if (!valid) {
                pp_error(ctx, node,
                         "only processor-time builtins may be called in a preprocessor condition");
                return false;
            }
        }
        else if (operator && core_tree_is_leaf(operator)
                 && (parse_tree_leaf_is(operator, CORE_STRING_STATIC("["))
                     || parse_tree_leaf_is(operator,
                                           CORE_STRING_STATIC("!")))) {
            pp_error(ctx, node,
                     "member access, indexing, and postfix operators are not allowed in a preprocessor condition");
            return false;
        }
    }
    if (is_name(ctx->grammar, node, "tagged-lit")) {
        core_string_t builtin = builtin_name(ctx, node);
        parse_tree_t *payload = child_named(ctx->grammar, node,
                                             "literal-payload");
        parse_tree_t *tuple = payload
            ? child_named(ctx->grammar, payload, "tuple-or-paren") : nullptr;
        if (!n00bb_preprocessor_builtin_name(builtin) || !tuple
            || core_tree_node_value(tuple).rule_index != 1) {
            pp_error(ctx, node,
                     "literal modifiers are not allowed in a preprocessor condition");
            return false;
        }
    }
    if (is_name(ctx->grammar, node, "unary-expr")) {
        size_t count = core_tree_num_children(node);
        bool allowed = count == 1;
        if (count == 2 && core_tree_is_leaf(core_tree_child(node, 0))) {
            core_string_t op = parse_tree_leaf_text(core_tree_child(node, 0));
            allowed = core_string_eq(op, CORE_STRING_STATIC("!"))
                || core_string_eq(op, CORE_STRING_STATIC("-"))
                || core_string_eq(op, CORE_STRING_STATIC("+"));
        }
        if (!allowed) {
            pp_error(ctx, node,
                     "casts and reference operators are not allowed in a preprocessor condition");
            return false;
        }
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!validate_expr_forms(ctx, core_tree_child(node, i)))
            return false;
    return true;
}

static core_string_t
builtin_name(pp_context_t *ctx, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return (core_string_t){0};
    if (is_name(ctx->grammar, node, "tagged-lit")) {
        parse_tree_t *prefix = child_named(ctx->grammar, node,
                                            "literal-prefix");
        while (prefix && !core_tree_is_leaf(prefix)
               && core_tree_num_children(prefix) == 1)
            prefix = core_tree_child(prefix, 0);
        return prefix ? leaf_text(ctx, prefix) : (core_string_t){0};
    }
    if (is_name(ctx->grammar, node, "postfix-expr")) {
        if (core_tree_num_children(node) < 3
            || !parse_tree_leaf_is(core_tree_child(node, 1),
                                   CORE_STRING_STATIC("(")))
            return (core_string_t){0};
        parse_tree_t *callee = core_tree_child(node, 0);
        while (callee && !core_tree_is_leaf(callee)
               && core_tree_num_children(callee) == 1)
            callee = core_tree_child(callee, 0);
        return callee && core_tree_is_leaf(callee)
            ? leaf_text(ctx, callee) : (core_string_t){0};
    }
    if (core_tree_num_children(node) != 1) return (core_string_t){0};
    return builtin_name(ctx, core_tree_child(node, 0));
}

static core_string_t
format_integer(__int128 value)
{
    char digits[48];
    size_t length = 0;
    bool negative = value < 0;
    unsigned __int128 magnitude = negative
        ? (unsigned __int128)(-(value + 1)) + 1 : (unsigned __int128)value;
    do {
        digits[length++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (negative) digits[length++] = '-';
    for (size_t i = 0; i < length / 2; i++) {
        char temp = digits[i];
        digits[i] = digits[length - i - 1];
        digits[length - i - 1] = temp;
    }
    return core_string_from_raw(digits, (int64_t)length);
}

static core_string_t
format_string_literal(core_string_t value)
{
    core_buffer_t *buffer = core_buffer_empty();
    core_buffer_putc(buffer, '"');
    for (size_t i = 0; i < value.u8_bytes; i++) {
        unsigned char c = (unsigned char)value.data[i];
        if (c == '\\' || c == '"') {
            core_buffer_putc(buffer, '\\');
            core_buffer_putc(buffer, (char)c);
        }
        else if (c == '\n') core_buffer_puts(buffer, "\\n");
        else if (c == '\r') core_buffer_puts(buffer, "\\r");
        else if (c == '\t') core_buffer_puts(buffer, "\\t");
        else if (c == '\0') core_buffer_puts(buffer, "\\0");
        else if (c < 0x20 || c == 0x7f) {
            core_buffer_printf(buffer, "\\x%02x", c);
        }
        else core_buffer_putc(buffer, (char)c);
    }
    core_buffer_putc(buffer, '"');
    size_t length = buffer->byte_len;
    char *data = core_buffer_take(buffer);
    return core_string_borrow(data, length);
}

static void
anchor_synthetic_tokens(parse_tree_t *node, const parse_token_info_t *source)
{
    if (!node) return;
    if (core_tree_is_leaf(node)) {
        parse_token_info_t *token = core_tree_leaf_value(node);
        token->line = source->line;
        token->column = source->column;
        token->endline = source->endline;
        token->endcol = source->endcol;
        token->start_byte = source->start_byte;
        token->end_byte = source->end_byte;
        if (core_option_is_set(source->file)) {
            token->file = core_option_set(core_string_t,
                core_string_dup(core_option_get(source->file)));
            token->owns_file = true;
        }
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        anchor_synthetic_tokens(core_tree_child(node, i), source);
}

static parse_tree_t *
make_value_literal(pp_context_t *ctx, pp_value_t value)
{
    core_string_t text = {0};
    int32_t simple_rule = 0;
    int64_t token_id = 0;
    parse_tree_t *literal = nullptr;
    if (value.kind == PP_BOOL) {
        text = value.as.boolean ? CORE_STRING_STATIC("true")
                                : CORE_STRING_STATIC("false");
        token_id = parse_terminal_id(ctx->grammar, text);
        parse_tree_t *bool_node = parse_tree_make_nt(
            ctx->grammar, CORE_STRING_STATIC("bool-lit"),
            value.as.boolean ? 0 : 1);
        parse_tree_t *token = parse_tree_make_synthetic_token(token_id, text);
        if (!bool_node || !token || !core_tree_add_child(bool_node, token)) {
            parse_tree_free(bool_node);
            parse_tree_free(token);
            return nullptr;
        }
        literal = bool_node;
        simple_rule = 5;
    }
    else if (value.kind == PP_INT) {
        text = format_integer(value.as.integer);
        token_id = parse_terminal_id(ctx->grammar, CORE_STRING_STATIC("INT_LIT"));
        simple_rule = 0;
    }
    else if (value.kind == PP_FLOAT) {
        if (!isfinite(value.as.real)) return nullptr;
        char number[64];
        int length = snprintf(number, sizeof(number), "%.17g", value.as.real);
        if (length <= 0 || (size_t)length >= sizeof(number)) return nullptr;
        if (!strchr(number, '.') && !strchr(number, 'e') && !strchr(number, 'E')) {
            if ((size_t)length + 2 >= sizeof(number)) return nullptr;
            number[length++] = '.';
            number[length++] = '0';
            number[length] = '\0';
        }
        text = core_string_from_raw(number, length);
        token_id = parse_terminal_id(ctx->grammar, CORE_STRING_STATIC("FLOAT_LIT"));
        simple_rule = 2;
    }
    else if (value.kind == PP_STRING) {
        text = format_string_literal(value.as.string);
        token_id = parse_terminal_id(ctx->grammar, CORE_STRING_STATIC("STRING_LIT"));
        simple_rule = 3;
    }
    else return nullptr;

    parse_tree_t *simple = parse_tree_make_nt(
        ctx->grammar, CORE_STRING_STATIC("simple-lit"), simple_rule);
    if (!literal) {
        parse_tree_t *token = parse_tree_make_synthetic_token(token_id, text);
        literal = token;
    }
    if (!simple || !literal || !core_tree_add_child(simple, literal)) {
        parse_tree_free(simple);
        parse_tree_free(literal);
        if (value.kind != PP_BOOL) core_free(text.data);
        return nullptr;
    }
    parse_tree_t *primary = parse_tree_make_nt(
        ctx->grammar, CORE_STRING_STATIC("primary"), 0);
    if (!primary || !core_tree_add_child(primary, simple)) {
        parse_tree_free(primary);
        parse_tree_free(simple);
        if (value.kind != PP_BOOL) core_free(text.data);
        return nullptr;
    }
    if (value.kind != PP_BOOL) core_free(text.data);
    return primary;
}

static bool
replace_builtin(pp_context_t *ctx, parse_tree_t **slot,
                parse_tree_t *parent, int32_t child_index)
{
    parse_tree_t *node = *slot;
    core_string_t name = builtin_name(ctx, node);
    if (!n00bb_preprocessor_builtin_name(name)
        || core_string_eq(name, CORE_STRING_STATIC("compile_warning"))
        || core_string_eq(name, CORE_STRING_STATIC("compile_error")))
        return false;
    pp_value_t value;
    if (!eval_builtin(ctx, node, &value)) return false;
    parse_tree_t *primary = make_value_literal(ctx, value);
    if (!primary) {
        pp_error(ctx, node, "cannot materialize processor-time result as a literal");
        return false;
    }
    parse_token_info_t *source = parse_tree_find_first_terminal(node);
    if (source) {
        parse_tree_t *last = last_node(node);
        parse_token_info_t *end = parse_node_token(last);
        parse_token_info_t span = *source;
        if (end) {
            span.endline = end->endline;
            span.endcol = end->endcol;
            span.end_byte = end->end_byte;
        }
        anchor_synthetic_tokens(primary, &span);
    }
    if (is_name(ctx->grammar, node, "primary")) {
        parse_tree_t *old = core_tree_remove_child(node, 0);
        parse_tree_free(old);
        core_tree_node_value(node).rule_index = 0;
        parse_tree_t *simple = core_tree_remove_child(primary, 0);
        parse_tree_free(primary);
        if (!core_tree_add_child(node, simple)) {
            parse_tree_free(simple);
            pp_error(ctx, node, "cannot rewrite processor-time literal");
            return false;
        }
        return true;
    }
    int32_t primary_rule = parse_grammar_single_child_rule(ctx->grammar,
        CORE_STRING_STATIC("postfix-expr"), CORE_STRING_STATIC("primary"));
    parse_tree_t *postfix = primary_rule >= 0
        ? parse_tree_make_nt(ctx->grammar,
            CORE_STRING_STATIC("postfix-expr"), primary_rule) : nullptr;
    if (!postfix || !core_tree_add_child(postfix, primary)) {
        parse_tree_free(postfix);
        parse_tree_free(primary);
        pp_error(ctx, node, "cannot rewrite processor-time literal");
        return false;
    }
    if (parent && child_index >= 0
        && !core_tree_set_child(parent, (size_t)child_index, postfix)) {
        parse_tree_free(postfix);
        pp_error(ctx, node, "cannot attach processor-time result to its parent");
        return false;
    }
    *slot = postfix;
    parse_tree_free(node);
    return true;
}

static size_t
module_state_index(pp_context_t *ctx, core_translation_unit_t *unit)
{
    for (size_t i = 0; i < ctx->modules.len; i++)
        if (core_module_spec_eq(&ctx->modules.data[i].unit->module,
                                &unit->module))
            return i;
    return SIZE_MAX;
}

static void
mark_cycle_backedge(pp_context_t *ctx, size_t target)
{
    for (size_t i = ctx->module_stack.len; i > 0; i--) {
        size_t index = ctx->module_stack.data[i - 1];
        ctx->modules.data[index].cyclic = true;
        if (index == target) return;
    }
}

static bool
process_import(pp_context_t *ctx, parse_tree_t *statement)
{
    if (!ctx->options || !ctx->options->load_import) {
        pp_error(ctx, statement, "no module loader is configured for imports");
        return false;
    }
    core_translation_unit_t *imported = nullptr;
    parse_tree_t *tree = nullptr;
    core_diagnostics_t *diags = core_translation_unit_diagnostics(ctx->unit);
    size_t diagnostic_count = core_diagnostics_count(diags);
    if (!ctx->options->load_import(ctx->options->import_context,
            ctx->grammar, statement, ctx->unit, &imported, &tree)
        || !imported || !tree) {
        if (core_diagnostics_count(diags) == diagnostic_count)
            pp_error(ctx, statement, "cannot load imported module");
        else ctx->failed = true;
        return false;
    }
    if (!core_translation_unit_add_import(ctx->unit, imported)) {
        pp_error(ctx, statement, "cannot record imported module reference");
        return false;
    }
    size_t index = module_state_index(ctx, imported);
    if (index != SIZE_MAX
        && ctx->modules.data[index].visit == PP_MODULE_VISITING) {
        mark_cycle_backedge(ctx, index);
        return true;
    }
    bool succeeded = process_module(ctx, imported, tree);
    if (!succeeded) ctx->failed = true;
    return succeeded;
}

static void
process_node(pp_context_t *ctx, parse_tree_t **slot, parse_tree_t *parent,
             int32_t depth, int32_t child_index,
             parse_tree_t *selected_import_body,
             parse_tree_t *current_body)
{
    parse_tree_t *node = *slot;
    if (!node || core_tree_is_leaf(node) || ctx->failed) return;
    if (is_name(ctx->grammar, node, "body")) current_body = node;

    bool scoped_body = is_name(ctx->grammar, node, "body")
        || is_name(ctx->grammar, node, "value-body")
        || is_name(ctx->grammar, node, "try-body");
    size_t binding_mark = ctx->bindings.len;

    if (is_name(ctx->grammar, node, "import-stmt")) {
        if (current_body && current_body != selected_import_body) {
            pp_error(ctx, node,
                "imports are only allowed at module level or directly in a selected when branch");
            return;
        }
        process_import(ctx, node);
        return;
    }

    if (is_name(ctx->grammar, node, "assert-stmt")
        && core_string_eq(first_leaf_text(ctx, node),
                          CORE_STRING_STATIC("assert"))
        && ctx->options && ctx->options->release_mode) {
        *slot = nullptr;
        return;
    }

    if (is_name(ctx->grammar, node, "expression-stmt")) {
        parse_tree_t *expression = child_named(
            ctx->grammar, node, "expression");
        core_string_t diagnostic_name = {0};
        while (expression && !core_tree_is_leaf(expression)) {
            diagnostic_name = builtin_name(ctx, expression);
            if (core_string_eq(diagnostic_name,
                               CORE_STRING_STATIC("compile_warning"))
                || core_string_eq(diagnostic_name,
                                  CORE_STRING_STATIC("compile_error")))
                break;
            if (core_tree_num_children(expression) != 1) break;
            expression = core_tree_child(expression, 0);
        }
        if (core_string_eq(diagnostic_name,
                           CORE_STRING_STATIC("compile_warning"))
            || core_string_eq(diagnostic_name,
                              CORE_STRING_STATIC("compile_error"))) {
            pp_value_t ignored;
            eval_builtin(ctx, expression, &ignored);
            *slot = nullptr;
            return;
        }
    }

    if (is_name(ctx->grammar, node, "variable-decl")) {
        remember_const(ctx, node);
        if (ctx->failed) return;
    }
    if (is_name(ctx->grammar, node, "when-stmt")) {
        parse_tree_t *body = selected_when_body(ctx, node);
        if (ctx->failed) return;
        if (body) {
            parse_tree_t *replacement = parse_tree_clone(body);
            if (!replacement) {
                pp_error(ctx, node, "cannot copy the selected when body");
                return;
            }
            if (parent && child_index >= 0
                && !core_tree_set_child(parent, (size_t)child_index,
                                        replacement)) {
                parse_tree_free(replacement);
                pp_error(ctx, node, "cannot attach the selected when body");
                return;
            }
            *slot = replacement;
            parse_tree_free(node);
            process_node(ctx, slot, parent, depth, child_index, replacement,
                         replacement);
        }
        else {
            *slot = nullptr;
        }
        return;
    }
    bool invocation = is_name(ctx->grammar, node, "tagged-lit")
        || (is_name(ctx->grammar, node, "postfix-expr")
            && postfix_has_token(node, "("));
    core_string_t processor_name = invocation ? builtin_name(ctx, node)
                                               : (core_string_t){0};
    if (core_string_eq(processor_name, CORE_STRING_STATIC("compile_warning"))
        || core_string_eq(processor_name, CORE_STRING_STATIC("compile_error"))) {
        pp_value_t ignored;
        eval_builtin(ctx, node, &ignored);
        if (ctx->failed) return;
    }
    if (replace_builtin(ctx, slot, parent, child_index)) node = *slot;
    if (ctx->failed || !node) return;
    for (size_t i = 0; i < core_tree_num_children(node);) {
        parse_tree_t *child = core_tree_child(node, i);
        process_node(ctx, &child, node, depth + 1, (int32_t)i,
                     selected_import_body, current_body);
        if (!child) {
            parse_tree_t *removed = core_tree_remove_child(node, i);
            parse_tree_free(removed);
        }
        else {
            core_tree_set_child(node, i, child);
            i++;
        }
    }
    if (!core_tree_num_children(node)
        && (is_name(ctx->grammar, node, "top-level-stmt")
            || is_name(ctx->grammar, node, "body-stmt")
            || is_name(ctx->grammar, node, "expression-stmt")))
        *slot = nullptr;
    if (scoped_body) ctx->bindings.len = binding_mark;
}

static void
process_module_statements(pp_context_t *ctx, parse_tree_t *node,
                          parse_tree_t *parent,
                          int32_t depth, int32_t child_index,
                          core_list_t(pp_deferred_stmt_t) *imports,
                          core_list_t(pp_deferred_stmt_t) *deferred)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (is_name(ctx->grammar, node, "top-level-stmt")) {
        parse_tree_t *import = child_named(ctx->grammar, node, "import-stmt");
        if (import) {
            if (ctx->options && ctx->options->prepare_import
                && !ctx->options->prepare_import(
                    ctx->options->import_context, ctx->grammar, import,
                    ctx->unit)) {
                ctx->failed = true;
                return;
            }
            if (parent)
                core_list_push(*imports, ((pp_deferred_stmt_t){
                    .parent = parent, .statement = node, .depth = depth,
                    .child_index = child_index,
                }));
        }
        else {
            if (parent)
                core_list_push(*deferred, ((pp_deferred_stmt_t){
                    .parent = parent, .statement = node, .depth = depth,
                    .child_index = child_index,
                }));
        }
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        process_module_statements(ctx, core_tree_child(node, i), node,
                                  depth + 1, (int32_t)i, imports, deferred);
}

static bool
process_deferred_statement(pp_context_t *ctx, pp_deferred_stmt_t deferred)
{
    for (size_t i = 0; i < core_tree_num_children(deferred.parent); i++) {
        if (core_tree_child(deferred.parent, i) != deferred.statement) continue;
        parse_tree_t *statement = deferred.statement;
        process_node(ctx, &statement, deferred.parent, deferred.depth,
                     (int32_t)i, nullptr, nullptr);
        if (!statement) {
            parse_tree_free(core_tree_remove_child(deferred.parent, i));
        }
        else core_tree_set_child(deferred.parent, i, statement);
        return true;
    }
    return false;
}

static bool
process_module(pp_context_t *ctx, core_translation_unit_t *unit,
               parse_tree_t *tree)
{
    size_t index = module_state_index(ctx, unit);
    if (index != SIZE_MAX) {
        pp_module_t *state = &ctx->modules.data[index];
        if (state->visit == PP_MODULE_VISITING) {
            mark_cycle_backedge(ctx, index);
            return true;
        }
        return state->succeeded;
    }
    core_list_push(ctx->modules, ((pp_module_t){
        .unit = unit, .tree = tree, .visit = PP_MODULE_VISITING,
    }));
    index = ctx->modules.len - 1;
    core_list_push(ctx->module_stack, index);

    core_translation_unit_t *previous_unit = ctx->unit;
    bool previous_cycle_warning = ctx->cycle_warning_emitted;
    ctx->unit = unit;
    ctx->cycle_warning_emitted = false;
    core_list_t(pp_deferred_stmt_t) imports =
        core_list_new(pp_deferred_stmt_t);
    core_list_t(pp_deferred_stmt_t) deferred =
        core_list_new(pp_deferred_stmt_t);
    process_module_statements(ctx, tree, nullptr, 0, -1, &imports, &deferred);
    for (size_t i = 0; i < imports.len && !ctx->failed; i++)
        if (!process_deferred_statement(ctx, imports.data[i])) {
            pp_error(ctx, tree, "module import tree changed during preprocessing");
            break;
        }
    for (size_t i = 0; i < deferred.len && !ctx->failed; i++)
        if (!process_deferred_statement(ctx, deferred.data[i])) {
            pp_error(ctx, tree, "module statement tree changed during preprocessing");
            break;
        }
    core_list_free(imports);
    core_list_free(deferred);
    bool succeeded = !ctx->failed
        && !core_diagnostics_failed(core_translation_unit_diagnostics(unit));
    if (!succeeded) ctx->failed = true;
    ctx->modules.data[index].tree = tree;
    ctx->modules.data[index].visit = PP_MODULE_DONE;
    ctx->modules.data[index].succeeded = succeeded;
    ctx->module_stack.len--;
    ctx->unit = previous_unit;
    ctx->cycle_warning_emitted = previous_cycle_warning;
    return succeeded;
}

bool
n00bb_preprocessor_run(n00bb_preprocessor_t *preprocessor,
                       parse_grammar_t *grammar, parse_tree_t *tree,
                       core_translation_unit_t *unit)
{
    if (!preprocessor || !grammar || !tree || !unit) return false;
    pp_context_t *ctx = &preprocessor->context;
    ctx->grammar = grammar;
    if (!ctx->module_stack.len) {
        ctx->cycle_warning_emitted = false;
        ctx->failed = false;
    }
    time_t now = time(nullptr);
    struct tm *utc = gmtime(&now);
    if (utc && !ctx->time_initialized) {
        ctx->utc_time = *utc;
        ctx->time_initialized = true;
    }
    return process_module(ctx, unit, tree);
}

n00bb_preprocessor_t *
n00bb_preprocessor_new(const n00bb_preprocessor_options_t *options)
{
    n00bb_preprocessor_t *preprocessor = core_alloc(n00bb_preprocessor_t);
    preprocessor->context.bindings = core_list_new(pp_binding_t);
    preprocessor->context.owned_strings = core_list_new(core_string_t);
    preprocessor->context.modules = core_list_new(pp_module_t);
    preprocessor->context.module_stack = core_list_new(size_t);
    preprocessor->context.options = options;
    return preprocessor;
}

void
n00bb_preprocessor_free(n00bb_preprocessor_t *preprocessor)
{
    if (!preprocessor) return;
    pp_context_t *ctx = &preprocessor->context;
    for (size_t i = 0; i < ctx->owned_strings.len; i++)
        core_free(ctx->owned_strings.data[i].data);
    core_list_free(ctx->owned_strings);
    core_list_free(ctx->bindings);
    core_list_free(ctx->modules);
    core_list_free(ctx->module_stack);
    core_free(preprocessor);
}

bool
n00bb_preprocess(parse_grammar_t *grammar, parse_tree_t *tree,
                 core_translation_unit_t *unit,
                 const n00bb_preprocessor_options_t *options)
{
    n00bb_preprocessor_t *preprocessor = n00bb_preprocessor_new(options);
    bool success = n00bb_preprocessor_run(preprocessor, grammar, tree, unit);
    n00bb_preprocessor_free(preprocessor);
    return success;
}
