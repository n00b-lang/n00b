#include "scanner/lex.h"
#include "private/parse/grammar.h"
#include "private/scanner/lex_program.h"
#include "private/scanner/unicode_profile.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEX_IMAGE_MAGIC UINT64_C(0x4c414e544c455849)
#define LEX_IMAGE_FORMAT UINT32_C(1)
#define LEX_IMAGE_ENDIAN UINT32_C(0x01020304)

typedef struct {
    uint64_t magic;
    uint32_t format;
    uint32_t endian;
    uint32_t pointer_bytes;
    uint32_t size_bytes;
    uint64_t abi;
    uint64_t grammar;
    uint64_t payload_bytes;
    uint64_t root_offset;
    uint64_t relocation_count;
    uint64_t checksum;
} lex_image_header_t;

typedef struct {
    core_buffer_t *payload;
    core_dict_t    objects;
    size_t        *relocations;
    size_t         relocation_count;
    size_t         relocation_capacity;
    bool           valid;
} lex_image_build_t;

static _Thread_local char image_error[256];

static uint64_t
hash_bytes(uint64_t hash, const void *data, size_t length)
{
    const uint8_t *bytes = data;

    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t
hash_header(uint64_t hash, const lex_image_header_t *header)
{
#define HASH_FIELD(field)                                                      \
    hash = hash_bytes(hash, &header->field, sizeof(header->field))
    HASH_FIELD(magic);
    HASH_FIELD(format);
    HASH_FIELD(endian);
    HASH_FIELD(pointer_bytes);
    HASH_FIELD(size_bytes);
    HASH_FIELD(abi);
    HASH_FIELD(grammar);
    HASH_FIELD(payload_bytes);
    HASH_FIELD(root_offset);
    HASH_FIELD(relocation_count);
#undef HASH_FIELD
    return hash;
}

static int
int64_compare(const void *left, const void *right)
{
    int64_t a = *(const int64_t *)left;
    int64_t b = *(const int64_t *)right;
    return (a > b) - (a < b);
}

static uint64_t
image_abi(void)
{
    const size_t sizes[] = {
        sizeof(scan_lex_t), sizeof(scan_lex_spec_t),
        sizeof(scan_lex_comment_t), sizeof(scan_lex_string_t),
        sizeof(scan_lex_escape_t), sizeof(scan_lex_radix_t),
        sizeof(scan_lex_suffix_t), sizeof(scan_lex_number_form_t),
        sizeof(scan_lex_sigil_t), sizeof(scan_lex_regex_t),
        sizeof(scan_lex_heredoc_header_t), sizeof(scan_lex_heredoc_t),
        sizeof(scan_lex_embed_t), sizeof(scan_lex_fence_t),
        sizeof(scan_lex_directive_t), sizeof(scan_lex_literal_hook_t),
        sizeof(scan_lex_recovery_t),
        sizeof(scan_lex_declaration_t),
        sizeof(scan_lex_issue_t),
        sizeof(lex_term_node_t), sizeof(lex_term_edge_t),
        sizeof(lex_bucket_t), sizeof(lex_ascii_slot_t),
        sizeof(lex_tid_set_t), sizeof(lex_literal_hook_t),
        sizeof(lex_recovery_t),
    };

    return hash_bytes(UINT64_C(1469598103934665603), sizes, sizeof(sizes));
}

static uint64_t
grammar_fingerprint(const parse_grammar_t *grammar)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    if (!grammar) {
        return 0;
    }
    for (size_t i = 0; i < grammar->named_terms.len; i++) {
        const parse_terminal_t *term = &grammar->named_terms.data[i];
        hash = hash_bytes(hash, &term->id, sizeof(term->id));
        hash = hash_bytes(hash, &term->value.u8_bytes,
                          sizeof(term->value.u8_bytes));
        hash = hash_bytes(hash, term->value.data, term->value.u8_bytes);
    }

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
    int64_t *codepoints = core_alloc_array(int64_t,
                                            codepoint_count
                                                ? codepoint_count : 1);
    size_t at = 0;
    for (size_t i = 0; i < grammar->rules.len; i++) {
        const parse_rule_t *rule = &grammar->rules.data[i];
        for (size_t j = 0; j < rule->contents.len; j++) {
            const parse_match_t *match = &rule->contents.data[j];
            if (match->kind == PARSE_MATCH_TERMINAL
                && match->terminal_id > 0
                && match->terminal_id < PARSE_TOK_START_ID) {
                codepoints[at++] = match->terminal_id;
            }
        }
    }
    qsort(codepoints, at, sizeof(*codepoints), int64_compare);
    for (size_t i = 0; i < at; i++) {
        if (!i || codepoints[i] != codepoints[i - 1]) {
            hash = hash_bytes(hash, &codepoints[i], sizeof(codepoints[i]));
        }
    }
    core_free(codepoints);
    return hash;
}

