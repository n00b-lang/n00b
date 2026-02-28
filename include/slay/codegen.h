#pragma once

/**
 * @file codegen.h
 * @brief Code generation API with MIR backend.
 *
 * Provides a language-agnostic code generation layer that:
 * - Auto-infers semantics for common patterns (binary `+` -> add, etc.)
 * - Lets languages register per-NT callbacks for custom constructs
 * - Provides pre-flight audit of codegen coverage
 * - Supports interpret, JIT, and compiled output via MIR
 *
 * ## Usage modes
 *
 * **Builder-only mode**: Pass `NULL` for grammar to `n00b_codegen_new()`.
 * Only the builder API (`n00b_cg_begin_func`, `n00b_cg_emit_*`, etc.) is
 * available. Useful for programmatic code generation without a grammar.
 *
 * **Grammar-driven mode**: Pass a grammar and annotation result.
 * `n00b_codegen_emit()` walks the parse tree, using registered handlers
 * and auto-inference from annotations (`@operator`, `@literal`, etc.).
 *
 * ## Naming
 *
 * - `n00b_codegen_*` — lifecycle, registration, emission, execution
 * - `n00b_cg_*` — builder API (functions, locals, instructions)
 */

#include "slay/cf_label.h"
#include "slay/grammar.h"
#include "slay/diagnostic.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include "mir.h"
#include "mir-gen.h"
#pragma clang diagnostic pop

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct n00b_codegen_t n00b_codegen_t;

// ============================================================================
// Machine-level type categories
// ============================================================================

/**
 * @brief Machine-level type tags for codegen values.
 *
 * These map 1:1 to MIR types. Languages provide a `type_map` callback
 * to translate their type system into these tags.
 */
typedef enum {
    N00B_CG_I8,
    N00B_CG_I16,
    N00B_CG_I32,
    N00B_CG_I64,
    N00B_CG_U8,
    N00B_CG_U16,
    N00B_CG_U32,
    N00B_CG_U64,
    N00B_CG_F32,
    N00B_CG_F64,
    N00B_CG_PTR,
    N00B_CG_BOOL,  /**< Backed by I64, semantically boolean. */
    N00B_CG_VOID,
} n00b_cg_type_tag_t;

// ============================================================================
// Value kind
// ============================================================================

/** @brief Discriminator for codegen value handles. */
typedef enum {
    N00B_CG_VAL_REG,   /**< MIR register. */
    N00B_CG_VAL_IMM,   /**< Immediate constant. */
    N00B_CG_VAL_MEM,   /**< Memory reference. */
    N00B_CG_VAL_LABEL, /**< Label handle. */
    N00B_CG_VAL_VOID,  /**< No value (void return, etc.). */
} n00b_cg_val_kind_t;

// ============================================================================
// Value handle (16 bytes, pass by value)
// ============================================================================

/**
 * @brief A codegen value handle.
 *
 * Wraps a MIR register, immediate, memory reference, or label.
 * Designed to be passed by value (16 bytes).
 */
typedef struct {
    uint32_t            id;       /**< MIR_reg_t or label index. */
    n00b_cg_val_kind_t  kind : 8;
    n00b_cg_type_tag_t  type_tag : 8;
    uint16_t            _pad;
    uint64_t            aux;      /**< Immediate value, displacement, or label handle. */
} n00b_cg_val_t;

/** @brief Sentinel void value. */
#define N00B_CG_VOID_VAL ((n00b_cg_val_t){.kind = N00B_CG_VAL_VOID, .type_tag = N00B_CG_VOID})

// ============================================================================
// Semantic operation codes
// ============================================================================

/**
 * @brief Semantic operation codes for auto-inferred operators.
 *
 * These are mapped to specific MIR instructions based on the operand
 * type tag (integer vs float, signed vs unsigned).
 */
