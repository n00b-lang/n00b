#include "scanner/lex_conformance.h"
#include "private/scanner/lex_program.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    scan_lex_decl_kind_t declaration;
    core_string_t        spelling;
    core_string_t        origin;
    core_string_t        source;
    size_t               open_bytes;
    size_t               close_start;
    size_t               close_bytes;
    bool                 needs_tail;
    bool                 overlap;
} conformance_vector_t;

typedef struct {
    scan_lex_decl_kind_t kind;
    core_string_t        spelling;
    core_string_t        origin;
} conformance_origin_t;

struct scan_lex_conformance_suite_t {
    scan_lex_conformance_case_t *cases;
    size_t                       count;
    size_t                       capacity;
    size_t                       limit;
    core_string_t               *sources;
    size_t                       source_count;
    size_t                       source_capacity;
    uint64_t                     fingerprint;
};

static int
string_order(core_string_t a, core_string_t b)
{
    size_t common = a.u8_bytes < b.u8_bytes ? a.u8_bytes : b.u8_bytes;
    int order = common ? memcmp(a.data, b.data, common) : 0;
    if (order) {
        return order;
    }
    return (a.u8_bytes > b.u8_bytes) - (a.u8_bytes < b.u8_bytes);
}

static bool
is_prefix(core_string_t prefix, core_string_t value)
{
    return prefix.u8_bytes <= value.u8_bytes
        && !memcmp(prefix.data, value.data, prefix.u8_bytes);
}

static int
vector_order(const void *left, const void *right)
{
    const conformance_vector_t *a = left;
    const conformance_vector_t *b = right;
    int order = string_order(a->spelling, b->spelling);
    if (order) {
        return order;
    }
    if (a->declaration != b->declaration) {
        return (a->declaration > b->declaration)
             - (a->declaration < b->declaration);
    }
    return string_order(a->source, b->source);
}

static int
origin_order(const void *left, const void *right)
{
    const conformance_origin_t *a = left;
    const conformance_origin_t *b = right;
    if (a->kind != b->kind) {
        return (a->kind > b->kind) - (a->kind < b->kind);
    }
    int order = string_order(a->spelling, b->spelling);
    return order ? order : string_order(a->origin, b->origin);
}

static int
case_order(const void *left, const void *right)
{
    const scan_lex_conformance_case_t *a = left;
    const scan_lex_conformance_case_t *b = right;
    if (a->kind != b->kind) {
        return (a->kind > b->kind) - (a->kind < b->kind);
    }
    if (a->declaration != b->declaration) {
        return (a->declaration > b->declaration)
             - (a->declaration < b->declaration);
    }
    int order = string_order(a->spelling, b->spelling);
    if (order) {
        return order;
    }
    order = string_order(a->source, b->source);
    if (order) {
        return order;
    }
    order = string_order(a->origin, b->origin);
    if (order) {
        return order;
    }
    if (a->input_bytes != b->input_bytes) {
        return (a->input_bytes > b->input_bytes)
             - (a->input_bytes < b->input_bytes);
    }
    if (a->split_byte != b->split_byte) {
        return (a->split_byte > b->split_byte)
             - (a->split_byte < b->split_byte);
    }
    if (a->expected_status != b->expected_status) {
        return (a->expected_status > b->expected_status)
             - (a->expected_status < b->expected_status);
    }
    return (a->recovery > b->recovery) - (a->recovery < b->recovery);
}

static core_string_t
make_source(core_string_t open, core_string_t body, core_string_t close,
            size_t *close_start)
{
    core_buffer_t *buffer = core_buffer_empty();
    core_buffer_append(buffer, open.data, open.u8_bytes);
    core_buffer_append(buffer, body.data, body.u8_bytes);
    *close_start = buffer->byte_len;
    core_buffer_append(buffer, close.data, close.u8_bytes);
    size_t bytes = buffer->byte_len;
    return (core_string_t){
        .data = core_buffer_take(buffer),
        .u8_bytes = bytes,
    };
}

