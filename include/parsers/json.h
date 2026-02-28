/**
 * @file json.h
 * @brief JSON value types, recursive descent parser, and encoder.
 *
 * Provides:
 * - Tagged-union `n00b_json_node_t` value type
 * - Recursive descent parser (`n00b_json_parse`)
 * - JSON encoder (`n00b_json_encode`)
 * - Constructors and accessors for building/querying value trees
 *
 * ### Usage
 *
 * ```c
 * auto r = n00b_json_parse("{\"key\": 42}", 11);
 * if (n00b_result_is_ok(r)) {
 *     n00b_json_node_t *root = n00b_result_get(r);
 *     // ...
 * }
 * ```
 */
#pragma once

#include "n00b.h"
#include "core/alloc.h"
#include "adt/result.h"
#include "adt/list.h"
#include "adt/dict_untyped.h"

// ============================================================================
// JSON value type
// ============================================================================

/**
 * @brief JSON value type tags.
 */
typedef enum {
    N00B_JSON_NULL,    /**< JSON null. */
    N00B_JSON_BOOL,    /**< JSON boolean. */
    N00B_JSON_INT,     /**< JSON integer (int64_t). */
    N00B_JSON_DOUBLE,  /**< JSON floating-point (double). */
    N00B_JSON_STRING,  /**< JSON string. */
    N00B_JSON_ARRAY,   /**< JSON array. */
    N00B_JSON_OBJECT,  /**< JSON object. */
} n00b_json_type_t;

typedef struct n00b_json_node n00b_json_node_t;

n00b_list_decl(n00b_json_node_t *);

/**
 * @brief JSON value — tagged union of all JSON types.
 *
 * All nodes are GC-allocated.  Strings are stored as `char *`
 * (NUL-terminated, GC-allocated).  Arrays use `n00b_list_t` by value.
 * Objects are `n00b_dict_untyped_t` with string keys.
 */
struct n00b_json_node {
    n00b_json_type_t type; /**< Value type tag. */
    union {
        bool                              boolean; /**< Boolean value. */
        int64_t                           integer; /**< Integer value. */
        double                            number;  /**< Floating-point value. */
        char                             *string;  /**< NUL-terminated string. */
        n00b_list_t(n00b_json_node_t *)   array;   /**< List of n00b_json_node_t *. */
        n00b_dict_untyped_t              *object;  /**< Dict of string -> n00b_json_node_t *. */
    };
};

// ============================================================================
// Constructors
// ============================================================================

n00b_json_node_t *n00b_json_null_new(void);
n00b_json_node_t *n00b_json_bool_new(bool val);
n00b_json_node_t *n00b_json_int_new(int64_t val);
n00b_json_node_t *n00b_json_double_new(double val);
n00b_json_node_t *n00b_json_string_new(const char *val);
n00b_json_node_t *n00b_json_array_new(void);
n00b_json_node_t *n00b_json_object_new(void);

// ============================================================================
// Mutation
// ============================================================================

void   n00b_json_array_push(n00b_json_node_t *arr, n00b_json_node_t *val);
void   n00b_json_object_put(n00b_json_node_t *obj, const char *key,
                             n00b_json_node_t *val);
size_t n00b_json_length(const n00b_json_node_t *val);

// ============================================================================
// Type query
// ============================================================================

static inline bool n00b_json_is_null(const n00b_json_node_t *n)
{
    return !n || n->type == N00B_JSON_NULL;
}

static inline bool n00b_json_is_bool(const n00b_json_node_t *n)
{
    return n && n->type == N00B_JSON_BOOL;
}

static inline bool n00b_json_is_int(const n00b_json_node_t *n)
{
    return n && n->type == N00B_JSON_INT;
}

static inline bool n00b_json_is_double(const n00b_json_node_t *n)
{
    return n && n->type == N00B_JSON_DOUBLE;
}

static inline bool n00b_json_is_string(const n00b_json_node_t *n)
{
    return n && n->type == N00B_JSON_STRING;
}

static inline bool n00b_json_is_array(const n00b_json_node_t *n)
{
    return n && n->type == N00B_JSON_ARRAY;
}

static inline bool n00b_json_is_object(const n00b_json_node_t *n)
{
    return n && n->type == N00B_JSON_OBJECT;
}

// ============================================================================
// Standalone parse/encode API
// ============================================================================

/**
 * @brief Parse a JSON string into a value tree.
 *
 * @param input     JSON text.
 * @param input_len Length in bytes.
 * @param err_out   If non-null, stores an error message on failure.
 *
 * @return Parsed value, or nullptr on error.
 */
n00b_json_node_t *n00b_json_parse(const char *input, size_t input_len,
                                   const char **err_out);

/**
 * @brief Encode a JSON value tree to text.
 *
 * @param val The root value to encode.
 * @kw pretty  Enable indented output (default false).
 * @kw indent  Indent width in spaces (default 2).
 *
 * @return NUL-terminated JSON text, or nullptr on error.
 */
extern char *n00b_json_encode(const n00b_json_node_t *val) _kargs {
    bool pretty = false;
    int  indent = 2;
};