static size_t
align_offset(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static bool
object_offset(lex_image_build_t *build, const void *object, size_t *offset)
{
    bool found = false;
    void *encoded = core_dict_get(&build->objects, (void *)object, &found);

    if (found && offset) {
        *offset = (size_t)(uintptr_t)encoded - 1;
    }
    return found;
}

static void
add_alias(lex_image_build_t *build, const void *object, size_t offset)
{
    if (object && !object_offset(build, object, nullptr)) {
        core_dict_put(&build->objects, (void *)object,
                      (void *)(uintptr_t)(offset + 1));
    }
}

static size_t
add_object(lex_image_build_t *build, const void *object, size_t bytes,
           size_t alignment)
{
    size_t have;

    if (!object) {
        return SIZE_MAX;
    }
    if (object_offset(build, object, &have)) {
        return have;
    }

    size_t offset = align_offset(build->payload->byte_len, alignment);
    size_t padding = offset - build->payload->byte_len;
    core_buffer_ensure(build->payload, padding + bytes + 1);
    memset(build->payload->data + build->payload->byte_len, 0,
           padding + bytes);
    build->payload->byte_len = offset + bytes;
    build->payload->data[build->payload->byte_len] = '\0';
    if (bytes) {
        memcpy(build->payload->data + offset, object, bytes);
    }
    add_alias(build, object, offset);
    return offset;
}

static void
add_relocation(lex_image_build_t *build, size_t slot)
{
    if (build->relocation_count == build->relocation_capacity) {
        size_t capacity = build->relocation_capacity
                        ? build->relocation_capacity * 2 : 128;
        build->relocations = core_realloc(
            build->relocations, capacity * sizeof(*build->relocations));
        build->relocation_capacity = capacity;
    }
    build->relocations[build->relocation_count++] = slot;
}

static int
relocation_compare(const void *left, const void *right)
{
    size_t a = *(const size_t *)left;
    size_t b = *(const size_t *)right;

    return (a > b) - (a < b);
}

static void
canonicalize_relocations(lex_image_build_t *build)
{
    if (build->relocation_count < 2) {
        return;
    }
    qsort(build->relocations, build->relocation_count,
          sizeof(*build->relocations), relocation_compare);

    size_t out = 1;
    for (size_t i = 1; i < build->relocation_count; i++) {
        if (build->relocations[i] != build->relocations[out - 1]) {
            build->relocations[out++] = build->relocations[i];
        }
    }
    build->relocation_count = out;
}

static void
relocate_to(lex_image_build_t *build, size_t slot, size_t target)
{
    if (target == SIZE_MAX || slot + sizeof(uintptr_t) > build->payload->byte_len) {
        build->valid = false;
        return;
    }
    uintptr_t encoded = (uintptr_t)(target + 1);
    memcpy(build->payload->data + slot, &encoded, sizeof(encoded));
    add_relocation(build, slot);
}

static size_t
snapshot_string(lex_image_build_t *build, core_string_t string)
{
    if (!string.data) {
        return SIZE_MAX;
    }
    size_t offset;
    if (object_offset(build, string.data, &offset)) {
        return offset;
    }
    offset = build->payload->byte_len;
    core_buffer_ensure(build->payload, string.u8_bytes + 2);
    if (string.u8_bytes) {
        memcpy(build->payload->data + offset, string.data, string.u8_bytes);
    }
    build->payload->data[offset + string.u8_bytes] = '\0';
    build->payload->byte_len += string.u8_bytes + 1;
    build->payload->data[build->payload->byte_len] = '\0';
    add_alias(build, string.data, offset);
    return offset;
}

static void
snapshot_string_field(lex_image_build_t *build, size_t object,
                      size_t field, core_string_t string)
{
    if (string.data) {
        relocate_to(build, object + field + offsetof(core_string_t, data),
                    snapshot_string(build, string));
    }
}

static size_t
string_array_count(const core_string_t *items)
{
    size_t count = 0;
    if (items) {
        do {
            count++;
        } while (items[count - 1].data);
    }
    return count;
}

static size_t
snapshot_string_array(lex_image_build_t *build, const core_string_t *items)
{
    size_t count = string_array_count(items);
    if (!count) {
        return SIZE_MAX;
    }
    size_t offset = add_object(build, items, count * sizeof(*items),
                               _Alignof(core_string_t));
    for (size_t i = 0; i + 1 < count; i++) {
        add_alias(build, &items[i], offset + i * sizeof(*items));
        snapshot_string_field(build, offset + i * sizeof(*items), 0, items[i]);
    }
    return offset;
}

const char *
scan_lex_image_error(void)
{
    return image_error[0] ? image_error : nullptr;
}

static size_t
snapshot_array(lex_image_build_t *build, const void *items, size_t count,
               size_t width, size_t alignment)
{
    if (!items || !count) {
        return SIZE_MAX;
    }
    size_t offset = add_object(build, items, count * width, alignment);
    for (size_t i = 0; i < count; i++) {
        add_alias(build, (const char *)items + i * width, offset + i * width);
    }
    return offset;
}

#define COUNT_RECIPES(ptr, field)                                             \
    ({                                                                         \
        size_t _n = 0;                                                         \
        if (ptr) {                                                             \
            do { _n++; } while ((ptr)[_n - 1].field.data);                    \
        }                                                                      \
        _n;                                                                    \
    })

#define SNAP_STRING(type, field)                                              \
    snapshot_string_field(build, item_offset, offsetof(type, field), item->field)

static size_t snapshot_escapes(lex_image_build_t *, const scan_lex_escape_t *);
static size_t snapshot_suffixes(lex_image_build_t *, const scan_lex_suffix_t *);
static void clear_pointer(lex_image_build_t *, size_t);

static size_t
snapshot_comments(lex_image_build_t *build, const scan_lex_comment_t *items)
{
    size_t count = COUNT_RECIPES(items, opener);
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(scan_lex_comment_t));
    for (size_t i = 0; i + 1 < count; i++) {
        const scan_lex_comment_t *item = &items[i];
        size_t item_offset = offset + i * sizeof(*items);
        SNAP_STRING(scan_lex_comment_t, opener);
        SNAP_STRING(scan_lex_comment_t, closer);
        SNAP_STRING(scan_lex_comment_t, fill);
        SNAP_STRING(scan_lex_comment_t, tok);
    }
    return offset;
}

static size_t
snapshot_escapes(lex_image_build_t *build, const scan_lex_escape_t *items)
{
    size_t count = COUNT_RECIPES(items, prefix);
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(scan_lex_escape_t));
    for (size_t i = 0; i + 1 < count; i++) {
        const scan_lex_escape_t *item = &items[i];
        size_t item_offset = offset + i * sizeof(*items);
        SNAP_STRING(scan_lex_escape_t, prefix);
        SNAP_STRING(scan_lex_escape_t, digits);
    }
    return offset;
}

