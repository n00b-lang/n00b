/**
 * @file codegen_builtins.h
 * @brief Built-in function handlers for the n00b codegen.
 *
 * These are C implementations of n00b built-in functions (print, etc.)
 * that get imported into MIR and called directly from JIT'd code,
 * bypassing the FFI system.
 */

#pragma once

#include "slay/codegen.h"

/**
 * @brief Dispatch a call to a built-in function.
 *
 * Checks @p func_name against the built-in table. If it matches,
 * imports the C implementation into MIR and emits a call. Returns
 * true if the function was handled, false if it's not a built-in.
 *
 * @param s         Codegen session.
 * @param func_name Name of the function being called.
 * @param args      Evaluated argument values.
 * @param n_args    Number of arguments.
 * @param[out] out  Result value (set only if function was handled).
 * @return true if this was a built-in call, false otherwise.
 */
bool n00b_codegen_builtin_call(n00b_cg_session_t *s,
                               const char        *func_name,
                               n00b_cg_val_t     *args,
                               int32_t            n_args,
                               n00b_cg_val_t     *out);

/**
 * @brief Try to dispatch a method call via the vtable.
 *
 * Given a method name and args (first arg is the receiver), looks up
 * the method on the receiver's type via the type registry. If found,
 * imports and calls it. Returns true if handled.
 */
bool n00b_codegen_method_dispatch(n00b_cg_session_t *s,
                                  const char        *method_name,
                                  n00b_cg_val_t     *args,
                                  int32_t            n_args,
                                  n00b_cg_val_t     *out);

// ============================================================================
// C runtime helpers (called from JIT'd code via MIR import)
// ============================================================================

void n00b_builtin_print_i64(int64_t val);
void n00b_builtin_print_u64(uint64_t val);
void n00b_builtin_print_f64(double val);
void n00b_builtin_print_bool(int64_t val);
void n00b_builtin_print_str(void *str_ptr);
void n00b_builtin_print_nil(void);

// Interpreter runtime types for option/result.
typedef struct {
    bool     has_value;
    uint64_t value;
} n00b_rt_option_t;

typedef struct {
    bool     is_ok;
    uint64_t payload;
    int64_t  err_code;
    char    *err_message;
} n00b_rt_result_t;

// String operations.
void    *n00b_builtin_str_concat(void *a, void *b);
int64_t  n00b_builtin_str_eq(void *a, void *b);
int64_t  n00b_builtin_str_len(void *s);

// Option operations.
void    *n00b_builtin_option_some(uint64_t val);
void    *n00b_builtin_option_none(void);
int64_t  n00b_builtin_option_is_set(void *opt);
uint64_t n00b_builtin_option_unwrap(void *opt);
void     n00b_builtin_print_option(void *opt);

// Result operations.
void    *n00b_builtin_result_ok(uint64_t val);
void    *n00b_builtin_result_err_code(int64_t code);
void    *n00b_builtin_result_err_msg(void *msg);
int64_t  n00b_builtin_result_is_ok(void *res);
uint64_t n00b_builtin_result_unwrap(void *res);
void     n00b_builtin_print_result(void *res);
