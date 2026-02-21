/**
 * @file construct.h
 * @brief Composable type constructors for the type inference engine.
 *
 * @ingroup typecheck
 *
 * All constructors return `n00b_tc_type_t *`.  They allocate via `n00b_alloc`
 * and set the variant payload.  In Phase 1, constructors work without a
 * context; in Phase 2, they will take `n00b_tc_ctx_t *` as the first
 * parameter and register the type on the context's `all_types` list.
 *
 * ## Varargs conventions
 *
 * - `n00b_tc_param()`, `n00b_tc_sum()`, `n00b_tc_tuple()` take positional
 *   `n00b_tc_type_t *+` (ncc typed varargs) for their type arguments.
 * - `n00b_tc_fn()` takes positional `n00b_tc_type_t *+` for positional
 *   params, with `.returns`, `.vargs`, `.kargs` as `_kargs`.
 * - `n00b_tc_record()` takes `n00b_tc_field_t +` (ncc typed varargs)
 *   for fields, with `.ordered` and `.open` as `_kargs`.
 * - `n00b_tc_field()` returns `n00b_tc_field_t` by value (small struct).
 */
#pragma once

#include "typecheck/types.h"
#include "core/alloc.h"
#include "core/vargs.h"
#include "core/string.h"

/**
 * @brief Create a named type variable.
 *
 * Sets `given_name` from @p name.  Generates a `display_name` from the
 * given name (e.g., "T" for a var named "T").
 *
 * @param name  User-visible name (e.g., `*r"T"`).
 * @return      Allocated type node with Var kind.
 */
extern n00b_tc_type_t *n00b_tc_var(n00b_string_t name);

/**
 * @brief Create an anonymous type variable.
 *
 * `given_name` is `none`.  `display_name` is auto-generated (e.g., "t_0").
 *
 * @return Allocated type node with Var kind.
 */
extern n00b_tc_type_t *n00b_tc_fresh_var(void);

/**
 * @brief Create a primitive type.
 *
 * @param name  Interned name (e.g., `*r"int"`, `*r"bool"`).
 * @return      Allocated type node with Prim kind.
 */
extern n00b_tc_type_t *n00b_tc_prim(n00b_string_t name);

/**
 * @brief Create a parameterized type.
 *
 * @param name  Constructor name (e.g., `*r"list"`, `*r"dict"`).
 * @param ...   Type parameters as `n00b_tc_type_t *` positional varargs.
 * @return      Allocated type node with Param kind.
 *
 * @par Example
 * @code
 * auto list_int = n00b_tc_param(*r"list", t_int);
 * auto dict_kv  = n00b_tc_param(*r"dict", t_string, t_int);
 * @endcode
 */
extern n00b_tc_type_t *n00b_tc_param(n00b_string_t name, n00b_tc_type_t *+);

/**
 * @brief Create a function type.
 *
 * Positional parameters are passed as `n00b_tc_type_t *` varargs.
 * Use `_kargs` for optional components.
 *
 * @param ...   Positional parameter types as varargs.
 *
 * @kw returns   Return type (required).
 * @kw variadic  Variadic parameter type (nullptr if not variadic).
 * @kw kwonly    Keyword argument record type (nullptr, or Record with ordered=false).
 *
 * @return Allocated type node with Fn kind.
 *
 * @par Example
 * @code
 * auto fn = n00b_tc_fn(t_int, t_string, .returns = t_bool);
 * @endcode
 */
extern n00b_tc_type_t *n00b_tc_fn(n00b_tc_type_t *+)
    _kargs {
        n00b_tc_type_t *returns  = nullptr;
        n00b_tc_type_t *variadic = nullptr;
        n00b_tc_type_t *kwonly   = nullptr;
    };

/**
 * @brief Create a sum type.
 *
 * Variants are flattened (no nested sums) and stored in the provided order.
 *
 * @param ...  Variant types as `n00b_tc_type_t *` varargs.
 * @return     Allocated type node with Sum kind.
 *
 * @par Example
 * @code
 * auto nullable_int = n00b_tc_sum(t_int, t_nil);
 * @endcode
 */
extern n00b_tc_type_t *n00b_tc_sum(n00b_tc_type_t *+);

/**
 * @brief Create a record type.
 *
 * Fields are passed as `n00b_tc_field_t *` varargs (use `n00b_tc_field()`
 * to build them).
 *
 * @param name  Record name (empty string for anonymous).
 * @param ...   Fields as `n00b_tc_field_t *` varargs.
 *
 * @kw ordered  true = struct layout (default); false = keyword bag.
 * @kw open     true = open row (duck typing); false = closed (default).
 *
 * @return Allocated type node with Record kind.
 *
 * @par Example
 * @code
 * auto point = n00b_tc_record(*r"Point",
 *     n00b_tc_field(*r"x", t_int),
 *     n00b_tc_field(*r"y", t_int));
 * @endcode
 */
extern n00b_tc_type_t *n00b_tc_record(n00b_string_t name, n00b_tc_field_t +)
    _kargs {
        bool ordered = true;
        bool open    = false;
    };

/**
 * @brief Create a tuple type.
 *
 * @param ...  Element types as `n00b_tc_type_t *` varargs.
 *
 * @kw open  true = open tuple (at least N elements); false = closed (default).
 *
 * @return Allocated type node with Tuple kind.
 *
 * @par Example
 * @code
 * auto pair = n00b_tc_tuple(t_int, t_string);
 * auto open = n00b_tc_tuple(t_int, t_string, .open = true);
 * @endcode
 */
extern n00b_tc_type_t *n00b_tc_tuple(n00b_tc_type_t *+)
    _kargs { bool open = false; };

/**
 * @brief Build a field descriptor for `n00b_tc_record()`.
 *
 * @param name  Field name.
 * @param type  Field type.
 *
 * @kw has_default  true = caller may omit (keyword args).
 *
 * @return Heap-allocated field descriptor.
 */
extern n00b_tc_field_t n00b_tc_field(n00b_string_t name, n00b_tc_type_t *type)
    _kargs { bool has_default = false; };
