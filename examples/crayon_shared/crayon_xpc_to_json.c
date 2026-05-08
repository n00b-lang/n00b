/**
 * @file crayon_xpc_to_json.c
 * @brief xpc_object_t → n00b_json_node_t recursive walker.
 */

#ifdef __APPLE__

#include <stdint.h>
#include <limits.h>
#include <string.h>

#include "crayon_xpc_to_json.h"
#include "crayon_xpc_apply.h"

// Per-entry callback used by `crayon_xpc_dict_apply`; ctx is the
// destination json object node.
static bool
dict_apply_step(const char *key, xpc_object_t value, void *ctx)
{
    n00b_json_node_t *o  = (n00b_json_node_t *)ctx;
    n00b_json_node_t *cv = crayon_xpc_to_json(value);
    if (cv) n00b_json_object_put(o, key, cv);
    return true;
}

n00b_json_node_t *
crayon_xpc_to_json(xpc_object_t obj)
{
    if (!obj) return NULL;
    xpc_type_t t = xpc_get_type(obj);

    if (t == XPC_TYPE_NULL) {
        return n00b_json_null_new();
    }
    if (t == XPC_TYPE_BOOL) {
        return n00b_json_bool_new(xpc_bool_get_value(obj));
    }
    if (t == XPC_TYPE_INT64) {
        return n00b_json_int_new(xpc_int64_get_value(obj));
    }
    if (t == XPC_TYPE_UINT64) {
        uint64_t u = xpc_uint64_get_value(obj);
        // Clamp to int64 max — JSON itself has no unsigned; downstream
        // consumers use int64_t for counts and timestamps in nanos.
        int64_t  v = (u > (uint64_t)INT64_MAX) ? INT64_MAX : (int64_t)u;
        return n00b_json_int_new(v);
    }
    if (t == XPC_TYPE_DOUBLE) {
        return n00b_json_double_new(xpc_double_get_value(obj));
    }
    if (t == XPC_TYPE_STRING) {
        const char *s = xpc_string_get_string_ptr(obj);
        return n00b_json_string_new(s ? s : "");
    }
    if (t == XPC_TYPE_ARRAY) {
        n00b_json_node_t *arr  = n00b_json_array_new();
        size_t            n    = xpc_array_get_count(obj);
        for (size_t i = 0; i < n; i++) {
            xpc_object_t      child = xpc_array_get_value(obj, i);
            n00b_json_node_t *cv    = crayon_xpc_to_json(child);
            if (cv) n00b_json_array_push(arr, cv);
        }
        return arr;
    }
    if (t == XPC_TYPE_DICTIONARY) {
        n00b_json_node_t *o = n00b_json_object_new();
        // ObjC shim hides xpc_dictionary_apply's block under a plain
        // function pointer so ncc can compile this file.
        crayon_xpc_dict_apply(obj, dict_apply_step, o);
        return o;
    }
    // data / uuid / fd / shmem / endpoint / connection / error: not
    // representable as JSON; fall back to null so consumers can still
    // walk the dict.
    return n00b_json_null_new();
}

#endif // __APPLE__
