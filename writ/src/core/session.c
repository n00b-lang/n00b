#include "internal.h"

#include "lib/hash.h"

static core_hash_value_t
str_hash(void *key)
{
    const writ_str_t *text = key;
    return core_hash_raw(text->data, text->length);
}

static bool
str_key_eq(const void *a, const void *b)
{
    return writ_str_eq(*(const writ_str_t *)a, *(const writ_str_t *)b);
}

uint32_t
writ_api_version(void)
{
    return WRIT_API_VERSION;
}

writ_session_t *
writ_session_new(const writ_session_config_t *config)
{
    writ_session_t *session = core_alloc(writ_session_t);
    core_arena_init(&session->arena, 0);
    session->strings     = core_indexed_table_new(1, UINT32_MAX - 1, str_hash, str_key_eq);
    session->sources     = core_list_new(writ_source_t);
    session->diagnostics = core_diagnostics_new();
    session->diags       = core_list_new(writ_diag_record_t);
    session->cleanups    = core_list_new(writ_cleanup_t);
    if (config && config->struct_size >= offsetof(writ_session_config_t, llvm_prefix)) {
        session->runner = config->process_runner;
    }
    return session;
}

void
writ_session_free(writ_session_t *session)
{
    if (!session) {
        return;
    }
    for (size_t i = session->cleanups.len; i > 0; i--) {
        session->cleanups.data[i - 1].release(session->cleanups.data[i - 1].object);
    }
    core_list_free(session->cleanups);
    core_list_foreach(session->sources, source) {
        core_list_free(source->line_starts);
    }
    core_list_foreach(session->diags, record) {
        core_list_free(record->secondary);
        core_list_free(record->suggestions);
    }
    core_list_free(session->sources);
    core_list_free(session->diags);
    core_diagnostics_free(session->diagnostics);
    core_indexed_table_free(session->strings);
    core_arena_free(&session->arena);
    core_free(session);
}

void
writ_on_session_free(writ_session_t *session, void (*release)(void *), void *object)
{
    core_list_push(session->cleanups, ((writ_cleanup_t){release, object}));
}

void *
writ_alloc(writ_session_t *session, size_t size)
{
    return core_arena_alloc(&session->arena, size ? size : 1);
}

writ_str_t
writ_str_copy(writ_session_t *session, const char *data, size_t length)
{
    char *copy = writ_alloc(session, length + 1);
    if (length) {
        memcpy(copy, data, length);
    }
    return (writ_str_t){copy, length};
}

writ_str_t
writ_str_format(writ_session_t *session, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    core_buffer_t *buffer = core_buffer_empty();
    core_buffer_vprintf(buffer, format, args);
    va_end(args);
    return writ_str_from_buffer(session, buffer);
}

writ_str_t
writ_str_from_buffer(writ_session_t *session, core_buffer_t *buffer)
{
    writ_str_t text = writ_str_copy(session, buffer->data, buffer->byte_len);
    core_buffer_free(buffer);
    return text;
}

bool
writ_str_eq(writ_str_t a, writ_str_t b)
{
    return a.length == b.length && (!a.length || !memcmp(a.data, b.data, a.length));
}

writ_str_t
writ_cstr(const char *text)
{
    return (writ_str_t){text, text ? strlen(text) : 0};
}