static size_t
snapshot_strings(lex_image_build_t *build, const scan_lex_string_t *items)
{
    size_t count = COUNT_RECIPES(items, opener);
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(scan_lex_string_t));
    for (size_t i = 0; i + 1 < count; i++) {
        const scan_lex_string_t *item = &items[i];
        size_t item_offset = offset + i * sizeof(*items);
        SNAP_STRING(scan_lex_string_t, opener);
        SNAP_STRING(scan_lex_string_t, closer);
        SNAP_STRING(scan_lex_string_t, nest_open);
        SNAP_STRING(scan_lex_string_t, escape);
        SNAP_STRING(scan_lex_string_t, interp_open);
        SNAP_STRING(scan_lex_string_t, interp_close);
        SNAP_STRING(scan_lex_string_t, tok);
        SNAP_STRING(scan_lex_string_t, tok_mid);
        SNAP_STRING(scan_lex_string_t, tok_end);
        SNAP_STRING(scan_lex_string_t, simple_escapes);
        if (item->escape_rules) {
            relocate_to(build,
                        item_offset + offsetof(scan_lex_string_t, escape_rules),
                        snapshot_escapes(build, item->escape_rules));
        }
    }
    return offset;
}

static size_t
snapshot_radixes(lex_image_build_t *build, const scan_lex_radix_t *items)
{
    size_t count = COUNT_RECIPES(items, prefix);
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(scan_lex_radix_t));
    for (size_t i = 0; i + 1 < count; i++) {
        const scan_lex_radix_t *item = &items[i];
        size_t item_offset = offset + i * sizeof(*items);
        SNAP_STRING(scan_lex_radix_t, prefix);
        SNAP_STRING(scan_lex_radix_t, digits);
        SNAP_STRING(scan_lex_radix_t, tok);
    }
    return offset;
}

static size_t
snapshot_suffixes(lex_image_build_t *build, const scan_lex_suffix_t *items)
{
    size_t count = COUNT_RECIPES(items, suffix);
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(scan_lex_suffix_t));
    for (size_t i = 0; i + 1 < count; i++) {
        const scan_lex_suffix_t *item = &items[i];
        size_t item_offset = offset + i * sizeof(*items);
        SNAP_STRING(scan_lex_suffix_t, suffix);
        SNAP_STRING(scan_lex_suffix_t, tok);
    }
    return offset;
}

static size_t
snapshot_number_forms(lex_image_build_t *build,
                      const scan_lex_number_form_t *items)
{
    size_t count = COUNT_RECIPES(items, digits);
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(scan_lex_number_form_t));
    for (size_t i = 0; i + 1 < count; i++) {
        const scan_lex_number_form_t *item = &items[i];
        size_t item_offset = offset + i * sizeof(*items);
        SNAP_STRING(scan_lex_number_form_t, prefix);
        SNAP_STRING(scan_lex_number_form_t, digits);
        SNAP_STRING(scan_lex_number_form_t, separator);
        SNAP_STRING(scan_lex_number_form_t, exponent);
        SNAP_STRING(scan_lex_number_form_t, exponent_digits);
        SNAP_STRING(scan_lex_number_form_t, sign);
        SNAP_STRING(scan_lex_number_form_t, tok_int);
        SNAP_STRING(scan_lex_number_form_t, tok_float);
        if (item->suffixes) {
            relocate_to(build,
                        item_offset + offsetof(scan_lex_number_form_t, suffixes),
                        snapshot_suffixes(build, item->suffixes));
        }
    }
    return offset;
}

#define SIMPLE_RECIPE_SNAPSHOT(fn, type, sentinel, BODY)                      \
static size_t fn(lex_image_build_t *build, const type *items)                 \
{                                                                             \
    size_t count = COUNT_RECIPES(items, sentinel);                            \
    size_t offset = snapshot_array(build, items, count, sizeof(*items),       \
                                   _Alignof(type));                            \
    for (size_t i = 0; i + 1 < count; i++) {                                  \
        const type *item = &items[i];                                          \
        size_t item_offset = offset + i * sizeof(*items);                      \
        BODY                                                                   \
    }                                                                         \
    return offset;                                                            \
}

SIMPLE_RECIPE_SNAPSHOT(snapshot_sigils, scan_lex_sigil_t, prefix,
    SNAP_STRING(scan_lex_sigil_t, prefix);
    SNAP_STRING(scan_lex_sigil_t, tok);)

static size_t
snapshot_regexes(lex_image_build_t *build, const scan_lex_regex_t *items)
{
    size_t count = COUNT_RECIPES(items, opener);
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(scan_lex_regex_t));
    for (size_t i = 0; i + 1 < count; i++) {
        const scan_lex_regex_t *item = &items[i];
        size_t item_offset = offset + i * sizeof(*items);
        SNAP_STRING(scan_lex_regex_t, opener);
        SNAP_STRING(scan_lex_regex_t, closer);
        SNAP_STRING(scan_lex_regex_t, suffix_chars);
        SNAP_STRING(scan_lex_regex_t, tok);
        if (item->not_after) {
            relocate_to(build,
                        item_offset + offsetof(scan_lex_regex_t, not_after),
                        snapshot_string_array(build, item->not_after));
        }
    }
    return offset;
}

static void
snapshot_heredoc_header_fields(lex_image_build_t *build, size_t item_offset,
                               const scan_lex_heredoc_header_t *item)
{
    SNAP_STRING(scan_lex_heredoc_header_t, name);
    SNAP_STRING(scan_lex_heredoc_header_t, tag_start_extra);
    SNAP_STRING(scan_lex_heredoc_header_t, tag_continue_extra);
}

SIMPLE_RECIPE_SNAPSHOT(snapshot_heredoc_headers,
                       scan_lex_heredoc_header_t, name,
    snapshot_heredoc_header_fields(build, item_offset, item);)

static size_t
snapshot_heredocs(lex_image_build_t *build, const scan_lex_heredoc_t *items)
{
    size_t count = COUNT_RECIPES(items, opener);
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(scan_lex_heredoc_t));
    for (size_t i = 0; i + 1 < count; i++) {
        const scan_lex_heredoc_t *item = &items[i];
        size_t item_offset = offset + i * sizeof(*items);
        SNAP_STRING(scan_lex_heredoc_t, opener);
        SNAP_STRING(scan_lex_heredoc_t, tok);
        SNAP_STRING(scan_lex_heredoc_t, interpolated_tok);
        size_t header_offset = item_offset + offsetof(scan_lex_heredoc_t, header);
        snapshot_heredoc_header_fields(build, header_offset, &item->header);
    }
    return offset;
}

