#include "parse/source.h"
#include "lib/alloc.h"
#include "unicode/encoding.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define SOURCE_CHUNK_BITS 10
#define SOURCE_CHUNK_SIZE (1u << SOURCE_CHUNK_BITS)
#define SOURCE_CHUNK_COUNT 4096u

typedef struct parse_source_t {
    _Atomic(char *)  text;
    _Atomic(size_t)  length;
    size_t           capacity;
    _Atomic(char *)  values;
    size_t           values_length;
    size_t           values_capacity;
    char           **retired;
    size_t           retired_count;
    size_t           retired_capacity;
    core_option_t(core_string_t) name;
    uint32_t        *line_starts;
    size_t           line_count;
    size_t           line_capacity;
    size_t           lines_scanned;
    pthread_mutex_t  lock;
    _Atomic(uint32_t) refs;
    uint32_t         id;
} parse_source_t;

typedef _Atomic(parse_source_t *) source_slot_t;

static _Atomic(source_slot_t *) source_chunks[SOURCE_CHUNK_COUNT];
static pthread_mutex_t          source_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t                 source_next_id = 1;

static source_slot_t *
source_slot(uint32_t id, bool create)
{
    uint32_t chunk = id >> SOURCE_CHUNK_BITS;
    if (!id || chunk >= SOURCE_CHUNK_COUNT) return nullptr;
    source_slot_t *slots = atomic_load_explicit(&source_chunks[chunk],
                                                memory_order_acquire);
    if (!slots && create) {
        slots = core_alloc_array(source_slot_t, SOURCE_CHUNK_SIZE);
        atomic_store_explicit(&source_chunks[chunk], slots,
                              memory_order_release);
    }
    return slots ? &slots[id & (SOURCE_CHUNK_SIZE - 1)] : nullptr;
}

static parse_source_t *
source_get(uint32_t id)
{
    source_slot_t *slot = source_slot(id, false);
    return slot ? atomic_load_explicit(slot, memory_order_acquire) : nullptr;
}

uint32_t
parse_source_new(core_option_t(core_string_t) name)
{
    parse_source_t *source = core_alloc(parse_source_t);
    pthread_mutex_init(&source->lock, nullptr);
    atomic_store(&source->refs, 1);
    if (core_option_is_set(name)) {
        source->name = core_option_set(core_string_t,
                                       core_string_dup(core_option_get(name)));
    }
    source->line_starts = core_alloc_array(uint32_t, 16);
    source->line_capacity = 16;
    source->line_count = 1;

    pthread_mutex_lock(&source_registry_lock);
    uint32_t id = source_next_id;
    source_slot_t *slot = source_slot(id, true);
    if (slot) {
        source_next_id++;
        source->id = id;
        atomic_store_explicit(slot, source, memory_order_release);
    }
    pthread_mutex_unlock(&source_registry_lock);

    if (!slot) {
        pthread_mutex_destroy(&source->lock);
        core_free(source->line_starts);
        if (core_option_is_set(source->name))
            core_free(core_option_get(source->name).data);
        core_free(source);
        return 0;
    }
    return id;
}

void
parse_source_retain(uint32_t id)
{
    parse_source_t *source = source_get(id);
    if (source) atomic_fetch_add(&source->refs, 1);
}

void
parse_source_release(uint32_t id)
{
    parse_source_t *source = source_get(id);
    if (!source || atomic_fetch_sub(&source->refs, 1) != 1) return;

    pthread_mutex_lock(&source_registry_lock);
    atomic_store_explicit(source_slot(id, false), nullptr,
                          memory_order_release);
    pthread_mutex_unlock(&source_registry_lock);

    for (size_t i = 0; i < source->retired_count; i++)
        core_free(source->retired[i]);
    core_free(source->retired);
    core_free(atomic_load(&source->text));
    core_free(atomic_load(&source->values));
    core_free(source->line_starts);
    if (core_option_is_set(source->name))
        core_free(core_option_get(source->name).data);
    pthread_mutex_destroy(&source->lock);
    core_free(source);
}

static void
retire_text(parse_source_t *source, char *text)
{
    if (!text) return;
    if (source->retired_count == source->retired_capacity) {
        size_t capacity = source->retired_capacity
            ? source->retired_capacity * 2 : 4;
        source->retired = core_realloc(source->retired,
                                       capacity * sizeof(char *));
        source->retired_capacity = capacity;
    }
    source->retired[source->retired_count++] = text;
}

static void
truncate_lines(parse_source_t *source, size_t offset)
{
    while (source->line_count > 1
           && source->line_starts[source->line_count - 1] > offset)
        source->line_count--;
    size_t start = source->line_starts[source->line_count - 1];
    if (source->lines_scanned > start) source->lines_scanned = start;
}

bool
parse_source_append(uint32_t id, const char *data, size_t len)
{
    parse_source_t *source = source_get(id);
    if (!source) return false;
    if (!len) return true;

    pthread_mutex_lock(&source->lock);
    char  *text = atomic_load(&source->text);
    size_t length = atomic_load(&source->length);
    if (length + len + 1 > source->capacity) {
        size_t capacity = source->capacity ? source->capacity * 2 : 256;
        while (capacity < length + len + 1) capacity *= 2;
        char *grown = core_alloc_size(capacity, 1);
        if (length) memcpy(grown, text, length);
        retire_text(source, text);
        text = grown;
        source->capacity = capacity;
        atomic_store_explicit(&source->text, text, memory_order_release);
    }
    memcpy(text + length, data, len);
    atomic_store_explicit(&source->length, length + len,
                          memory_order_release);
    pthread_mutex_unlock(&source->lock);
    return true;
}

