#pragma once

/**
 * @file codegen_internal.h
 * @internal
 * @brief Internal structs for the codegen engine: session and module state.
 */

#include "slay/codegen.h"
#include "slay/symtab.h"
#include "internal/slay/grammar_internal.h"
#include "adt/dict.h"
#include "adt/dict_untyped.h"
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
// Module: per-file or per-REPL-expression compilation unit
// ============================================================================

struct n00b_cg_module_t {
    n00b_cg_session_t       *session;
    const char               *name;           // FQN: "package.module" or "repl_N"
    MIR_module_t              mir_module;
    n00b_cg_module_state_t    state;

    // Annotation result (set per-module for tree-walk codegen)
    n00b_annot_result_t      *annot;

    // Functions defined in this module
    MIR_item_t               *func_protos;
    const char              **func_names;
    int32_t                   func_proto_count;
    int32_t                   func_proto_cap;

    // Per-module imports (for MIR linking)
    n00b_cg_import_t         *imports;
    int32_t                   import_count;
    int32_t                   import_cap;

    // Per-module emit state (reset each function / module)
    MIR_label_t              *labels;
    int32_t                   label_count;
    int32_t                   label_cap;
    n00b_cg_loop_entry_t     *loop_stack;
    int32_t                   loop_depth;
    int32_t                   loop_cap;
    int32_t                   temp_counter;
    MIR_item_t                cur_func;       // function being emitted into
};

// ============================================================================
// Session: persistent state across all modules
// ============================================================================

struct n00b_cg_session_t {
    // MIR context (single, persistent)
    MIR_context_t             mir_ctx;
    bool                      gen_inited;

    // Grammar & callbacks (shared across all modules)
    n00b_grammar_t           *grammar;
    n00b_cg_type_map_fn       type_map;
    n00b_cg_literal_fn        literal_parser;
    n00b_cg_storage_fn        storage_policy;
    n00b_cg_handler_fn        default_handler;

    // Handler table: indexed by NT id (shared)
    n00b_cg_handler_fn       *handlers;
    int32_t                   handler_cap;

    // Operator mapping: small array, linear scan (shared)
    n00b_cg_op_entry_t       *op_map;
    int32_t                   op_map_count;
    int32_t                   op_map_cap;

    // FFI import table (from N00B_CG_EXPORT / manual registration)
    n00b_cg_import_table_t   *import_table;

    // All modules in this session (compilation order)
    n00b_cg_module_t        **modules;
    int32_t                   module_count;
    int32_t                   module_cap;

    // Active module: target for all emit operations
    n00b_cg_module_t         *active_module;

    // Backward compat: annotation result stored on session for old API
    n00b_annot_result_t      *annot;

    // Module cache: C-string FQN → n00b_cg_module_t * (for use-stmt loading)
    n00b_dict_untyped_t      *module_cache;

    // Loading stack for cycle detection (modules currently being loaded)
    const char              **loading_stack;
    int32_t                   loading_depth;
    int32_t                   loading_cap;

    // Global scope: merged public symbols from all compiled modules
    n00b_symtab_t            *global_scope;

    // Diagnostics
    n00b_diag_ctx_t          *diag;
    void                     *user_data;
};

// ============================================================================
// Internal helpers (codegen_mir.c)
// ============================================================================

MIR_type_t n00b_cg_mir_type(n00b_cg_type_tag_t tag);

MIR_op_t n00b_cg_mir_op(n00b_cg_session_t *session, n00b_cg_val_t val);

n00b_cg_val_t n00b_cg_temp(n00b_cg_session_t *session, n00b_cg_type_tag_t type);

n00b_cg_val_t n00b_cg_emit_binop(n00b_cg_session_t    *session,
                                   n00b_cg_semantic_op_t op,
                                   n00b_cg_val_t          a,
                                   n00b_cg_val_t          b);

n00b_cg_val_t n00b_cg_emit_unop(n00b_cg_session_t    *session,
                                  n00b_cg_semantic_op_t op,
                                  n00b_cg_val_t          a);

MIR_item_t n00b_cg_get_or_create_proto(n00b_cg_session_t   *session,
                                         const char           *name,
                                         n00b_cg_type_tag_t    ret,
                                         n00b_cg_type_tag_t   *param_types,
                                         int32_t               n_params,
                                         bool                  is_vararg);

n00b_cg_import_t *n00b_cg_find_import(n00b_cg_session_t *session,
                                        const char         *name);

MIR_item_t n00b_cg_find_func(n00b_cg_session_t *session, const char *name);