SIMPLE_RECIPE_SNAPSHOT(snapshot_embeds, scan_lex_embed_t, open,
    SNAP_STRING(scan_lex_embed_t, open);
    SNAP_STRING(scan_lex_embed_t, close);
    SNAP_STRING(scan_lex_embed_t, tok);
    SNAP_STRING(scan_lex_embed_t, open_tok);
    SNAP_STRING(scan_lex_embed_t, close_tok);
    SNAP_STRING(scan_lex_embed_t, lexer);)

SIMPLE_RECIPE_SNAPSHOT(snapshot_fences, scan_lex_fence_t, outer_open,
    SNAP_STRING(scan_lex_fence_t, outer_open);
    SNAP_STRING(scan_lex_fence_t, outer_close);
    SNAP_STRING(scan_lex_fence_t, fill);
    SNAP_STRING(scan_lex_fence_t, body_open);
    SNAP_STRING(scan_lex_fence_t, body_close);
    SNAP_STRING(scan_lex_fence_t, tok);
    SNAP_STRING(scan_lex_fence_t, pad_chars);)

SIMPLE_RECIPE_SNAPSHOT(snapshot_directives, scan_lex_directive_t, open,
    SNAP_STRING(scan_lex_directive_t, open);
    SNAP_STRING(scan_lex_directive_t, tok_eol);
    SNAP_STRING(scan_lex_directive_t, tok_other);)

static size_t
snapshot_literal_hooks(lex_image_build_t *build,
                       const scan_lex_literal_hook_t *items)
{
    size_t count = COUNT_RECIPES(items, token);
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(scan_lex_literal_hook_t));
    for (size_t i = 0; i + 1 < count; i++) {
        const scan_lex_literal_hook_t *item = &items[i];
        size_t item_offset = offset + i * sizeof(*items);
        SNAP_STRING(scan_lex_literal_hook_t, token);
        SNAP_STRING(scan_lex_literal_hook_t, hook);
        SNAP_STRING(scan_lex_literal_hook_t, modifier);
        if (item->outputs) {
            relocate_to(build,
                        item_offset + offsetof(scan_lex_literal_hook_t, outputs),
                        snapshot_string_array(build, item->outputs));
        }
        if (item->types) {
            relocate_to(build,
                        item_offset + offsetof(scan_lex_literal_hook_t, types),
                        snapshot_string_array(build, item->types));
        }
    }
    return offset;
}

static size_t
snapshot_recoveries(lex_image_build_t *build,
                    const scan_lex_recovery_t *items)
{
    size_t count = COUNT_RECIPES(items, token);
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(scan_lex_recovery_t));
    for (size_t i = 0; i + 1 < count; i++) {
        const scan_lex_recovery_t *item = &items[i];
        size_t item_offset = offset + i * sizeof(*items);
        SNAP_STRING(scan_lex_recovery_t, token);
        SNAP_STRING(scan_lex_recovery_t, replacement);
        if (item->synchronize) {
            relocate_to(build,
                        item_offset
                            + offsetof(scan_lex_recovery_t, synchronize),
                        snapshot_string_array(build, item->synchronize));
        }
    }
    return offset;
}

static size_t
snapshot_declarations(lex_image_build_t *build,
                      const scan_lex_declaration_t *items)
{
    size_t count = 0;
    while (items[count].kind != SCAN_LEX_DECL_NONE) {
        count++;
    }
    count++;
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(scan_lex_declaration_t));
    for (size_t i = 0; i + 1 < count; i++) {
        snapshot_string_field(build, offset + i * sizeof(*items),
                              offsetof(scan_lex_declaration_t, spelling),
                              items[i].spelling);
        snapshot_string_field(build, offset + i * sizeof(*items),
                              offsetof(scan_lex_declaration_t, origin),
                              items[i].origin);
    }
    return offset;
}

static size_t
snapshot_spec(lex_image_build_t *build, const scan_lex_spec_t *spec)
{
    size_t existing;
    if (object_offset(build, spec, &existing)) {
        return existing;
    }
    size_t offset = add_object(build, spec, sizeof(*spec),
                               _Alignof(scan_lex_spec_t));

#define SPEC_STRING(field)                                                     \
    snapshot_string_field(build, offset, offsetof(scan_lex_spec_t, field),    \
                          spec->field)
#define SPEC_ARRAY(field, fn)                                                  \
    do {                                                                       \
        if (spec->field) {                                                     \
            relocate_to(build, offset + offsetof(scan_lex_spec_t, field),     \
                        fn(build, spec->field));                               \
        }                                                                      \
    } while (0)

    SPEC_STRING(name);
    SPEC_ARRAY(comments, snapshot_comments);
    SPEC_ARRAY(strings, snapshot_strings);
    SPEC_ARRAY(sigils, snapshot_sigils);
    SPEC_ARRAY(regexes, snapshot_regexes);
    SPEC_ARRAY(heredoc_headers, snapshot_heredoc_headers);
    SPEC_ARRAY(heredocs, snapshot_heredocs);
    SPEC_ARRAY(embeds, snapshot_embeds);
    SPEC_ARRAY(fences, snapshot_fences);
    SPEC_ARRAY(literal_hooks, snapshot_literal_hooks);
    SPEC_ARRAY(recoveries, snapshot_recoveries);
    SPEC_ARRAY(declarations, snapshot_declarations);
    if (spec->operators) {
        relocate_to(build, offset + offsetof(scan_lex_spec_t, operators),
                    snapshot_string_array(build, spec->operators));
    }
    if (spec->soft_keywords) {
        relocate_to(build, offset + offsetof(scan_lex_spec_t, soft_keywords),
                    snapshot_string_array(build, spec->soft_keywords));
    }
    if (spec->number.radixes) {
        relocate_to(build,
                    offset + offsetof(scan_lex_spec_t, number)
                        + offsetof(scan_lex_number_t, radixes),
                    snapshot_radixes(build, spec->number.radixes));
    }
    if (spec->number.suffixes) {
        relocate_to(build,
                    offset + offsetof(scan_lex_spec_t, number)
                        + offsetof(scan_lex_number_t, suffixes),
                    snapshot_suffixes(build, spec->number.suffixes));
    }
#define NUMBER_STRING(field)                                                   \
    snapshot_string_field(build, offset + offsetof(scan_lex_spec_t, number),  \
                          offsetof(scan_lex_number_t, field),                  \
                          spec->number.field)
    NUMBER_STRING(sep);
    NUMBER_STRING(sign);
    NUMBER_STRING(exponent);
    NUMBER_STRING(tok_int);
    NUMBER_STRING(tok_float);
    SPEC_ARRAY(number_forms, snapshot_number_forms);
    SPEC_STRING(tok_newline);
    SPEC_STRING(tok_terminator);
    SPEC_STRING(terminator_after);
    SPEC_STRING(tok_indent);
    SPEC_STRING(tok_dedent);
    SPEC_STRING(tok_eof);
    SPEC_STRING(tok_ident);
    SPEC_STRING(line_continue);
    SPEC_STRING(brackets);
    SPEC_ARRAY(directives, snapshot_directives);
    SPEC_STRING(ident_start_extra);
    SPEC_STRING(ident_extra);
    SPEC_STRING(tok_ident_upper);

    size_t profile = offset + offsetof(scan_lex_spec_t, identifier_profile);
    snapshot_string_field(build, profile,
                          offsetof(scan_lex_identifier_profile_t, name),
                          spec->identifier_profile.name);
    snapshot_string_field(build, profile,
                          offsetof(scan_lex_identifier_profile_t,
                                   unicode_version),
                          spec->identifier_profile.unicode_version);
    if (spec->identifier_profile.scripts) {
        relocate_to(build,
                    profile + offsetof(scan_lex_identifier_profile_t, scripts),
                    snapshot_string_array(build,
                                          spec->identifier_profile.scripts));
    }

#undef NUMBER_STRING
#undef SPEC_ARRAY
#undef SPEC_STRING
    return offset;
}

