/**
 * @file print.h
 * @brief Type-to-string rendering for the type inference engine.
 *
 * @ingroup typecheck
 *
 * Converts `n00b_tc_type_t *` values into valid n00b type-spec syntax
 * strings.  Follows union-find links to their canonical representative,
 * handles cycles via an occurs check (printing `<cycle>` on re-entry),
 * and optionally appends constraint annotations in `where` clause form.
 *
 * ## Output examples
 *
 * | Input type                | Output string                          |
 * |---------------------------|----------------------------------------|
 * | Prim "int"                | `int`                                  |
 * | Param list[int]           | `list[int]`                            |
 * | Fn (int, string) -> bool  | `(int, string) -> bool`                |
 * | Fn (int, *string) -> void | `(int, *string) -> void`               |
 * | Sum int \| string         | `int \| string`                        |
 * | Var (unresolved)          | `` `T ``                               |
 * | Tuple (int, string)       | `(int, string)`                        |
 * | Record {x: int, y: int}   | `{x: int, y: int}`                     |
 */
#pragma once

#include "typecheck/types.h"

/**
 * @brief Render a type as a valid n00b type-spec string.
 *
 * Follows union-find links.  Handles cycles via occurs check.
 *
 * @param type  Type node to render.
 * @return      String representation (by value).
 */
extern n00b_string_t n00b_tc_type_to_string(n00b_tc_type_t *type);

/**
 * @brief Render a type with constraint annotations (where clause).
 *
 * After printing the type, if it's an unresolved Var with constraints,
 * appends `` where `T: <constraints> ``.
 *
 * @param type  Type node to render.
 * @return      String representation with optional where clause.
 */
extern n00b_string_t n00b_tc_type_to_string_full(n00b_tc_type_t *type);

/**
 * @brief Render a single constraint in human-readable form.
 *
 * @param con  Constraint to render.
 * @return     String representation (e.g., "Numeric", "!= nil").
 */
extern n00b_string_t n00b_tc_constraint_to_string(n00b_tc_constraint_t *con);