typedef enum {
    N00B_CG_OP_ADD,
    N00B_CG_OP_SUB,
    N00B_CG_OP_MUL,
    N00B_CG_OP_DIV,
    N00B_CG_OP_MOD,
    N00B_CG_OP_NEG,
    N00B_CG_OP_AND,
    N00B_CG_OP_OR,
    N00B_CG_OP_XOR,
    N00B_CG_OP_NOT,
    N00B_CG_OP_SHL,
    N00B_CG_OP_SHR,
    N00B_CG_OP_EQ,
    N00B_CG_OP_NE,
    N00B_CG_OP_LT,
    N00B_CG_OP_LE,
    N00B_CG_OP_GT,
    N00B_CG_OP_GE,
    N00B_CG_OP_LOGICAL_AND,
    N00B_CG_OP_LOGICAL_OR,
    N00B_CG_OP_LOGICAL_NOT,
    N00B_CG_OP_COUNT, /**< Sentinel; custom ops start here. */
} n00b_cg_semantic_op_t;

// ============================================================================
// Storage policy
// ============================================================================

/** @brief How a variable should be stored. */
typedef enum {
    N00B_CG_STORE_REG,  /**< In a MIR register. */
    N00B_CG_STORE_MEM,  /**< In memory (alloca). */
} n00b_cg_storage_t;

// ============================================================================
// Callback signatures
// ============================================================================

/**
 * @brief Type mapping callback: language type -> machine type tag.
 * @param cg    Codegen context.
 * @param type  Language-level type from annotation walk.
 * @return Machine-level type tag.
 */
typedef n00b_cg_type_tag_t (*n00b_cg_type_map_fn)(n00b_codegen_t *cg,
                                                    n00b_tc_type_t *type);

/**
 * @brief Per-NT codegen handler: emit code for a node, return result value.
 * @param cg    Codegen context.
 * @param node  Parse tree node to generate code for.
 * @return The result value (or `N00B_CG_VOID_VAL` for void).
 */
typedef n00b_cg_val_t (*n00b_cg_handler_fn)(n00b_codegen_t   *cg,
                                              n00b_parse_tree_t *node);

/**
 * @brief Literal parser: token text -> immediate value.
 * @param cg        Codegen context.
 * @param node      Parse tree node (leaf token).
 * @param lit_kind  Literal kind string (e.g., "integer", "float", "string").
 * @param type_tag  Expected machine type.
 * @return Immediate value.
 */
typedef n00b_cg_val_t (*n00b_cg_literal_fn)(n00b_codegen_t    *cg,
                                              n00b_parse_tree_t *node,
                                              n00b_string_t      lit_kind,
                                              n00b_cg_type_tag_t type_tag);

/**
 * @brief Storage policy: register vs memory per symbol.
 * @param cg   Codegen context.
 * @param sym  Symbol table entry.
 * @return Storage decision.
 */
typedef n00b_cg_storage_t (*n00b_cg_storage_fn)(n00b_codegen_t  *cg,
                                                  n00b_sym_entry_t *sym);

// ============================================================================
// Audit result
// ============================================================================

/**
 * @brief Pre-flight audit result showing codegen coverage.
 *
 * Each NT in the grammar is classified as explicit (has a registered
 * handler), auto-inferred (has semantic annotations), or unhandled.
 */
typedef struct {
    const char **unhandled_nts;     /**< NT names with no handler or inference. */
    int32_t      unhandled_count;
    const char **auto_inferred;     /**< NT names handled by auto-inference. */
    int32_t      auto_inferred_count;
    const char **explicit_handled;  /**< NT names with explicit handlers. */
    int32_t      explicit_count;
} n00b_cg_audit_t;

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Create a new codegen context.
 *
 * @param grammar  Grammar for tree-walking codegen (NULL for builder-only mode).
 *
 * @kw annot           Annotation walk result (NULL for builder-only mode).
 * @kw type_map        Type mapping callback (NULL = everything is I64).
 * @kw literal_parser  Literal parsing callback (NULL = strtoll/strtod).
 * @kw storage_policy  Storage policy callback (NULL = all registers).
 * @kw default_handler Default handler for unregistered NTs (NULL = lower children).
 * @kw user_data       User-defined context pointer (NULL).
 * @kw module_name     MIR module name ("main").
 */
