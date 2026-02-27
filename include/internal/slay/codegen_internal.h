#pragma once

/**
 * @file codegen_internal.h
 * @internal
 * @brief Internal context struct for the codegen engine.
 */

#include "slay/codegen.h"
#include "internal/slay/grammar_internal.h"
#include "core/dict.h"
#include "core/string.h"

// ============================================================================
// Operator mapping entry
// ============================================================================

typedef struct {
    const char           *text;
    n00b_cg_semantic_op_t op;
} n00b_cg_op_entry_t;

// ============================================================================
// Loop label stack entry
// ============================================================================

typedef struct {
    n00b_cg_val_t break_label;
    n00b_cg_val_t continue_label;
} n00b_cg_loop_entry_t;

// ============================================================================
// Import record (for MIR_CALL proto caching)
// ============================================================================

typedef struct {
    const char  *name;
    MIR_item_t   proto;
    MIR_item_t   import;
    void        *addr;
} n00b_cg_import_t;

// ============================================================================
// Codegen context
// ============================================================================

struct n00b_codegen_t {
    // Inputs
    n00b_grammar_t       *grammar;
    n00b_annot_result_t  *annot;

    // MIR state
    MIR_context_t         mir_ctx;
    MIR_module_t          mir_module;
    MIR_item_t            cur_func;
    bool                  linked;
    bool                  gen_inited;

    // Callbacks
    n00b_cg_type_map_fn   type_map;
    n00b_cg_literal_fn    literal_parser;
    n00b_cg_storage_fn    storage_policy;
    n00b_cg_handler_fn    default_handler;

    // Handler table: indexed by NT id
    n00b_cg_handler_fn   *handlers;
    int32_t               handler_cap;

    // Operator mapping: small array, linear scan
    n00b_cg_op_entry_t   *op_map;
    int32_t               op_map_count;
    int32_t               op_map_cap;

    // Label tracking: index -> MIR_label_t
    MIR_label_t          *labels;
    int32_t               label_count;
    int32_t               label_cap;

    // Break/continue label stack (for loops)
    n00b_cg_loop_entry_t *loop_stack;
    int32_t               loop_depth;
    int32_t               loop_cap;

    // Import records (for proto caching)
    n00b_cg_import_t     *imports;
    int32_t               import_count;
    int32_t               import_cap;

    // Function proto cache (for calls to defined functions)
    MIR_item_t           *func_protos;
    const char          **func_names;
    int32_t               func_proto_count;
    int32_t               func_proto_cap;

    // Diagnostics
    n00b_diag_ctx_t      *diag;
    int32_t               temp_counter;
    void                 *user_data;
};

// ============================================================================
// Internal helpers (codegen_mir.c)
// ============================================================================

/**
 * @brief Map a codegen type tag to a MIR type.
 */
MIR_type_t n00b_cg_mir_type(n00b_cg_type_tag_t tag);

/**
 * @brief Create a MIR operand from a codegen value.
 */
MIR_op_t n00b_cg_mir_op(n00b_codegen_t *cg, n00b_cg_val_t val);

/**
 * @brief Allocate a fresh temporary register.
 */
n00b_cg_val_t n00b_cg_temp(n00b_codegen_t *cg, n00b_cg_type_tag_t type);

/**
 * @brief Emit a binary operation using semantic op dispatch.
 */
n00b_cg_val_t n00b_cg_emit_binop(n00b_codegen_t       *cg,
                                   n00b_cg_semantic_op_t op,
                                   n00b_cg_val_t          a,
                                   n00b_cg_val_t          b);

/**
 * @brief Emit a unary operation using semantic op dispatch.
 */
n00b_cg_val_t n00b_cg_emit_unop(n00b_codegen_t       *cg,
                                  n00b_cg_semantic_op_t op,
                                  n00b_cg_val_t          a);

/**
 * @brief Look up or create a prototype for a function call.
 */
MIR_item_t n00b_cg_get_or_create_proto(n00b_codegen_t     *cg,
                                         const char          *name,
                                         n00b_cg_type_tag_t   ret,
                                         n00b_cg_type_tag_t  *param_types,
                                         int32_t              n_params,
                                         bool                 is_vararg);

/**
 * @brief Find an import record by name.
 */
n00b_cg_import_t *n00b_cg_find_import(n00b_codegen_t *cg, const char *name);

/**
 * @brief Find a MIR function item by name.
 */
MIR_item_t n00b_cg_find_func(n00b_codegen_t *cg, const char *name);

/**
 * @brief Look up a semantic op from operator text.
 * @return The semantic op, or -1 if not found.
 */
int32_t n00b_cg_lookup_op(n00b_codegen_t *cg, const char *text);