static void
push_vector(conformance_vector_t **items, size_t *count, size_t *capacity,
            scan_lex_decl_kind_t declaration, core_string_t spelling,
            core_string_t source, size_t open_bytes, size_t close_start,
            size_t close_bytes, bool needs_tail)
{
    if (!source.data || !source.u8_bytes) {
        core_free(source.data);
        return;
    }
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 32;
        *items = core_realloc(*items, next * sizeof(**items));
        *capacity = next;
    }
    (*items)[(*count)++] = (conformance_vector_t){
        .declaration = declaration,
        .spelling = spelling,
        .source = source,
        .open_bytes = open_bytes,
        .close_start = close_start,
        .close_bytes = close_bytes,
        .needs_tail = needs_tail,
    };
}

static core_string_t
owned_copy(core_string_t value)
{
    if (!value.data) {
        return (core_string_t){0};
    }
    char *data = core_alloc_size(1, value.u8_bytes + 1);
    memcpy(data, value.data, value.u8_bytes);
    data[value.u8_bytes] = 0;
    return core_string_borrow(data, value.u8_bytes);
}

static conformance_origin_t *
make_origin_index(const scan_lex_spec_t *spec, size_t *count)
{
    *count = 0;
    for (const scan_lex_declaration_t *item = spec->declarations;
         item && item->kind != SCAN_LEX_DECL_NONE; item++) {
        (*count)++;
    }
    if (!*count) {
        return nullptr;
    }
    conformance_origin_t *index
        = core_alloc_array(conformance_origin_t, *count);
    for (size_t i = 0; i < *count; i++) {
        index[i] = (conformance_origin_t){
            .kind = spec->declarations[i].kind,
            .spelling = spec->declarations[i].spelling,
            .origin = spec->declarations[i].origin,
        };
    }
    qsort(index, *count, sizeof(*index), origin_order);
    return index;
}

static core_string_t
origin_for(const conformance_origin_t *index, size_t count,
           scan_lex_decl_kind_t kind, core_string_t spelling)
{
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int order = index[mid].kind == kind
                  ? string_order(index[mid].spelling, spelling)
                  : (index[mid].kind > kind ? 1 : -1);
        if (order < 0) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }
    return lo < count && index[lo].kind == kind
           && core_string_eq(index[lo].spelling, spelling)
         ? index[lo].origin : (core_string_t){0};
}

static void
own_source(scan_lex_conformance_suite_t *suite, core_string_t source)
{
    if (suite->source_count == suite->source_capacity) {
        size_t next = suite->source_capacity ? suite->source_capacity * 2 : 32;
        suite->sources = core_realloc(suite->sources,
                                      next * sizeof(*suite->sources));
        suite->source_capacity = next;
    }
    suite->sources[suite->source_count++] = source;
}

static bool
push_case(scan_lex_conformance_suite_t *suite,
          scan_lex_conformance_case_t item)
{
    if (suite->limit && suite->count >= suite->limit) {
        return false;
    }
    if (suite->count == suite->capacity) {
        size_t next = suite->capacity ? suite->capacity * 2 : 128;
        if (suite->limit && next > suite->limit) {
            next = suite->limit;
        }
        suite->cases = core_realloc(suite->cases,
                                    next * sizeof(*suite->cases));
        suite->capacity = next;
    }
    item.id = suite->count;
    suite->cases[suite->count++] = item;
    return true;
}

static void
add_boundary_cases(scan_lex_conformance_suite_t *suite,
                   const conformance_vector_t *vector)
{
    scan_lex_conformance_case_t base = {
        .kind = vector->overlap ? SCAN_LEX_CONFORMANCE_LONGEST_PREFIX
                                : SCAN_LEX_CONFORMANCE_BOUNDARY,
        .declaration = vector->declaration,
        .spelling = vector->spelling,
        .origin = vector->origin,
        .source = vector->source,
        .input_bytes = vector->source.u8_bytes,
        .split_byte = SIZE_MAX,
        .expected_status = SCAN_STATUS_COMPLETE,
    };
    if (!push_case(suite, base)) {
        return;
    }
    base.kind = SCAN_LEX_CONFORMANCE_CONCURRENT;
    if (!push_case(suite, base)) {
        return;
    }

    size_t positions[2] = {vector->open_bytes,
                           vector->close_start + vector->close_bytes};
    size_t starts[2] = {0, vector->close_start};
    for (size_t part = 0; part < 2; part++) {
        size_t end = positions[part];
        if (!end || end <= starts[part]) {
            continue;
        }
        for (size_t split = starts[part] + 1; split < end; split++) {
            scan_lex_conformance_case_t incremental = base;
            incremental.kind = SCAN_LEX_CONFORMANCE_INCREMENTAL_SPLIT;
            incremental.split_byte = split;
            if (!push_case(suite, incremental)) {
                return;
            }
            scan_lex_conformance_case_t truncated = incremental;
            truncated.kind = SCAN_LEX_CONFORMANCE_TRUNCATION;
            truncated.input_bytes = split;
            truncated.split_byte = SIZE_MAX;
            truncated.expected_status = SCAN_STATUS_INCOMPLETE;
            if (!push_case(suite, truncated)) {
                return;
            }
        }
    }
    if (vector->needs_tail && vector->open_bytes < vector->source.u8_bytes) {
        scan_lex_conformance_case_t incremental = base;
        incremental.kind = SCAN_LEX_CONFORMANCE_INCREMENTAL_SPLIT;
        incremental.split_byte = vector->open_bytes;
        if (!push_case(suite, incremental)) {
            return;
        }
        incremental.kind = SCAN_LEX_CONFORMANCE_TRUNCATION;
        incremental.input_bytes = vector->open_bytes;
        incremental.split_byte = SIZE_MAX;
        incremental.expected_status = SCAN_STATUS_INCOMPLETE;
        push_case(suite, incremental);
    }
}