n00b_codegen_t *n00b_codegen_new(n00b_grammar_t *grammar)
_kargs {
    n00b_annot_result_t *annot;
    n00b_cg_type_map_fn  type_map;
    n00b_cg_literal_fn   literal_parser;
    n00b_cg_storage_fn   storage_policy;
    n00b_cg_handler_fn   default_handler;
    void                *user_data;
    const char          *module_name;
};

/**
 * @brief Free a codegen context and all associated MIR state.
 */
void n00b_codegen_free(n00b_codegen_t *cg);

// ============================================================================
// Handler registration
// ============================================================================

/**
 * @brief Register a codegen handler by NT name.
 * @param cg       Codegen context (must have a grammar).
 * @param nt_name  Non-terminal name.
 * @param handler  Handler function.
 * @return True if NT was found in grammar, false otherwise.
 */
bool n00b_codegen_register(n00b_codegen_t  *cg,
                            const char       *nt_name,
                            n00b_cg_handler_fn handler);

/**
 * @brief Register a codegen handler by NT id.
 * @param cg       Codegen context (must have a grammar).
 * @param nt_id    Non-terminal id.
 * @param handler  Handler function.
 * @return True if NT id is valid, false otherwise.
 */
bool n00b_codegen_register_by_id(n00b_codegen_t    *cg,
                                  int64_t             nt_id,
                                  n00b_cg_handler_fn  handler);

/**
 * @brief Register an operator token -> semantic op mapping.
 *
 * Built-in mappings for standard operators (+, -, *, /, etc.) are
 * installed by default. Use this to add custom operators or override.
 *
 * @param cg          Codegen context.
 * @param token_text  Operator token text (e.g., "**" for power).
 * @param op          Semantic operation code.
 */
void n00b_codegen_map_operator(n00b_codegen_t      *cg,
                                const char           *token_text,
                                n00b_cg_semantic_op_t op);

// ============================================================================
// Pre-flight audit
// ============================================================================

/**
 * @brief Audit codegen coverage against the grammar.
 *
 * @pre Codegen context must have a grammar (`grammar != NULL`).
 * @return Audit result. Free with `n00b_cg_audit_free()`.
 */
n00b_cg_audit_t n00b_codegen_audit(n00b_codegen_t *cg);

/**
 * @brief Free an audit result.
 */
void n00b_cg_audit_free(n00b_cg_audit_t *audit);

// ============================================================================
// Operator lookup (for testing / handler use)
// ============================================================================

/**
 * @brief Look up a semantic op from operator text.
 * @return The semantic op index, or -1 if not found.
 */
int32_t n00b_cg_lookup_op(n00b_codegen_t *cg, const char *text);

// ============================================================================
// Code emission (tree-walking)
// ============================================================================

/**
 * @brief Emit code for an entire parse tree.
 *
 * Walks the tree top-down, dispatching to registered handlers and
 * auto-inferred codegen based on annotations.
 *
 * @pre Codegen context must have a grammar and annotation result.
 * @param cg    Codegen context.
 * @param tree  Parse tree root.
 * @return True on success, false if errors occurred.
 */
bool n00b_codegen_emit(n00b_codegen_t *cg, n00b_parse_tree_t *tree);

/**
 * @brief Lower a single child node (for use within handlers).
 *
 * Recursively generates code for the given subtree and returns
 * the result value.
 *
 * @param cg     Codegen context.
 * @param child  Parse tree node to lower.
 * @return Result value.
 */
n00b_cg_val_t n00b_codegen_lower(n00b_codegen_t *cg, n00b_parse_tree_t *child);

// ============================================================================
// Queries (for use in handlers)
// ============================================================================

/**
 * @brief Get the machine-level type of a parse tree node.
 *
 * Uses the type map callback and annotation result's node_types.
 */
n00b_cg_type_tag_t n00b_codegen_node_type(n00b_codegen_t    *cg,
                                            n00b_parse_tree_t *node);

/**
 * @brief Get the CF label for a parse tree node, if any.
 */
