#ifndef N00BB_QUALIFIED_NAMES_H
#define N00BB_QUALIFIED_NAMES_H

#include "lib/string.h"

static inline core_string_t
n00bb_name_leaf(core_string_t qualified)
{
    int64_t dot = qualified.data ? core_string_rfind_byte(qualified, '.')
                                 : -1;
    return dot < 0 ? qualified
        : core_string_slice(qualified, (size_t)dot + 1,
                            qualified.u8_bytes - (size_t)dot - 1);
}

static inline bool
n00bb_name_member(core_string_t qualified, core_string_t prefix,
                  core_string_t *member)
{
    if (qualified.u8_bytes <= prefix.u8_bytes + 1
        || qualified.data[prefix.u8_bytes] != '.'
        || !core_string_starts_with(qualified, prefix))
        return false;
    *member = core_string_slice(qualified, prefix.u8_bytes + 1,
        qualified.u8_bytes - prefix.u8_bytes - 1);
    return true;
}

static inline core_string_t
n00bb_function_root(core_string_t name)
{
    int64_t mark = name.data ? core_string_find_byte(name, '#') : -1;
    return mark < 0 ? name : core_string_slice(name, 0, (size_t)mark);
}

static inline bool
n00bb_names_function(core_string_t root, core_string_t function)
{
    return core_string_eq(n00bb_function_root(function), root);
}

static inline bool
n00bb_generic_generated_finalizer(core_string_t name)
{
    return name.data
        && core_string_find(name, CORE_STRING_STATIC("#n00bb_generic_")) >= 0;
}

#endif