#undef SNAP_STRING
#undef SIMPLE_RECIPE_SNAPSHOT
#undef COUNT_RECIPES

static void
clear_pointer(lex_image_build_t *build, size_t slot)
{
    uintptr_t zero = 0;
    memcpy(build->payload->data + slot, &zero, sizeof(zero));
}

static void
snapshot_known_pointer(lex_image_build_t *build, size_t slot,
                       const void *target)
{
    size_t offset;
    if (!target) {
        clear_pointer(build, slot);
    }
    else if (object_offset(build, target, &offset)) {
        relocate_to(build, slot, offset);
    }
    else {
        build->valid = false;
    }
}

static size_t
bucket_items(const lex_bucket_t buckets[256])
{
    size_t count = 0;
    for (size_t i = 0; i < 256; i++) {
        size_t end = (size_t)buckets[i].start + buckets[i].count;
        if (end > count) {
            count = end;
        }
    }
    return count;
}

static void
snapshot_pointer_array(lex_image_build_t *build, size_t owner_slot,
                       const void *const *items, size_t count)
{
    if (!items) {
        clear_pointer(build, owner_slot);
        return;
    }
    size_t offset = snapshot_array(build, items, count, sizeof(*items),
                                   _Alignof(void *));
    relocate_to(build, owner_slot, offset);
    for (size_t i = 0; i < count; i++) {
        snapshot_known_pointer(build, offset + i * sizeof(*items), items[i]);
    }
}

static void
snapshot_list(lex_image_build_t *build, size_t program_offset,
              size_t field_offset, const void *data, size_t count,
              size_t width, size_t alignment)
{
    size_t slot = program_offset + field_offset;
    if (!data || !count) {
        clear_pointer(build, slot);
        return;
    }
    relocate_to(build, slot,
                snapshot_array(build, data, count, width, alignment));
}

static size_t snapshot_program(lex_image_build_t *, const scan_lex_t *);

