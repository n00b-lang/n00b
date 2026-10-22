#ifndef N00BB_COMPILER_AXIOMS_H
#define N00BB_COMPILER_AXIOMS_H

#include "ir/ir.h"
#include "ir/summary.h"
#include "parse/types.h"

typedef enum : uint8_t {
    N00BB_AXIOM_NATIVE,
    N00BB_AXIOM_FUNCTION,
    N00BB_AXIOM_BINDING,
    N00BB_AXIOM_BUILTIN,
} n00bb_axiom_key_t;

enum {
    N00BB_AXIOM_MUTATOR_ALLOCATOR = 1u << 0,
    N00BB_AXIOM_MUTATOR_GETTER    = 1u << 1,
    N00BB_AXIOM_LOCK_ACQUIRE      = 1u << 3,
    N00BB_AXIOM_LOCK_RELEASE      = 1u << 4,
    N00BB_AXIOM_READS_TARGET      = 1u << 5,
    N00BB_AXIOM_LOCK_SYNC         = 1u << 6,
};

typedef struct {
    n00bb_axiom_key_t key;
    core_string_t     module;
    core_string_t     name;
    core_string_t     native;
    uint32_t          effects;
    uint8_t           reads;
    uint8_t           writes;
    uint8_t           entries;
    uint8_t           roles;
    bool              collection_free;
    ir_sreturn_kind_t returns;
} n00bb_axiom_t;

#define N00BB_AXIOM_ENTRY_FLAGS (IR_SUMMARY_CALLS_UNKNOWN                  \
                                 | IR_SUMMARY_CALLS_FINALIZERS             \
                                 | IR_SUMMARY_STARTS_THREADS)
#define N00BB_AXIOM_PARAM(index) (1u << (index))

const n00bb_axiom_t *n00bb_axiom_of_function(const ir_func_t *function);
const n00bb_axiom_t *n00bb_axiom_of_native_name(core_string_t name);
const n00bb_axiom_t *n00bb_axiom_of_builtin(core_string_t name);
const n00bb_axiom_t *const *n00bb_axiom_bindings(size_t *count);
const n00bb_axiom_t *n00bb_axiom_of_symbol(parse_sym_entry_t *symbol);
bool                 n00bb_axiom_function_has(const ir_func_t *function,
                                              uint8_t role);
void                 n00bb_axiom_note_pure_native(core_string_t symbol);
bool                 n00bb_axiom_pure_native(const ir_func_t *function);

#endif
