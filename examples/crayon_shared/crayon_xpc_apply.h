/**
 * @file crayon_xpc_apply.h
 * @brief Plain-C wrapper around `xpc_dictionary_apply` so ncc-compiled
 *        translation units don't need block syntax.
 */
#pragma once

#ifdef __APPLE__

#include <stdbool.h>
#include <xpc/xpc.h>

/**
 * @brief Per-entry callback fired by @ref crayon_xpc_dict_apply.
 *
 * Return false to stop iteration; true to continue.
 */
typedef bool (*crayon_xpc_dict_iter_fn)(const char  *key,
                                        xpc_object_t value,
                                        void        *ctx);

/**
 * @brief Iterate over a dict's entries with a plain-C callback.
 */
void crayon_xpc_dict_apply(xpc_object_t            dict,
                           crayon_xpc_dict_iter_fn fn,
                           void                   *ctx);

#endif // __APPLE__