static size_t
snapshot_program(lex_image_build_t *build, const scan_lex_t *program)
{
    size_t existing;
    if (object_offset(build, program, &existing)) {
        return existing;
    }
    size_t offset = add_object(build, program, sizeof(*program),
                               _Alignof(scan_lex_t));
    clear_pointer(build, offset + offsetof(scan_lex_t, g));
    clear_pointer(build, offset + offsetof(scan_lex_t, image_owner));
    clear_pointer(build, offset + offsetof(scan_lex_t, identifier_profile));
    relocate_to(build, offset + offsetof(scan_lex_t, spec),
                snapshot_spec(build, program->spec));

    if (program->issues) {
        size_t issues = snapshot_array(build, program->issues,
                                       program->issue_count,
                                       sizeof(*program->issues),
                                       _Alignof(scan_lex_issue_t));
        relocate_to(build, offset + offsetof(scan_lex_t, issues), issues);
        for (size_t i = 0; i < program->issue_count; i++) {
            snapshot_string_field(build,
                                  issues + i * sizeof(*program->issues),
                                  offsetof(scan_lex_issue_t, token),
                                  program->issues[i].token);
            snapshot_string_field(build,
                                  issues + i * sizeof(*program->issues),
                                  offsetof(scan_lex_issue_t, origin),
                                  program->issues[i].origin);
        }
    }
    else {
        clear_pointer(build, offset + offsetof(scan_lex_t, issues));
    }

#define SNAP_LIST(field, type)                                                 \
    snapshot_list(build, offset, offsetof(scan_lex_t, field),                 \
                  program->field.data, program->field.len, sizeof(type),       \
                  _Alignof(type))
    SNAP_LIST(term_nodes, lex_term_node_t);
    SNAP_LIST(term_edges, lex_term_edge_t);
    SNAP_LIST(opener_nodes, lex_term_node_t);
    SNAP_LIST(opener_edges, lex_term_edge_t);
    SNAP_LIST(string_nodes, lex_term_node_t);
    SNAP_LIST(string_edges, lex_term_edge_t);
    SNAP_LIST(number_nodes, lex_term_node_t);
    SNAP_LIST(number_edges, lex_term_edge_t);
    SNAP_LIST(number_suffix_nodes, lex_term_node_t);
    SNAP_LIST(number_suffix_edges, lex_term_edge_t);
    SNAP_LIST(number_suffix_roots, int32_t);
#undef SNAP_LIST

#define SNAP_DISPATCH(field, buckets)                                         \
    snapshot_pointer_array(                                                    \
        build, offset + offsetof(scan_lex_t, field),                          \
        program->field, bucket_items(program->buckets))
    SNAP_DISPATCH(comment_dispatch, comment_buckets);
    SNAP_DISPATCH(sigil_dispatch, sigil_buckets);
    SNAP_DISPATCH(regex_dispatch, regex_buckets);
    SNAP_DISPATCH(heredoc_dispatch, heredoc_buckets);
    SNAP_DISPATCH(embed_dispatch, embed_buckets);
    SNAP_DISPATCH(embed_close_dispatch, embed_close_buckets);
    SNAP_DISPATCH(fence_dispatch, fence_buckets);
    SNAP_DISPATCH(radix_dispatch, radix_buckets);
    SNAP_DISPATCH(suffix_dispatch, suffix_buckets);
#undef SNAP_DISPATCH

    if (program->embed_programs) {
        for (size_t i = 0; i < program->embed_count; i++) {
            snapshot_program(build, program->embed_programs[i]);
        }
        snapshot_pointer_array(
            build, offset + offsetof(scan_lex_t, embed_programs),
            (const void *const *)program->embed_programs,
            program->embed_count);
    }
    else {
        clear_pointer(build, offset + offsetof(scan_lex_t, embed_programs));
    }

    if (program->regex_not_after) {
        size_t sets = snapshot_array(build, program->regex_not_after,
                                     program->regex_count,
                                     sizeof(*program->regex_not_after),
                                     _Alignof(lex_tid_set_t));
        relocate_to(build, offset + offsetof(scan_lex_t, regex_not_after), sets);
        for (size_t i = 0; i < program->regex_count; i++) {
            const lex_tid_set_t *set = &program->regex_not_after[i];
            size_t item = sets + i * sizeof(*set);
            if (set->slots) {
                relocate_to(build, item + offsetof(lex_tid_set_t, slots),
                            snapshot_array(build, set->slots, set->capacity,
                                           sizeof(*set->slots),
                                           _Alignof(int32_t)));
            }
        }
    }
    else {
        clear_pointer(build, offset + offsetof(scan_lex_t, regex_not_after));
    }

    snapshot_pointer_array(
        build, offset + offsetof(scan_lex_t, string_escape_dispatch),
        (const void *const *)program->string_escape_dispatch,
        program->string_count * 128);
    if (program->string_escape_complex) {
        relocate_to(build,
                    offset + offsetof(scan_lex_t, string_escape_complex),
                    snapshot_array(build, program->string_escape_complex,
                                   program->string_count, sizeof(bool),
                                   _Alignof(bool)));
    }
    else {
        clear_pointer(build,
                      offset + offsetof(scan_lex_t, string_escape_complex));
    }

#define SNAP_TID_SET(field)                                                    \
    do {                                                                       \
        const lex_tid_set_t *_set = &program->field;                          \
        size_t _slot = offset + offsetof(scan_lex_t, field)                   \
                     + offsetof(lex_tid_set_t, slots);                         \
        if (_set->slots) {                                                     \
            relocate_to(build, _slot,                                         \
                        snapshot_array(build, _set->slots, _set->capacity,    \
                                       sizeof(*_set->slots),                   \
                                       _Alignof(int32_t)));                    \
        }                                                                      \
        else {                                                                 \
            clear_pointer(build, _slot);                                       \
        }                                                                      \
    } while (0)
    SNAP_TID_SET(is_literal);
    SNAP_TID_SET(codepoint_terminals);
#undef SNAP_TID_SET

    if (program->literal_hooks) {
        size_t hooks = snapshot_array(build, program->literal_hooks,
                                      program->literal_hook_count,
                                      sizeof(*program->literal_hooks),
                                      _Alignof(lex_literal_hook_t));
        relocate_to(build, offset + offsetof(scan_lex_t, literal_hooks), hooks);
        for (size_t i = 0; i < program->literal_hook_count; i++) {
            snapshot_known_pointer(
                build,
                hooks + i * sizeof(*program->literal_hooks)
                    + offsetof(lex_literal_hook_t, recipe),
                program->literal_hooks[i].recipe);
        }
    }
    else {
        clear_pointer(build, offset + offsetof(scan_lex_t, literal_hooks));
    }

    if (program->recoveries) {
        size_t recoveries = snapshot_array(build, program->recoveries,
                                            program->recovery_count,
                                            sizeof(*program->recoveries),
                                            _Alignof(lex_recovery_t));
        relocate_to(build, offset + offsetof(scan_lex_t, recoveries),
                    recoveries);
        for (size_t i = 0; i < program->recovery_count; i++) {
            snapshot_known_pointer(
                build,
                recoveries + i * sizeof(*program->recoveries)
                    + offsetof(lex_recovery_t, recipe),
                program->recoveries[i].recipe);
        }
    }
    else {
        clear_pointer(build, offset + offsetof(scan_lex_t, recoveries));
    }

    if (program->ascii_sets) {
        size_t sets = snapshot_array(build, program->ascii_sets,
                                     program->ascii_set_capacity,
                                     sizeof(*program->ascii_sets),
                                     _Alignof(lex_ascii_slot_t));
        relocate_to(build, offset + offsetof(scan_lex_t, ascii_sets), sets);
        for (size_t i = 0; i < program->ascii_set_capacity; i++) {
            if (program->ascii_sets[i].key.data) {
                snapshot_string_field(
                    build, sets + i * sizeof(*program->ascii_sets),
                    offsetof(lex_ascii_slot_t, key),
                    program->ascii_sets[i].key);
            }
        }
    }
    else {
        clear_pointer(build, offset + offsetof(scan_lex_t, ascii_sets));
    }

    if (program->term_after.data && program->term_after.len) {
        size_t terms = snapshot_array(build, program->term_after.data,
                                      program->term_after.len,
                                      sizeof(core_string_t),
                                      _Alignof(core_string_t));
        relocate_to(build,
                    offset + offsetof(scan_lex_t, term_after)
                        + offsetof(core_list_t(core_string_t), data),
                    terms);
        for (size_t i = 0; i < program->term_after.len; i++) {
            snapshot_string_field(build, terms + i * sizeof(core_string_t),
                                  0, program->term_after.data[i]);
        }
    }
    else {
        clear_pointer(build,
                      offset + offsetof(scan_lex_t, term_after)
                          + offsetof(core_list_t(core_string_t), data));
    }
    return offset;
}

