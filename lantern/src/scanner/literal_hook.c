#include "scanner/lex.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"

#include <pthread.h>
#include <string.h>

typedef struct {
    core_string_t            name;
    scan_lex_literal_hook_fn fn;
    core_dict_t              cache;
    uint64_t                 generation;
} literal_hook_t;

core_list_decl(literal_hook_t);

static core_list_t(literal_hook_t) hooks;
static pthread_once_t hooks_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t hooks_lock = PTHREAD_MUTEX_INITIALIZER;

static void
literal_hooks_init(void)
{
    hooks = core_list_new(literal_hook_t);
}

void
scan_lex_literal_result_free(scan_lex_literal_result_t *result)
{
    if (!result) {
        return;
    }

    core_free(result->value.data);
    core_free(result->token.data);
    core_free(result->type.data);
    core_free(result->diagnostic.data);
    *result = (scan_lex_literal_result_t){0};
}

static scan_lex_literal_result_t *
literal_result_dup(const scan_lex_literal_result_t *source)
{
    scan_lex_literal_result_t *copy = core_alloc(scan_lex_literal_result_t);

    *copy = *source;
    copy->value      = core_string_dup(source->value);
    copy->token      = core_string_dup(source->token);
    copy->type       = core_string_dup(source->type);
    copy->diagnostic = core_string_dup(source->diagnostic);
    return copy;
}

static void
literal_result_copy_out(scan_lex_literal_result_t       *out,
                        const scan_lex_literal_result_t *source)
{
    *out = *source;
    out->value      = core_string_dup(source->value);
    out->token      = core_string_dup(source->token);
    out->type       = core_string_dup(source->type);
    out->diagnostic = core_string_dup(source->diagnostic);
}

static void
literal_cache_clear(literal_hook_t *hook)
{
    core_dict_foreach(&hook->cache, item) {
        core_string_t *key = item->key;
        scan_lex_literal_result_t *value = item->value;

        core_free(key->data);
        core_free(key);
        scan_lex_literal_result_free(value);
        core_free(value);
    }
    core_dict_free(&hook->cache);
    core_dict_init_string(&hook->cache);
}

static literal_hook_t *
literal_hook_find(core_string_t name)
{
    for (size_t i = 0; i < core_list_len(hooks); i++) {
        if (core_string_eq(hooks.data[i].name, name)) {
            return &hooks.data[i];
        }
    }
    return nullptr;
}

bool
scan_lex_literal_hook_register_pure(core_string_t name,
                                    scan_lex_literal_hook_fn fn)
{
    if (!name.data || !name.u8_bytes || !fn) {
        return false;
    }

    pthread_once(&hooks_once, literal_hooks_init);
    pthread_mutex_lock(&hooks_lock);

    literal_hook_t *hook = literal_hook_find(name);
    if (hook) {
        hook->fn = fn;
        hook->generation++;
        literal_cache_clear(hook);
        pthread_mutex_unlock(&hooks_lock);
        return true;
    }

    literal_hook_t item = {
        .name = core_string_dup(name),
        .fn = fn,
        .generation = 1,
    };
    core_dict_init_string(&item.cache);
    core_list_push(hooks, item);
    pthread_mutex_unlock(&hooks_lock);
    return true;
}

bool
scan_lex_literal_hook_exists(core_string_t name)
{
    pthread_once(&hooks_once, literal_hooks_init);
    pthread_mutex_lock(&hooks_lock);
    bool found = literal_hook_find(name) != nullptr;
    pthread_mutex_unlock(&hooks_lock);
    return found;
}

static core_string_t
literal_cache_key(const scan_lex_literal_input_t *input)
{
    core_buffer_t *buffer = core_buffer_empty();
    size_t lengths[] = {
        input->token.u8_bytes,
        input->modifier.u8_bytes,
        input->spelling.u8_bytes,
    };

    core_buffer_append(buffer, (const char *)lengths, sizeof(lengths));
    core_buffer_append(buffer, input->token.data, input->token.u8_bytes);
    core_buffer_append(buffer, input->modifier.data, input->modifier.u8_bytes);
    core_buffer_append(buffer, input->spelling.data, input->spelling.u8_bytes);

    size_t len = buffer->byte_len;
    return (core_string_t){.data = core_buffer_take(buffer), .u8_bytes = len};
}

static scan_lex_literal_result_t
literal_result_own(scan_lex_literal_borrowed_result_t borrowed,
                   size_t spelling_bytes)
{
    scan_lex_literal_result_t result = {
        .accepted = borrowed.accepted,
        .value = core_string_dup(borrowed.borrowed_value),
        .token = core_string_dup(borrowed.borrowed_token),
        .type = core_string_dup(borrowed.borrowed_type),
        .diagnostic = core_string_dup(borrowed.borrowed_diagnostic),
        .diagnostic_start = borrowed.diagnostic_start,
        .diagnostic_end = borrowed.diagnostic_end,
    };

    if (result.diagnostic_start > spelling_bytes
        || result.diagnostic_end < result.diagnostic_start
        || result.diagnostic_end > spelling_bytes) {
        scan_lex_literal_result_free(&result);
        result.accepted = false;
        result.diagnostic = core_string_from_cstr(
            "literal hook returned an invalid diagnostic span");
        result.diagnostic_end = spelling_bytes;
    }
    if (!result.accepted && !result.diagnostic.data) {
        result.diagnostic = core_string_from_cstr("literal hook rejected input");
        result.diagnostic_end = spelling_bytes;
    }
    return result;
}

bool
scan_lex_literal_hook_call(core_string_t name,
                           const scan_lex_literal_input_t *input,
                           scan_lex_literal_result_t *result)
{
    if (!name.data || !input || !result || !input->spelling.data) {
        return false;
    }

    *result = (scan_lex_literal_result_t){0};
    pthread_once(&hooks_once, literal_hooks_init);
    core_string_t key = literal_cache_key(input);
    pthread_mutex_lock(&hooks_lock);

    literal_hook_t *hook = literal_hook_find(name);
    if (!hook) {
        pthread_mutex_unlock(&hooks_lock);
        core_free(key.data);
        return false;
    }

    bool found = false;
    scan_lex_literal_result_t *cached = core_dict_get(&hook->cache, &key,
                                                       &found);
    if (found) {
        literal_result_copy_out(result, cached);
        core_free(key.data);
        pthread_mutex_unlock(&hooks_lock);
        return true;
    }

    scan_lex_literal_hook_fn fn = hook->fn;
    uint64_t generation = hook->generation;
    pthread_mutex_unlock(&hooks_lock);

    scan_lex_literal_borrowed_result_t borrowed = fn(input);
    scan_lex_literal_result_t owned = literal_result_own(
        borrowed, input->spelling.u8_bytes);

    if (borrowed.cacheable) {
        pthread_mutex_lock(&hooks_lock);
        hook = literal_hook_find(name);
        found = false;
        if (hook && hook->fn == fn && hook->generation == generation) {
            core_dict_get(&hook->cache, &key, &found);
        }
        if (hook && hook->fn == fn && hook->generation == generation
            && !found) {
            core_string_t *stored_key = core_alloc(core_string_t);
            *stored_key = key;
            scan_lex_literal_result_t *stored = literal_result_dup(&owned);
            core_dict_put(&hook->cache, stored_key, stored);
            key = (core_string_t){0};
        }
        pthread_mutex_unlock(&hooks_lock);
    }
    core_free(key.data);

    *result = owned;
    return true;
}
