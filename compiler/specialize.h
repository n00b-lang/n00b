#ifndef N00BB_SPECIALIZE_H
#define N00BB_SPECIALIZE_H

#include "ir/ir.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "lib/string.h"
#include "parse/symtab.h"

typedef ir_func_t *n00bb_ir_func_ptr_t;
core_list_decl(n00bb_ir_func_ptr_t);

typedef struct {
    ir_func_t *generic;
    ir_func_t *instance;
    th_type *signature;
} n00bb_specialization_t;
core_list_decl(n00bb_specialization_t);

typedef struct {
    ir_module_t *requester;
    ir_func_t *instance;
} n00bb_specialization_request_t;
core_list_decl(n00bb_specialization_request_t);

typedef struct {
    ir_func_t *generic;
    th_type *signature;
} n00bb_specialization_seed_t;

typedef struct {
    uint64_t typehash;
    ir_func_t *function;
} n00bb_boxed_finalizer_t;
core_list_decl(n00bb_boxed_finalizer_t);

typedef struct {
    core_list_t(n00bb_boxed_finalizer_t) finalizers;
    core_list_t(n00bb_boxed_finalizer_t) addresses;
    core_list_t(n00bb_boxed_finalizer_t) mutable_addresses;
} n00bb_boxed_hooks_t;

typedef struct {
    core_buffer_t message;
    core_string_t module;
    core_string_t function;
    core_string_t before;
    core_string_t after;
    core_string_t other;
    core_string_t trait;
    ir_span_t span;
    core_string_t site_module;
    ir_span_t site;
    bool located;
    bool site_located;
    bool finalized_copy;
    bool missing_implementation;
    bool mismatched_operands;
} n00bb_specialization_failure_t;

static inline bool
n00bb_specialization_module(core_string_t module)
{
    return core_string_find(module, CORE_STRING_STATIC("$spec$")) >= 0;
}

typedef struct {
    ir_func_t *(*implementation)(void *context, th_ctx *types,
                                 th_type *receiver, core_string_t trait,
                                 core_string_t method);
    parse_sym_entry_t *const *(*comparison)(void *context, th_type *type);
    bool (*flagset)(void *context, th_type *type);
    void *context;
} n00bb_trait_binder_t;

bool n00bb_specialize_calls(ir_ctx_t *context,
    core_list_t(n00bb_ir_func_ptr_t) *functions,
    const n00bb_specialization_seed_t *seeds, size_t seed_count,
    core_list_t(n00bb_specialization_t) *instances,
    core_list_t(n00bb_specialization_request_t) *requests,
    n00bb_boxed_hooks_t *boxed_hooks,
    const n00bb_trait_binder_t *binder,
    n00bb_specialization_failure_t *failure);

#endif
