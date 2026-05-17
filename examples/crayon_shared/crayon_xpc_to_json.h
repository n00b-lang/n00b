/**
 * @file crayon_xpc_to_json.h
 * @brief Convert an `xpc_object_t` event tree into an `n00b_json_node_t`.
 *
 * The warehouse delivers events as native XPC dicts; downstream the demo
 * wants to work in n00b types so it can reuse n00b's accessors,
 * persistence, and display.  This bridge walks the xpc tree once and
 * produces a fully-owned n00b JSON value.
 */
#pragma once

#ifdef __APPLE__

#include <xpc/xpc.h>
#include "parsers/json.h"

/**
 * @brief Convert an xpc value tree to a fresh n00b JSON value.
 *
 * Mapping:
 *   - `XPC_TYPE_DICTIONARY` → `N00B_JSON_OBJECT`
 *   - `XPC_TYPE_ARRAY`      → `N00B_JSON_ARRAY`
 *   - `XPC_TYPE_STRING`     → `N00B_JSON_STRING`
 *   - `XPC_TYPE_INT64`      → `N00B_JSON_INT`
 *   - `XPC_TYPE_UINT64`     → `N00B_JSON_INT`  (clamped to int64_t max)
 *   - `XPC_TYPE_BOOL`       → `N00B_JSON_BOOL`
 *   - `XPC_TYPE_DOUBLE`     → `N00B_JSON_DOUBLE`
 *   - `XPC_TYPE_NULL`       → `N00B_JSON_NULL`
 *   - everything else (data, uuid, fd, …) → `N00B_JSON_NULL`
 *
 * Returns nullptr only when @p obj is nullptr.
 */
n00b_json_node_t *crayon_xpc_to_json(xpc_object_t obj);

#endif // __APPLE__
