#pragma once

/**
 * @file n00b_compile_binary.h
 * @brief Compilation pipeline: codegen → object file → linked binary.
 *
 * Extracts machine code from MIR after JIT compilation, emits
 * platform-native `.o` files via the objfile builders, and links
 * them into an executable via clang.
 */

#include "slay/codegen.h"

/**
 * @brief A single relocation record (external symbol reference in code).
 */
typedef struct {
    const char *sym;    /**< External symbol name. */
    size_t      offset; /**< Byte offset within the function's code. */
} n00b_reloc_t;

/**
 * @brief Machine code and metadata for one compiled function.
 */
typedef struct {
    const char   *name;        /**< Function name (e.g. "_main"). */
    uint8_t      *code;        /**< Pointer to generated machine code. */
    size_t        code_len;    /**< Length of machine code in bytes. */
    n00b_reloc_t *relocs;      /**< Relocation records for external refs. */
    size_t        reloc_count; /**< Number of relocation records. */
} n00b_func_code_t;

/**
 * @brief All compiled functions from one module.
 */
typedef struct {
    n00b_func_code_t *funcs;      /**< Array of compiled functions. */
    size_t            func_count; /**< Number of functions. */
    const char       *module_name;/**< Module name. */
} n00b_module_code_t;

/**
 * @brief Compile a module's parse tree to machine code without executing.
 *
 * Runs the two-pass codegen (func-defs then _main wrapper),
 * JIT-compiles all functions via MIR_gen(), and extracts the
 * machine code + relocation info.
 *
 * @param s    Codegen session.
 * @param tree Parse tree to compile.
 * @return Module code struct, or NULL on failure.
 */
n00b_module_code_t *
n00b_cg_session_compile_module(n00b_cg_session_t *s,
                               n00b_parse_tree_t *tree)
_kargs {
    n00b_annot_result_t *annot;
    const char          *entry_name;
};

/**
 * @brief Emit a `.o` file from compiled module code.
 *
 * Uses the platform-appropriate object file builder (Mach-O on macOS,
 * ELF on Linux) to produce a relocatable object file.
 *
 * @param mod Compiled module code.
 * @return Buffer containing the `.o` file bytes, or NULL on failure.
 */
n00b_buffer_t *
n00b_emit_object_file(n00b_module_code_t *mod);

/**
 * @brief Link object files into an executable via clang.
 *
 * @param obj_paths  Array of `.o` file paths.
 * @param n_objs     Number of object files.
 * @param output     Output binary path.
 * @param lib_dir    Directory containing libn00b.a (or NULL for auto-detect).
 * @return 0 on success, non-zero on failure.
 */
int
n00b_link_binary(const char **obj_paths, int n_objs,
                 const char *output, const char *lib_dir);