core_string_t
scan_lex_image_encode(const scan_lex_t *program)
{
    image_error[0] = '\0';
    if (!program || !program->g || !program->spec) {
        snprintf(image_error, sizeof(image_error),
                 "scanner image needs a compiled program and grammar");
        return (core_string_t){0};
    }

    lex_image_build_t build = {
        .payload = core_buffer_empty(),
        .valid = true,
    };
    core_dict_init(&build.objects, core_hash_word, nullptr);
    size_t root = snapshot_program(&build, program);
    canonicalize_relocations(&build);

    core_string_t result = {0};
    if (build.valid) {
        lex_image_header_t header = {
            .magic = LEX_IMAGE_MAGIC,
            .format = LEX_IMAGE_FORMAT,
            .endian = LEX_IMAGE_ENDIAN,
            .pointer_bytes = sizeof(void *),
            .size_bytes = sizeof(size_t),
            .abi = image_abi(),
            .grammar = grammar_fingerprint(program->g),
            .payload_bytes = build.payload->byte_len,
            .root_offset = root,
            .relocation_count = build.relocation_count,
        };
        uint64_t checksum = hash_header(UINT64_C(1469598103934665603),
                                        &header);
        checksum = hash_bytes(checksum, build.payload->data,
                              build.payload->byte_len);
        for (size_t i = 0; i < build.relocation_count; i++) {
            uint64_t relocation = build.relocations[i];
            checksum = hash_bytes(checksum, &relocation,
                                  sizeof(relocation));
        }
        header.checksum = checksum;
        size_t bytes = sizeof(header) + build.payload->byte_len
                     + build.relocation_count * sizeof(uint64_t);
        char *data = core_alloc_size(1, bytes);
        memcpy(data, &header, sizeof(header));
        memcpy(data + sizeof(header), build.payload->data,
               build.payload->byte_len);
        for (size_t i = 0; i < build.relocation_count; i++) {
            uint64_t relocation = build.relocations[i];
            memcpy(data + sizeof(header) + build.payload->byte_len
                       + i * sizeof(relocation),
                   &relocation, sizeof(relocation));
        }
        result = core_string_borrow(data, bytes);
    }
    else {
        snprintf(image_error, sizeof(image_error),
                 "scanner program contains an unencodable reference");
    }

    core_dict_free(&build.objects);
    core_free(build.relocations);
    core_buffer_free(build.payload);
    return result;
}

typedef struct {
    scan_lex_t **items;
    size_t       count;
    size_t       capacity;
    core_dict_t  seen;
    char        *base;
    size_t       bytes;
    bool         valid;
} loaded_programs_t;

static bool
image_contains(const loaded_programs_t *programs, const void *pointer,
               size_t bytes)
{
    uintptr_t start = (uintptr_t)programs->base;
    uintptr_t value = (uintptr_t)pointer;
    return value >= start && value - start <= programs->bytes
        && bytes <= programs->bytes - (value - start);
}

static void
collect_program(loaded_programs_t *programs, scan_lex_t *program, size_t depth)
{
    if (!programs->valid || depth > 64
        || !image_contains(programs, program, sizeof(*program))) {
        programs->valid = false;
        return;
    }
    bool found = false;
    core_dict_get(&programs->seen, program, &found);
    if (found) {
        return;
    }
    core_dict_put(&programs->seen, program, (void *)1);
    if (programs->count == programs->capacity) {
        size_t capacity = programs->capacity ? programs->capacity * 2 : 8;
        programs->items = core_realloc(programs->items,
                                       capacity * sizeof(*programs->items));
        programs->capacity = capacity;
    }
    programs->items[programs->count++] = program;
    if (program->embed_count && !image_contains(
            programs, program->embed_programs,
            program->embed_count * sizeof(*program->embed_programs))) {
        programs->valid = false;
        return;
    }
    for (size_t i = 0; i < program->embed_count; i++) {
        collect_program(programs, program->embed_programs[i], depth + 1);
    }
}

static bool
validate_loaded_program(loaded_programs_t *all, scan_lex_t *program,
                        parse_grammar_t *grammar)
{
    if (!image_contains(all, program->spec, sizeof(*program->spec))) {
        snprintf(image_error, sizeof(image_error),
                 "scanner image has an invalid specification reference");
        return false;
    }
    if (program->issue_count > SIZE_MAX / sizeof(*program->issues)
        || (program->issue_count
            && !image_contains(all, program->issues,
                               program->issue_count
                                   * sizeof(*program->issues)))) {
        snprintf(image_error, sizeof(image_error),
                 "scanner image has an invalid analysis-issue table");
        return false;
    }
    for (size_t i = 0; i < program->issue_count; i++) {
        const scan_lex_issue_t *issue = &program->issues[i];
        if ((issue->token.u8_bytes
             && !image_contains(all, issue->token.data,
                                issue->token.u8_bytes))
            || (issue->origin.u8_bytes
                && !image_contains(all, issue->origin.data,
                                   issue->origin.u8_bytes))) {
            snprintf(image_error, sizeof(image_error),
                     "scanner image has an invalid analysis issue");
            return false;
        }
    }
    for (const scan_lex_literal_hook_t *hook = program->spec->literal_hooks;
         hook && hook->token.data; hook++) {
        if (!scan_lex_literal_hook_exists(hook->hook)) {
            snprintf(image_error, sizeof(image_error),
                     "scanner image requires unregistered literal hook '%.*s'",
                     (int)hook->hook.u8_bytes, hook->hook.data);
            return false;
        }
    }
    if (program->spec->identifier_profile.name.data) {
        program->identifier_profile = scan_unicode_profile_compile(
            &program->spec->identifier_profile,
            image_error, sizeof(image_error));
        if (!program->identifier_profile) {
            return false;
        }
    }
    program->g = grammar;
    return true;
}