bool
parse_source_replace(uint32_t id, size_t start, size_t end,
                     const char *data, size_t len)
{
    parse_source_t *source = source_get(id);
    if (!source || start > end || (len && !data)) return false;

    pthread_mutex_lock(&source->lock);
    char  *text = atomic_load(&source->text);
    size_t length = atomic_load(&source->length);
    if (end > length) {
        pthread_mutex_unlock(&source->lock);
        return false;
    }
    size_t updated = length - (end - start) + len;
    size_t capacity = source->capacity > updated + 1
        ? source->capacity : updated + 1;
    char *replaced = core_alloc_size(capacity, 1);
    if (start) memcpy(replaced, text, start);
    if (len) memcpy(replaced + start, data, len);
    if (length > end)
        memcpy(replaced + start + len, text + end, length - end);
    retire_text(source, text);
    source->capacity = capacity;
    truncate_lines(source, start);
    atomic_store_explicit(&source->text, replaced, memory_order_release);
    atomic_store_explicit(&source->length, updated, memory_order_release);
    pthread_mutex_unlock(&source->lock);
    return true;
}

bool
parse_source_store_value(uint32_t id, size_t start, size_t end,
                         uint32_t *offset)
{
    parse_source_t *source = source_get(id);
    if (!source || start > end) return false;

    pthread_mutex_lock(&source->lock);
    size_t length = atomic_load(&source->length);
    size_t needed = source->values_length + (end - start) + 1;
    if (end > length || needed > UINT32_MAX) {
        pthread_mutex_unlock(&source->lock);
        return false;
    }
    char *values = atomic_load(&source->values);
    if (needed > source->values_capacity) {
        size_t capacity = source->values_capacity
            ? source->values_capacity * 2 : 1024;
        while (capacity < needed) capacity *= 2;
        char *grown = core_alloc_size(capacity, 1);
        if (source->values_length)
            memcpy(grown, values, source->values_length);
        retire_text(source, values);
        values = grown;
        source->values_capacity = capacity;
        atomic_store_explicit(&source->values, values, memory_order_release);
    }
    const char *text = atomic_load(&source->text);
    *offset = (uint32_t)source->values_length;
    if (end > start) memcpy(values + *offset, text + start, end - start);
    values[*offset + (end - start)] = '\0';
    source->values_length = needed;
    pthread_mutex_unlock(&source->lock);
    return true;
}

const char *
parse_source_value(uint32_t id, uint32_t offset)
{
    parse_source_t *source = source_get(id);
    if (!source) return nullptr;
    char *values = atomic_load_explicit(&source->values, memory_order_acquire);
    return values ? values + offset : nullptr;
}

size_t
parse_source_length(uint32_t id)
{
    parse_source_t *source = source_get(id);
    return source ? atomic_load_explicit(&source->length,
                                         memory_order_acquire) : 0;
}

bool
parse_source_slice(uint32_t id, size_t start, size_t end, core_string_t *out)
{
    parse_source_t *source = source_get(id);
    if (!source || start > end) return false;
    size_t length = atomic_load_explicit(&source->length,
                                         memory_order_acquire);
    if (end > length) return false;
    char *text = atomic_load_explicit(&source->text, memory_order_acquire);
    if (out) {
        *out = end > start
            ? (core_string_t){.data = text + start, .u8_bytes = end - start}
            : (core_string_t){0};
    }
    return true;
}

core_option_t(core_string_t)
parse_source_name(uint32_t id)
{
    parse_source_t *source = source_get(id);
    return source ? source->name : core_option_none(core_string_t);
}

static bool
line_terminator(int32_t cp)
{
    return cp == '\n' || cp == 0x2028;
}

static void
extend_lines(parse_source_t *source, const char *text, size_t limit)
{
    uint32_t pos = (uint32_t)source->lines_scanned;
    while (pos < limit) {
        int32_t cp = unicode_utf8_decode(text, (uint32_t)limit, &pos);
        if (cp < 0) {
            pos++;
            continue;
        }
        if (!line_terminator(cp)) continue;
        if (source->line_count == source->line_capacity) {
            source->line_capacity *= 2;
            source->line_starts = core_realloc(
                source->line_starts,
                source->line_capacity * sizeof(uint32_t));
        }
        source->line_starts[source->line_count++] = pos;
    }
    if (pos > source->lines_scanned) source->lines_scanned = pos;
}

bool
parse_source_position(uint32_t id, size_t offset, uint32_t *line,
                      uint32_t *column)
{
    parse_source_t *source = source_get(id);
    if (!source) return false;

    pthread_mutex_lock(&source->lock);
    const char *text = atomic_load(&source->text);
    size_t length = atomic_load(&source->length);
    if (offset > length) {
        pthread_mutex_unlock(&source->lock);
        return false;
    }
    if (source->lines_scanned < offset) extend_lines(source, text, offset);

    size_t low = 0;
    size_t high = source->line_count;
    while (high - low > 1) {
        size_t mid = low + (high - low) / 2;
        if (source->line_starts[mid] <= offset) low = mid;
        else high = mid;
    }

    uint32_t count = 1;
    uint32_t pos = source->line_starts[low];
    while (pos < offset) {
        if (unicode_utf8_decode(text, (uint32_t)length, &pos) < 0) pos++;
        count++;
    }
    pthread_mutex_unlock(&source->lock);

    if (line) *line = (uint32_t)low + 1;
    if (column) *column = count;
    return true;
}