n00b_cf_label_t *n00b_codegen_cf_label(n00b_codegen_t    *cg,
                                         n00b_parse_tree_t *node);

/** @brief Get the grammar from a codegen context. */
n00b_grammar_t *n00b_codegen_grammar(n00b_codegen_t *cg);

/** @brief Get the annotation result from a codegen context. */
n00b_annot_result_t *n00b_codegen_annot(n00b_codegen_t *cg);

/** @brief Get the diagnostics context. */
n00b_diag_ctx_t *n00b_codegen_diagnostics(n00b_codegen_t *cg);

/** @brief Get the user data pointer. */
void *n00b_codegen_get_user_data(n00b_codegen_t *cg);

// ============================================================================
// Builder API: Function scope
// ============================================================================

/**
 * @brief Begin a new function definition.
 *
 * @param cg    Codegen context.
 * @param name  Function name.
 *
 * @kw ret          Return type (N00B_CG_VOID).
 * @kw param_names  Parameter name array (NULL).
 * @kw param_types  Parameter type array (NULL).
 * @kw n_params     Number of parameters (0).
 * @kw is_vararg    Whether function is variadic (false).
 */
void n00b_cg_begin_func(n00b_codegen_t *cg, const char *name)
_kargs {
    n00b_cg_type_tag_t   ret;
    const char         **param_names;
    n00b_cg_type_tag_t  *param_types;
    int32_t              n_params;
    bool                 is_vararg;
};

/**
 * @brief End the current function definition.
 */
void n00b_cg_end_func(n00b_codegen_t *cg);

/**
 * @brief Get the value handle for a function parameter by index.
 */
n00b_cg_val_t n00b_cg_param(n00b_codegen_t *cg, int32_t index);

// ============================================================================
// Builder API: Locals
// ============================================================================

/**
 * @brief Declare a local variable.
 *
 * @param cg    Codegen context.
 * @param name  Variable name.
 * @kw type     Variable type (N00B_CG_I64).
 */
n00b_cg_val_t n00b_cg_local(n00b_codegen_t *cg, const char *name)
_kargs {
    n00b_cg_type_tag_t type;
};

/**
 * @brief Store a value into a register or memory location.
 */
void n00b_cg_store(n00b_codegen_t *cg, n00b_cg_val_t dst, n00b_cg_val_t src);

/**
 * @brief Load a value from a register or memory location.
 */
n00b_cg_val_t n00b_cg_load(n00b_codegen_t *cg, n00b_cg_val_t var);

// ============================================================================
// Builder API: Constants
// ============================================================================

/**
 * @brief Create an immediate constant from a C value.
 *
 * Uses `_Generic` to dispatch on C type.
 */
#define n00b_cg_const(cg, value)                    \
    _Generic((value),                               \
        int64_t:  _n00b_cg_const_i64,              \
        int32_t:  _n00b_cg_const_i32,              \
        uint64_t: _n00b_cg_const_u64,              \
        double:   _n00b_cg_const_f64,              \
        float:    _n00b_cg_const_f32,              \
        bool:     _n00b_cg_const_bool,             \
        void *:   _n00b_cg_const_ptr,              \
        default:  _n00b_cg_const_i64               \
    )(cg, value)

n00b_cg_val_t _n00b_cg_const_i64(n00b_codegen_t *cg, int64_t v);
n00b_cg_val_t _n00b_cg_const_i32(n00b_codegen_t *cg, int32_t v);
n00b_cg_val_t _n00b_cg_const_u64(n00b_codegen_t *cg, uint64_t v);
n00b_cg_val_t _n00b_cg_const_f64(n00b_codegen_t *cg, double v);
n00b_cg_val_t _n00b_cg_const_f32(n00b_codegen_t *cg, float v);
n00b_cg_val_t _n00b_cg_const_bool(n00b_codegen_t *cg, bool v);
n00b_cg_val_t _n00b_cg_const_ptr(n00b_codegen_t *cg, void *v);

// ============================================================================
// Builder API: Arithmetic / logic
// ============================================================================