scan_lex_t *
scan_lex_image_load(core_string_t image, parse_grammar_t *grammar)
{
    image_error[0] = '\0';
    if (!image.data || image.u8_bytes < sizeof(lex_image_header_t)
        || !grammar) {
        snprintf(image_error, sizeof(image_error),
                 "scanner image or grammar is missing");
        return nullptr;
    }

    lex_image_header_t header;
    memcpy(&header, image.data, sizeof(header));
    if (header.magic != LEX_IMAGE_MAGIC || header.format != LEX_IMAGE_FORMAT
        || header.endian != LEX_IMAGE_ENDIAN
        || header.pointer_bytes != sizeof(void *)
        || header.size_bytes != sizeof(size_t)
        || header.abi != image_abi()) {
        snprintf(image_error, sizeof(image_error),
                 "scanner image is incompatible with this build");
        return nullptr;
    }
    if (header.payload_bytes > SIZE_MAX
        || header.relocation_count > SIZE_MAX / sizeof(uint64_t)) {
        snprintf(image_error, sizeof(image_error),
                 "scanner image sections are out of bounds");
        return nullptr;
    }

    size_t payload_bytes = (size_t)header.payload_bytes;
    size_t relocation_bytes
        = (size_t)header.relocation_count * sizeof(uint64_t);
    size_t available = image.u8_bytes - sizeof(header);
    if (payload_bytes > available
        || relocation_bytes != available - payload_bytes
        || header.root_offset > payload_bytes
        || sizeof(scan_lex_t) > payload_bytes - (size_t)header.root_offset) {
        snprintf(image_error, sizeof(image_error),
                 "scanner image sections are out of bounds");
        return nullptr;
    }

    const char *payload = image.data + sizeof(header);
    const char *relocation_data = payload + payload_bytes;
    uint64_t checksum = hash_header(UINT64_C(1469598103934665603), &header);
    checksum = hash_bytes(checksum, payload, payload_bytes);
    checksum = hash_bytes(checksum, relocation_data, relocation_bytes);
    if (header.checksum != checksum) {
        snprintf(image_error, sizeof(image_error),
                 "scanner image checksum does not match");
        return nullptr;
    }
    if (header.grammar != grammar_fingerprint(grammar)) {
        snprintf(image_error, sizeof(image_error),
                 "scanner image terminal vocabulary does not match grammar");
        return nullptr;
    }

    char *storage = core_alloc_size(1, payload_bytes);
    memcpy(storage, payload, payload_bytes);

    for (size_t i = 0; i < (size_t)header.relocation_count; i++) {
        uint64_t slot64;
        memcpy(&slot64, relocation_data + i * sizeof(slot64), sizeof(slot64));
        if (slot64 > SIZE_MAX
            || (size_t)slot64 > payload_bytes
            || sizeof(uintptr_t) > payload_bytes - (size_t)slot64) {
            snprintf(image_error, sizeof(image_error),
                     "scanner image relocation is out of bounds");
            core_free(storage);
            return nullptr;
        }
        uintptr_t encoded;
        memcpy(&encoded, storage + (size_t)slot64, sizeof(encoded));
        if (!encoded || encoded - 1 >= payload_bytes) {
            snprintf(image_error, sizeof(image_error),
                     "scanner image relocation %zu target %zu is out of "
                     "bounds at slot %zu (root %zu) for %zu bytes",
                     i, (size_t)encoded, (size_t)slot64,
                     (size_t)header.root_offset, payload_bytes);
            core_free(storage);
            return nullptr;
        }
        void *pointer = storage + encoded - 1;
        memcpy(storage + (size_t)slot64, &pointer, sizeof(pointer));
    }

    scan_lex_t *root = (scan_lex_t *)(void *)(storage + header.root_offset);
    loaded_programs_t programs = {
        .base = storage,
        .bytes = payload_bytes,
        .valid = true,
    };
    core_dict_init(&programs.seen, core_hash_word, nullptr);
    collect_program(&programs, root, 0);

    bool valid = programs.valid;
    for (size_t i = 0; valid && i < programs.count; i++) {
        valid = validate_loaded_program(&programs, programs.items[i], grammar);
    }
    if (!valid) {
        if (!image_error[0]) {
            snprintf(image_error, sizeof(image_error),
                     "scanner image program graph is invalid");
        }
        for (size_t i = 0; i < programs.count; i++) {
            scan_unicode_profile_free(programs.items[i]->identifier_profile);
            programs.items[i]->identifier_profile = nullptr;
        }
        core_dict_free(&programs.seen);
        core_free(programs.items);
        core_free(storage);
        return nullptr;
    }

    lex_image_owner_t *owner = core_alloc(lex_image_owner_t);
    owner->storage = storage;
    owner->storage_bytes = payload_bytes;
    owner->live_programs = programs.count;
    for (size_t i = 0; i < programs.count; i++) {
        programs.items[i]->image_owner = owner;
    }
    parse_grammar_prepare_contextual(grammar, root->spec->soft_keywords,
                                     root->spec->tok_ident);

    core_dict_free(&programs.seen);
    core_free(programs.items);
    return root;
}

bool
scan_grammar_set_lex_image(parse_grammar_t *grammar, core_string_t image)
{
    if (!grammar) {
        return false;
    }
    if (grammar->finalized) {
        snprintf(image_error, sizeof(image_error),
                 "scanner image must be installed before grammar finalization");
        return false;
    }
    scan_lex_t *program = scan_lex_image_load(image, grammar);
    if (!program) {
        return false;
    }

    if (grammar->lex_compiled) {
        scan_lex_free(grammar->lex_compiled);
    }
    if (grammar->lex_spec_owned) {
        scan_lex_spec_free(grammar->lex_spec);
    }
    grammar->lex_compiled = program;
    grammar->lex_spec = program->spec;
    grammar->lex_spec_owned = false;
    grammar->lex_resolved = true;
    return true;
}
