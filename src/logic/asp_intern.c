#include "logic/asp_intern.h"

#include <stdio.h>
#include <string.h>

#define INTERN_INIT_CAP 64

void
n00b_dl_intern_init(n00b_dl_intern_t *intern)
{
    n00b_dl_str_i64_map_init(&intern->name_to_id);
    n00b_dl_i64_str_map_init(&intern->var_id_to_name);
    intern->id_to_name = n00b_list_new_cap_private(n00b_string_t *, INTERN_INIT_CAP);
    intern->next_var   = N00B_DL_VAR_BASE;
}

void
n00b_dl_intern_free(n00b_dl_intern_t *intern)
{
    n00b_list_free(intern->id_to_name);

    n00b_dl_i64_str_map_free(&intern->var_id_to_name);
    n00b_dl_str_i64_map_free(&intern->name_to_id);
}

n00b_dl_sym_t
n00b_dl_intern(n00b_dl_intern_t *intern, n00b_string_t *name)
{
    int64_t *existing = n00b_dl_str_i64_map_get(&intern->name_to_id, name);
    if (existing) {
        return *existing;
    }

    n00b_dl_sym_t id = (n00b_dl_sym_t)n00b_list_len(intern->id_to_name);

    n00b_list_push(intern->id_to_name, name);
    n00b_dl_str_i64_map_put(&intern->name_to_id, name, (int64_t)id);

    return id;
}

n00b_option_t(n00b_dl_sym_t)
n00b_dl_intern_lookup(n00b_dl_intern_t *intern, n00b_string_t *name)
{
    int64_t *existing = n00b_dl_str_i64_map_get(&intern->name_to_id, name);
    if (existing) {
        return n00b_option_set(n00b_dl_sym_t, (n00b_dl_sym_t)*existing);
    }
    return n00b_option_none(n00b_dl_sym_t);
}

n00b_string_t *
n00b_dl_intern_name(n00b_dl_intern_t *intern, n00b_dl_sym_t id)
{
    if (n00b_dl_is_var(id)) {
        n00b_string_t **namep = n00b_dl_i64_str_map_get(
            &intern->var_id_to_name, (int64_t)id);
        if (namep) {
            return *namep;
        }
        return n00b_string_empty();
    }
    if (id < 0 || (size_t)id >= n00b_list_len(intern->id_to_name)) {
        return n00b_string_empty();
    }
    return n00b_list_get(intern->id_to_name, id);
}

n00b_dl_sym_t
n00b_dl_intern_var(n00b_dl_intern_t *intern, n00b_string_t *name)
{
    int64_t *existing = n00b_dl_str_i64_map_get(&intern->name_to_id, name);
    if (existing) {
        return (n00b_dl_sym_t)*existing;
    }

    n00b_dl_sym_t id = intern->next_var;
    intern->next_var--;

    n00b_dl_str_i64_map_put(&intern->name_to_id, name, (int64_t)id);
    n00b_dl_i64_str_map_put(&intern->var_id_to_name, (int64_t)id, name);

    return id;
}

n00b_dl_sym_t
n00b_dl_intern_int(n00b_dl_intern_t *intern, int64_t value)
{
    // Determine how many bytes we need for "#<value>\0".
    int needed = snprintf(nullptr, 0, "#%lld", (long long)value);
    if (needed < 0) {
        return N00B_DL_SYM_INVALID;
    }
    char tmp[(size_t)needed + 1];
    snprintf(tmp, (size_t)needed + 1, "#%lld", (long long)value);
    n00b_string_t *s = n00b_string_from_raw(tmp, needed);
    return n00b_dl_intern(intern, s);
}
