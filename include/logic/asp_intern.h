/**
 * @file asp_intern.h
 * @brief Symbol interning for the Datalog engine.
 *
 * Maps string names to unique integer symbol IDs and back.
 * Constants receive non-negative IDs; logic variables receive
 * IDs that decrement from @ref N00B_DL_VAR_BASE.
 */
#pragma once

#include "logic/asp_types.h"
#include "internal/logic/asp_containers.h"
#include "core/option.h"
#include "core/list.h"

n00b_option_decl(n00b_dl_sym_t);

/**
 * @brief Symbol intern table.
 *
 * Maintains bidirectional mappings between string names and
 * integer symbol IDs for both constants and logic variables.
 */
typedef struct {
    n00b_dl_str_i64_map_t     name_to_id;
    n00b_dl_i64_str_map_t     var_id_to_name;
    n00b_list_t(n00b_string_t) id_to_name;
    int64_t                   next_var;
} n00b_dl_intern_t;

/**
 * @brief Initialize an intern table.
 * @param intern Table to initialize.
 */
void n00b_dl_intern_init(n00b_dl_intern_t *intern);

/**
 * @brief Free all resources held by an intern table.
 * @param intern Table to free.
 */
void n00b_dl_intern_free(n00b_dl_intern_t *intern);

/**
 * @brief Intern a constant symbol.
 *
 * Returns the existing ID if @p name has already been interned,
 * otherwise assigns a new non-negative ID.
 *
 * @param intern Intern table.
 * @param name   Constant name (copied internally).
 * @return Non-negative symbol ID.
 */
n00b_dl_sym_t n00b_dl_intern(n00b_dl_intern_t *intern, n00b_string_t name);

/**
 * @brief Look up a symbol by name without inserting.
 *
 * @param intern Intern table.
 * @param name   Name to look up.
 * @return Option containing the symbol ID, or none if not found.
 */
n00b_option_t(n00b_dl_sym_t) n00b_dl_intern_lookup(n00b_dl_intern_t *intern,
                                                     n00b_string_t     name);

/**
 * @brief Get the name for a symbol ID.
 *
 * @param intern Intern table.
 * @param id     Symbol ID (constant or variable).
 * @return Name string, or empty string if not found.
 */
n00b_string_t n00b_dl_intern_name(n00b_dl_intern_t *intern,
                                    n00b_dl_sym_t     id);

/**
 * @brief Intern a logic variable.
 *
 * Assigns a unique negative ID decrementing from @ref N00B_DL_VAR_BASE.
 * Returns the existing ID if @p name was already interned.
 *
 * @param intern Intern table.
 * @param name   Variable name (copied internally).
 * @return Negative symbol ID (<= N00B_DL_VAR_BASE).
 */
n00b_dl_sym_t n00b_dl_intern_var(n00b_dl_intern_t *intern,
                                   n00b_string_t     name);

/**
 * @brief Intern an integer value as a symbol.
 *
 * Encodes as `#42`, `#-7`, etc.
 *
 * @param intern Intern table.
 * @param value  Integer value.
 * @return Non-negative symbol ID.
 */
n00b_dl_sym_t n00b_dl_intern_int(n00b_dl_intern_t *intern, int64_t value);