n00b_cg_val_t n00b_cg_emit_add(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_sub(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_mul(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_div(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_mod(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_neg(n00b_codegen_t *cg, n00b_cg_val_t a);

n00b_cg_val_t n00b_cg_emit_and(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_or(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_xor(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_not(n00b_codegen_t *cg, n00b_cg_val_t a);
n00b_cg_val_t n00b_cg_emit_shl(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_shr(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);

n00b_cg_val_t n00b_cg_emit_eq(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_ne(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_lt(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_le(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_gt(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);
n00b_cg_val_t n00b_cg_emit_ge(n00b_codegen_t *cg, n00b_cg_val_t a, n00b_cg_val_t b);

/**
 * @brief Convert a value to a different type.
 */
n00b_cg_val_t n00b_cg_emit_convert(n00b_codegen_t    *cg,
                                     n00b_cg_val_t       src,
                                     n00b_cg_type_tag_t  dst_type);

// ============================================================================
// Builder API: Control flow
// ============================================================================

/**
 * @brief Create a new label (not yet placed).
 */
n00b_cg_val_t n00b_cg_label_new(n00b_codegen_t *cg);

/**
 * @brief Place a label at the current position.
 */
void n00b_cg_label_here(n00b_codegen_t *cg, n00b_cg_val_t label);

/** @brief Emit an unconditional jump. */
void n00b_cg_emit_jmp(n00b_codegen_t *cg, n00b_cg_val_t label);

/** @brief Emit a branch-if-true. */
void n00b_cg_emit_bt(n00b_codegen_t *cg, n00b_cg_val_t cond, n00b_cg_val_t label);

/** @brief Emit a branch-if-false. */
void n00b_cg_emit_bf(n00b_codegen_t *cg, n00b_cg_val_t cond, n00b_cg_val_t label);

/** @brief Emit a return with a value. */
void n00b_cg_emit_ret(n00b_codegen_t *cg, n00b_cg_val_t val);

/** @brief Emit a void return. */
void n00b_cg_emit_ret_void(n00b_codegen_t *cg);

// ============================================================================
// Builder API: Function calls
// ============================================================================

/**
 * @brief Emit a direct function call.
 *
 * @param cg         Codegen context.
 * @param func_name  Name of the function to call.
 * @param args       Array of argument values.
 * @param n_args     Number of arguments.
 * @kw ret           Return type (N00B_CG_VOID).
 */
n00b_cg_val_t n00b_cg_emit_call(n00b_codegen_t *cg,
                                  const char      *func_name,
                                  n00b_cg_val_t   *args,
                                  int32_t          n_args)
_kargs {
    n00b_cg_type_tag_t ret;
};

/**
 * @brief Emit an indirect function call (via function pointer).
 *
 * @param cg        Codegen context.
 * @param func_ptr  Value holding the function pointer.
 * @param args      Array of argument values.
 * @param n_args    Number of arguments.
 * @kw ret          Return type (N00B_CG_VOID).
 */
n00b_cg_val_t n00b_cg_emit_call_indirect(n00b_codegen_t *cg,
                                           n00b_cg_val_t    func_ptr,
                                           n00b_cg_val_t   *args,
                                           int32_t          n_args)
_kargs {
    n00b_cg_type_tag_t ret;
};

// ============================================================================
// Builder API: Memory
// ============================================================================

/**
 * @brief Load a value from memory.
 *
 * @param cg    Codegen context.
 * @param addr  Address to load from.
 * @param type  Type of the value to load.
 * @kw offset   Byte offset from addr (0).
 */
n00b_cg_val_t n00b_cg_emit_mem_load(n00b_codegen_t    *cg,
                                      n00b_cg_val_t       addr,
                                      n00b_cg_type_tag_t  type)
_kargs {
    int64_t offset;
};

/**
 * @brief Store a value to memory.
 *
 * @param cg     Codegen context.
 * @param addr   Address to store to.
 * @param value  Value to store.
 * @kw offset    Byte offset from addr (0).
 */
void n00b_cg_emit_mem_store(n00b_codegen_t *cg,
                             n00b_cg_val_t    addr,
                             n00b_cg_val_t    value)
_kargs {
    int64_t offset;
};

/**
 * @brief Allocate stack space.
 * @param cg    Codegen context.
 * @param size  Number of bytes to allocate.
 * @return Pointer value to the allocated space.
 */
n00b_cg_val_t n00b_cg_emit_alloca(n00b_codegen_t *cg, int64_t size);

// ============================================================================
// Builder API: Imports
// ============================================================================

/**
 * @brief Import an external C function for calling from generated code.
 *
 * @param cg    Codegen context.
 * @param name  Function name.
 * @param addr  Function address.
 *
 * @kw ret          Return type (N00B_CG_VOID).
 * @kw param_types  Parameter type array (NULL).
 * @kw n_params     Number of parameters (0).
 * @kw is_vararg    Whether function is variadic (false).
 */
void n00b_cg_import_func(n00b_codegen_t *cg,
                          const char      *name,
                          void            *addr)
_kargs {
    n00b_cg_type_tag_t   ret;
    n00b_cg_type_tag_t  *param_types;
    int32_t              n_params;
    bool                 is_vararg;
};

// ============================================================================
// Execution modes
// ============================================================================

/**
 * @brief Interpret a function via MIR interpreter.
 *
 * @param cg         Codegen context.
 * @param func_name  Function to call.
 * @param result     Output: result value (NULL for void functions).
 * @param args       Argument values (as MIR_val_t array).
 * @param n_args     Number of arguments.
 * @return True on success.
 */
bool n00b_codegen_interpret(n00b_codegen_t *cg,
                             const char      *func_name,
                             void            *result,
                             void            *args,
                             int32_t          n_args);

/**
 * @brief JIT-compile a function and return a callable function pointer.
 *
 * @param cg         Codegen context.
 * @param func_name  Function to compile.
 * @return Function pointer, or NULL on failure.
 */
void *n00b_codegen_jit(n00b_codegen_t *cg, const char *func_name);

/**
 * @brief Dump the MIR module to a file for debugging.
 */
void n00b_codegen_dump(n00b_codegen_t *cg, FILE *f);

// ============================================================================
// Convenience helpers
// ============================================================================

/**
 * @brief Emit a parse tree as a named function body.
 *
 * Wraps the tree in a function: begins the function, lowers the tree
 * via `n00b_codegen_lower()`, emits a return (the last value produced,
 * or a zero/void return), and ends the function.
 *
 * After this call, the function is defined in the MIR module and can
 * be JIT'd or interpreted by name.
 *
 * @param cg         Codegen context.
 * @param tree       Parse tree to lower as the function body.
 * @param func_name  Name for the generated function.
 *
 * @kw ret  Return type tag (N00B_CG_I64).
 *
 * @return True if emission succeeded (no codegen errors).
 */
bool n00b_cg_emit_func_from_tree(n00b_codegen_t    *cg,
                                   n00b_parse_tree_t *tree,
                                   const char         *func_name)
_kargs {
    n00b_cg_type_tag_t ret;
};

/**
 * @brief One-shot evaluate: lower a tree, JIT, call, return i64 result.
 *
 * Creates a fresh codegen context, wraps the tree in a function,
 * JIT-compiles it, calls it, and returns the result as an `int64_t`.
 * The codegen context is freed before returning.
 *
 * @param grammar  Grammar (for tree-walk auto-inference).
 * @param tree     Parse tree to evaluate.
 *
 * @kw annot        Annotation walk result (NULL).
 * @kw type_map     Type mapping callback (NULL = everything is I64).
 * @kw func_name    Function name for the wrapper ("_eval").
 * @kw ok           If non-NULL, set to true on success, false on error.
 *
 * @return The int64_t result of execution (0 on error).
 */
int64_t n00b_codegen_eval_tree(n00b_grammar_t    *grammar,
                                 n00b_parse_tree_t *tree)
_kargs {
    n00b_annot_result_t *annot;
    n00b_cg_type_map_fn  type_map;
    const char          *func_name;
    bool                *ok;
};
