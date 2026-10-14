#include "internal.h"

writ_sym_t
writ_intern(writ_session_t *session, const char *data, size_t length)
{
    writ_str_t probe = {data, length};
    const core_indexed_entry_t *found = core_indexed_table_find(session->strings, &probe);
    if (found) {
        return found->index;
    }
    writ_str_t *key = writ_alloc(session, sizeof(writ_str_t));
    *key = writ_str_copy(session, data, length);
    return core_indexed_table_intern(session->strings, key, key, nullptr)->index;
}

writ_sym_t
writ_intern_str(writ_session_t *session, writ_str_t text)
{
    return writ_intern(session, text.data, text.length);
}

writ_str_t
writ_sym_str(const writ_session_t *session, writ_sym_t sym)
{
    const core_indexed_entry_t *entry = sym ? core_indexed_table_at(session->strings, sym) : nullptr;
    return entry ? *(const writ_str_t *)entry->key : (writ_str_t){"", 0};
}

static bool
starts_word(writ_str_t name, size_t i)
{
    char c = name.data[i];
    if (i == 0 || c == '_' || !(c >= 'A' && c <= 'Z')) {
        return false;
    }
    char before = name.data[i - 1];
    bool next_lower = i + 1 < name.length && name.data[i + 1] >= 'a' && name.data[i + 1] <= 'z';
    return (before >= 'a' && before <= 'z') || (before >= '0' && before <= '9')
        || (before >= 'A' && before <= 'Z' && next_lower);
}

writ_str_t
writ_name_case(writ_session_t *session, writ_str_t name, writ_case_t style)
{
    if (style == WRIT_CASE_KEEP) {
        return name;
    }
    core_buffer_t *buffer = core_buffer_empty();
    bool word_start = true;
    for (size_t i = 0; i < name.length; i++) {
        char c = name.data[i];
        if (c == '_') {
            word_start = true;
            continue;
        }
        if (starts_word(name, i)) {
            word_start = true;
        }
        bool lower = c >= 'a' && c <= 'z';
        bool upper = c >= 'A' && c <= 'Z';
        bool capital = style == WRIT_CASE_UPPER
                    || (word_start && (style == WRIT_CASE_PASCAL || (style == WRIT_CASE_CAMEL && buffer->byte_len)));
        if (word_start && buffer->byte_len && (style == WRIT_CASE_SNAKE || style == WRIT_CASE_UPPER)) {
            core_buffer_putc(buffer, '_');
        }
        core_buffer_putc(buffer, capital && lower ? (char)(c - 32) : !capital && upper ? (char)(c + 32) : c);
        word_start = false;
    }
    return writ_str_from_buffer(session, buffer);
}
