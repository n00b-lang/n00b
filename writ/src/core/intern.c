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
