/**
 * @file crayon_xpc_apply.m
 * @brief ObjC implementation; lets ncc-compiled C files iterate xpc dicts.
 */

#ifdef __APPLE__

#include "crayon_xpc_apply.h"

void
crayon_xpc_dict_apply(xpc_object_t            dict,
                      crayon_xpc_dict_iter_fn fn,
                      void                   *ctx)
{
    if (!dict || !fn) return;
    xpc_dictionary_apply(dict, ^bool(const char *key, xpc_object_t value) {
        return fn(key, value, ctx);
    });
}

#endif // __APPLE__