static uint64_t
hash_bytes(uint64_t hash, const void *data, size_t bytes)
{
    const unsigned char *at = data;
    for (size_t i = 0; i < bytes; i++) {
        hash ^= at[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void
add_malformed_case(scan_lex_conformance_suite_t *suite,
                   const conformance_origin_t *origins, size_t origin_count,
                   scan_lex_decl_kind_t declaration, core_string_t spelling,
                   core_string_t source, bool recovery)
{
    own_source(suite, source);
    push_case(suite, (scan_lex_conformance_case_t){
        .kind = SCAN_LEX_CONFORMANCE_MALFORMED,
        .declaration = declaration,
        .spelling = spelling,
        .origin = origin_for(origins, origin_count, declaration, spelling),
        .source = source,
        .input_bytes = source.u8_bytes,
        .split_byte = SIZE_MAX,
        .expected_status = recovery ? SCAN_STATUS_RECOVERED
                                    : SCAN_STATUS_INVALID,
        .recovery = recovery,
    });
}

static bool
escape_starts_with(const scan_lex_escape_t *items, char byte)
{
    for (; items && items->prefix.data; items++) {
        if (items->prefix.u8_bytes
            && items->prefix.data[0] == byte) {
            return true;
        }
    }
    return false;
}

static bool
simple_escape_contains(core_string_t simple, char byte)
{
    return simple.data && memchr(simple.data, byte, simple.u8_bytes);
}

static core_string_t
malformed_string(const scan_lex_string_t *item)
{
    if (!item->escape.data || !item->escape.u8_bytes
        || item->flags & SCAN_LEX_STR_RAW
        || (!item->simple_escapes.data && !item->escape_rules)) {
        return (core_string_t){0};
    }
    char bad = 0;
    for (char candidate = '!'; candidate <= '~'; candidate++) {
        if (!simple_escape_contains(item->simple_escapes, candidate)
            && !escape_starts_with(item->escape_rules, candidate)) {
            bad = candidate;
            break;
        }
    }
    if (!bad) {
        return (core_string_t){0};
    }
    core_buffer_t *buffer = core_buffer_empty();
    core_buffer_append(buffer, item->opener.data, item->opener.u8_bytes);
    core_buffer_append(buffer, item->escape.data, item->escape.u8_bytes);
    core_buffer_putc(buffer, bad);
    core_buffer_append(buffer, item->closer.data, item->closer.u8_bytes);
    size_t bytes = buffer->byte_len;
    return core_string_borrow(core_buffer_take(buffer), bytes);
}

static core_string_t
append_sync(core_string_t malformed, const scan_lex_recovery_t *recovery)
{
    if (!malformed.data || !recovery->synchronize
        || !recovery->synchronize[0].data) {
        return (core_string_t){0};
    }
    core_buffer_t *buffer = core_buffer_empty();
    core_buffer_append(buffer, malformed.data, malformed.u8_bytes);
    core_buffer_append(buffer, recovery->synchronize[0].data,
                       recovery->synchronize[0].u8_bytes);
    size_t bytes = buffer->byte_len;
    return core_string_borrow(core_buffer_take(buffer), bytes);
}

scan_lex_conformance_suite_t *
scan_lex_conformance_generate(const scan_lex_t *program, size_t max_cases)
{
    if (!program || !program->spec) {
        return nullptr;
    }
    const scan_lex_spec_t *spec = program->spec;
    conformance_vector_t *vectors = nullptr;
    size_t count = 0;
    size_t capacity = 0;
    size_t close_start = 0;

#define WRAPPED(field, type, open_member, close_member, decl_kind, body)       \
    for (const type *item = spec->field; item && item->open_member.data;       \
         item++) {                                                             \
        core_string_t source = make_source(item->open_member, body,            \
                                            item->close_member, &close_start);  \
        push_vector(&vectors, &count, &capacity, decl_kind,                    \
                    item->open_member, source, item->open_member.u8_bytes,     \
                    close_start, item->close_member.u8_bytes, true);           \
    }
    for (const scan_lex_comment_t *item = spec->comments;
         item && item->opener.data; item++) {
        core_string_t close = item->kind == SCAN_LEX_LINE
                            ? CORE_STRING_STATIC("\n") : item->closer;
        core_string_t source = make_source(item->opener,
                                            CORE_STRING_STATIC("x"), close,
                                            &close_start);
        push_vector(&vectors, &count, &capacity, SCAN_LEX_DECL_COMMENT,
                    item->opener, source, item->opener.u8_bytes, close_start,
                    close.u8_bytes, true);
    }
    WRAPPED(strings, scan_lex_string_t, opener, closer,
            SCAN_LEX_DECL_STRING, CORE_STRING_STATIC("x"));
    WRAPPED(regexes, scan_lex_regex_t, opener, closer,
            SCAN_LEX_DECL_REGEX, CORE_STRING_STATIC("x"));
    WRAPPED(embeds, scan_lex_embed_t, open, close,
            SCAN_LEX_DECL_EMBED, (core_string_t){0});
#undef WRAPPED

    for (const scan_lex_sigil_t *item = spec->sigils;
         item && item->prefix.data; item++) {
        core_string_t source = make_source(item->prefix,
                                            CORE_STRING_STATIC("x"),
                                            (core_string_t){0}, &close_start);
        push_vector(&vectors, &count, &capacity, SCAN_LEX_DECL_SIGIL,
                    item->prefix, source, item->prefix.u8_bytes, 0, 0, true);
    }
    for (const scan_lex_heredoc_t *item = spec->heredocs;
         item && item->opener.data; item++) {
        core_buffer_t *buffer = core_buffer_empty();
        core_buffer_append(buffer, item->opener.data, item->opener.u8_bytes);
        core_buffer_puts(buffer, "TAG\nx\nTAG\n");
        size_t bytes = buffer->byte_len;
        core_string_t source = core_string_borrow(core_buffer_take(buffer),
                                                   bytes);
        push_vector(&vectors, &count, &capacity, SCAN_LEX_DECL_HEREDOC,
                    item->opener, source, item->opener.u8_bytes,
                    bytes - 4, 4, true);
    }
    for (const scan_lex_fence_t *item = spec->fences;
         item && item->outer_open.data; item++) {
        core_buffer_t *buffer = core_buffer_empty();
        core_buffer_append(buffer, item->outer_open.data,
                           item->outer_open.u8_bytes);
        for (uint32_t i = 0; i < item->minimum; i++) {
            core_buffer_append(buffer, item->fill.data, item->fill.u8_bytes);
        }
        core_buffer_append(buffer, item->body_open.data,
                           item->body_open.u8_bytes);
        core_buffer_putc(buffer, 'x');
        size_t close = buffer->byte_len;
        core_buffer_append(buffer, item->body_close.data,
                           item->body_close.u8_bytes);
        for (uint32_t i = 0; i < item->minimum; i++) {
            core_buffer_append(buffer, item->fill.data, item->fill.u8_bytes);
        }
        core_buffer_append(buffer, item->outer_close.data,
                           item->outer_close.u8_bytes);
        size_t bytes = buffer->byte_len;
        core_string_t source = core_string_borrow(core_buffer_take(buffer),
                                                   bytes);
        push_vector(&vectors, &count, &capacity, SCAN_LEX_DECL_FENCE,
                    item->outer_open, source, item->outer_open.u8_bytes,
                    close, bytes - close, true);
    }
    for (const scan_lex_directive_t *item = spec->directives;
         item && item->open.data; item++) {
        core_string_t source = make_source(item->open,
                                            CORE_STRING_STATIC("x"),
                                            CORE_STRING_STATIC("\n"),
                                            &close_start);
        push_vector(&vectors, &count, &capacity, SCAN_LEX_DECL_DIRECTIVE,
                    item->open, source, item->open.u8_bytes, close_start, 1,
                    true);
    }
    for (const scan_lex_radix_t *item = spec->number.radixes;
         item && item->prefix.data; item++) {
        char digit = item->digits.u8_bytes ? item->digits.data[0] : '0';
        core_string_t body = core_string_borrow(&digit, 1);
        core_string_t source = make_source(item->prefix, body,
                                            (core_string_t){0}, &close_start);
        push_vector(&vectors, &count, &capacity, SCAN_LEX_DECL_NUMBER,
                    item->prefix, source, item->prefix.u8_bytes, 0, 0, true);
    }
    for (const scan_lex_number_form_t *item = spec->number_forms;
         item && item->digits.data; item++) {
        if (!item->prefix.data || !item->prefix.u8_bytes) {
            continue;
        }
        char digit = item->digits.u8_bytes ? item->digits.data[0] : '0';
        core_string_t body = core_string_borrow(&digit, 1);
        core_string_t source = make_source(item->prefix, body,
                                            (core_string_t){0}, &close_start);
        push_vector(&vectors, &count, &capacity, SCAN_LEX_DECL_NUMBER,
                    item->prefix, source, item->prefix.u8_bytes, 0, 0, true);
    }
    for (const core_string_t *item = spec->operators;
         item && item->data; item++) {
        push_vector(&vectors, &count, &capacity, SCAN_LEX_DECL_OPERATOR,
                    *item, owned_copy(*item), item->u8_bytes, 0, 0, false);
    }
    for (const core_string_t *item = spec->soft_keywords;
         item && item->data; item++) {
        push_vector(&vectors, &count, &capacity,
                    SCAN_LEX_DECL_SOFT_KEYWORD, *item, owned_copy(*item),
                    item->u8_bytes, 0, 0, true);
    }

    qsort(vectors, count, sizeof(*vectors), vector_order);
    size_t *prefixes = core_alloc_array(size_t, count ? count : 1);
    size_t prefix_count = 0;
    for (size_t i = 0; i < count; i++) {
        while (prefix_count
               && !is_prefix(vectors[prefixes[prefix_count - 1]].spelling,
                             vectors[i].spelling)) {
            prefix_count--;
        }
        if (prefix_count) {
            vectors[prefixes[prefix_count - 1]].overlap = true;
            vectors[i].overlap = true;
        }
        prefixes[prefix_count++] = i;
    }
    core_free(prefixes);

    scan_lex_conformance_suite_t *suite
        = core_alloc(scan_lex_conformance_suite_t);
    suite->limit = max_cases;
    size_t origin_count = 0;
    conformance_origin_t *origins = make_origin_index(spec, &origin_count);
    for (size_t i = 0; i < count; i++) {
        vectors[i].origin = origin_for(origins, origin_count,
                                       vectors[i].declaration,
                                       vectors[i].spelling);
        own_source(suite, vectors[i].source);
        add_boundary_cases(suite, &vectors[i]);
        if (suite->limit && suite->count >= suite->limit) {
            for (size_t j = i + 1; j < count; j++) {
                core_free(vectors[j].source.data);
            }
            break;
        }
    }
    core_free(vectors);

    if (!suite->limit || suite->count < suite->limit) {
        for (const scan_lex_string_t *item = spec->strings;
             item && item->opener.data; item++) {
            core_string_t malformed = malformed_string(item);
            if (malformed.data) {
                add_malformed_case(suite, origins, origin_count,
                                   SCAN_LEX_DECL_STRING, item->opener,
                                   malformed, false);
            }
            if (suite->limit && suite->count >= suite->limit) {
                break;
            }
        }
    }
    if ((!suite->limit || suite->count < suite->limit)
        && spec->number.exponent.data && spec->number.exponent.u8_bytes) {
        core_buffer_t *buffer = core_buffer_from_cstr("1");
        core_buffer_putc(buffer, spec->number.exponent.data[0]);
        if (spec->number.sign.data
            && memchr(spec->number.sign.data, '+', spec->number.sign.u8_bytes)) {
            core_buffer_putc(buffer, '+');
        }
        size_t bytes = buffer->byte_len;
        add_malformed_case(suite, origins, origin_count,
                           SCAN_LEX_DECL_NUMBER, (core_string_t){0},
                           core_string_borrow(core_buffer_take(buffer), bytes),
                           false);
    }
    for (const scan_lex_radix_t *item = spec->number.radixes;
         (!suite->limit || suite->count < suite->limit)
             && item && item->prefix.data; item++) {
        add_malformed_case(suite, origins, origin_count,
                           SCAN_LEX_DECL_NUMBER, item->prefix,
                           owned_copy(item->prefix), false);
    }
    for (const scan_lex_number_form_t *item = spec->number_forms;
         (!suite->limit || suite->count < suite->limit)
             && item && item->digits.data; item++) {
        if (item->prefix.data && item->prefix.u8_bytes) {
            add_malformed_case(suite, origins, origin_count,
                               SCAN_LEX_DECL_NUMBER, item->prefix,
                               owned_copy(item->prefix), false);
        }
    }
    for (const scan_lex_recovery_t *recovery = spec->recoveries;
         (!suite->limit || suite->count < suite->limit)
             && recovery && recovery->token.data; recovery++) {
        core_string_t malformed = {0};
        core_string_t spelling = recovery->token;
        scan_lex_decl_kind_t declaration = SCAN_LEX_DECL_RECOVERY;
        for (const scan_lex_string_t *item = spec->strings;
             !malformed.data && item && item->opener.data; item++) {
            if (core_string_eq(item->tok, recovery->token)) {
                malformed = malformed_string(item);
                spelling = item->opener;
                declaration = SCAN_LEX_DECL_STRING;
            }
        }
        if (!malformed.data
            && (core_string_eq(spec->number.tok_int, recovery->token)
                || core_string_eq(spec->number.tok_float, recovery->token))) {
            malformed = core_string_from_cstr("1__x");
            spelling = recovery->token;
            declaration = SCAN_LEX_DECL_RECOVERY;
        }
        core_string_t recovered = append_sync(malformed, recovery);
        core_free(malformed.data);
        if (recovered.data) {
            add_malformed_case(suite, origins, origin_count,
                               declaration, spelling, recovered, true);
        }
    }
    core_free(origins);

    qsort(suite->cases, suite->count, sizeof(*suite->cases), case_order);
    for (size_t i = 0; i < suite->count; i++) {
        suite->cases[i].id = i;
    }

    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < suite->count; i++) {
        const scan_lex_conformance_case_t *item = &suite->cases[i];
        hash = hash_bytes(hash, &item->kind, sizeof(item->kind));
        hash = hash_bytes(hash, &item->declaration, sizeof(item->declaration));
        hash = hash_bytes(hash, item->spelling.data, item->spelling.u8_bytes);
        hash = hash_bytes(hash, item->origin.data, item->origin.u8_bytes);
        hash = hash_bytes(hash, item->source.data, item->source.u8_bytes);
        hash = hash_bytes(hash, &item->input_bytes, sizeof(item->input_bytes));
        hash = hash_bytes(hash, &item->split_byte, sizeof(item->split_byte));
        hash = hash_bytes(hash, &item->expected_status,
                          sizeof(item->expected_status));
        hash = hash_bytes(hash, &item->recovery, sizeof(item->recovery));
    }
    suite->fingerprint = hash;
    return suite;
}

size_t
scan_lex_conformance_count(const scan_lex_conformance_suite_t *suite)
{
    return suite ? suite->count : 0;
}

bool
scan_lex_conformance_case(const scan_lex_conformance_suite_t *suite,
                          size_t index, scan_lex_conformance_case_t *out)
{
    if (!suite || !out || index >= suite->count) {
        return false;
    }
    *out = suite->cases[index];
    return true;
}

uint64_t
scan_lex_conformance_fingerprint(const scan_lex_conformance_suite_t *suite)
{
    return suite ? suite->fingerprint : 0;
}

void
scan_lex_conformance_free(scan_lex_conformance_suite_t *suite)
{
    if (!suite) {
        return;
    }
    for (size_t i = 0; i < suite->source_count; i++) {
        core_free(suite->sources[i].data);
    }
    core_free(suite->sources);
    core_free(suite->cases);
    core_free(suite);
}
