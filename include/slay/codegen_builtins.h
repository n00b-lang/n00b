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
                               n00b_cg_type_tag_t expected_ret,
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

// Shared runtime value representation for pointer-backed helpers.
typedef struct {
    uint64_t payload;
    uint64_t tag;
} n00b_rt_value_t;

n00b_rt_value_t n00b_rt_value_pack(uint64_t payload, n00b_cg_type_tag_t tag);
bool            n00b_rt_value_eq(n00b_rt_value_t a, n00b_rt_value_t b);
uint64_t        n00b_rt_value_hash64(n00b_rt_value_t value);
n00b_rt_value_t n00b_rt_value_copy(n00b_rt_value_t value);
void            n00b_builtin_print_value(uint64_t payload, int64_t tag);

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

typedef bool (*n00b_rt_callback_invoke_fn)(void      *target,
                                           MIR_val_t *result,
                                           MIR_val_t *args,
                                           int32_t    n_args);

typedef struct n00b_rt_callback_t {
    void                      *target;
    n00b_rt_callback_invoke_fn invoke;
    const char                *func_name;
    bool                       has_signature;
    n00b_cg_type_tag_t         ret_type;
    n00b_cg_type_tag_t         param_types[32];
    int32_t                    n_params;
} n00b_rt_callback_t;

// String operations.
void   *n00b_builtin_str_concat(void *a, void *b);
int64_t n00b_builtin_str_eq(void *a, void *b);
int64_t n00b_builtin_str_len(void *s);
void   *n00b_builtin_str_get(void *s, int64_t ix);
void *
n00b_builtin_str_slice(void *s, int64_t start, int64_t has_start, int64_t end, int64_t has_end);

// List operations. MIR passes list pointers and element payloads as i64.
void    *n00b_builtin_list_new(void);
void     n00b_builtin_list_push_i64(void *list, uint64_t val);
void     n00b_builtin_list_push_value(void *list, uint64_t payload, int64_t tag);
uint64_t n00b_builtin_list_get_i64(void *list, int64_t ix);
uint64_t n00b_builtin_list_get_value_payload(void *list, int64_t ix);
int64_t  n00b_builtin_list_get_value_tag(void *list, int64_t ix);
void     n00b_builtin_list_set_i64(void *list, int64_t ix, uint64_t val);
void     n00b_builtin_list_set_value(void *list, int64_t ix, uint64_t payload, int64_t tag);
int64_t  n00b_builtin_list_len(void *list);
void    *n00b_builtin_list_slice(void   *list,
                                 int64_t start,
                                 int64_t has_start,
                                 int64_t end,
                                 int64_t has_end);
void     n00b_builtin_list_slice_assign(void   *list,
                                        int64_t start,
                                        int64_t has_start,
                                        int64_t end,
                                        int64_t has_end,
                                        void   *replacement);

// Dict operations.
void    *n00b_builtin_dict_new(void);
void     n00b_builtin_dict_put_i64(void *dict, uint64_t key, uint64_t val);
void     n00b_builtin_dict_put_value(void    *dict,
                                     uint64_t key_payload,
                                     int64_t  key_tag,
                                     uint64_t val_payload,
                                     int64_t  val_tag);
uint64_t n00b_builtin_dict_get_i64(void *dict, uint64_t key);
uint64_t n00b_builtin_dict_get_value_payload(void *dict, uint64_t key_payload, int64_t key_tag);
int64_t  n00b_builtin_dict_get_value_tag(void *dict, uint64_t key_payload, int64_t key_tag);
int64_t  n00b_builtin_dict_len(void *dict);

// Set and collection copy operations.
void   *n00b_builtin_set_new(void);
void    n00b_builtin_set_add_value(void *set, uint64_t payload, int64_t tag);
int64_t n00b_builtin_set_len(void *set);
void   *n00b_builtin_copy_collection(void *collection, int64_t tag);

// Module parameter and confspec metadata operations.
void     n00b_builtin_parameter_validate(void *name, int64_t ok);
// Confspec registration and query builtins must share this session namespace key.
uint64_t n00b_codegen_session_namespace_key(n00b_cg_session_t *s);
void n00b_builtin_confspec_register(uint64_t namespace_key, int64_t sections, int64_t fields);
int64_t  n00b_builtin_confspec_section_count(uint64_t namespace_key);
int64_t  n00b_builtin_confspec_field_count(uint64_t namespace_key);
uint64_t n00b_builtin_callback_call0(void *callback);
uint64_t n00b_builtin_callback_call1(void *callback, uint64_t arg0);
uint64_t n00b_builtin_callback_call2(void *callback, uint64_t arg0, uint64_t arg1);

// Once-function cache operations.
int64_t  n00b_builtin_once_is_done(uint64_t key);
uint64_t n00b_builtin_once_get_i64(uint64_t key);
void     n00b_builtin_once_store_i64(uint64_t key, uint64_t value);
float    n00b_builtin_once_get_f32(uint64_t key);
void     n00b_builtin_once_store_f32(uint64_t key, float value);
double   n00b_builtin_once_get_f64(uint64_t key);
void     n00b_builtin_once_store_f64(uint64_t key, double value);

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
