/**
 * @file compile.h
 * @brief Compilation driver interface.
 *
 * Provides the main entry points for compiling source files,
 * either with ncc transformations or as a passthrough to the
 * underlying compiler.
 */
#pragma once

#include "argv_parse.h"
#include "buf.h"
#include "lex.h"
#include "st.h"
#include "types.h"

/**
 * @brief Find the underlying C compiler to use.
 *
 * Checks NCC_COMPILER, then CC, then falls back to "cc".
 * Skips any value that points to ncc itself to avoid infinite recursion.
 *
 * @return Compiler path (may be a static string or env pointer)
 */
extern char *ncc_find_compiler(void);

/**
 * @brief Invoke the C preprocessor on the given input.
 *
 * Forks the underlying compiler with -E, feeds it the input on stdin,
 * and returns the preprocessed output.
 *
 * @param ctx      Parsed command-line arguments
 * @param compiler Path to the compiler
 * @param input    Input buffer (consumed — caller must not use after call)
 * @return Preprocessed output buffer (caller owns)
 */
extern ncc_buf_t *ncc_invoke_preprocessor(ncc_argv_t *ctx, char *compiler, ncc_buf_t *input);

/**
 * @brief Pass arguments directly to the underlying compiler.
 *
 * Used when ncc should act as a transparent wrapper without
 * performing any transformations.
 *
 * @param ctx Parsed command-line arguments
 * @note This function does not return (calls exec)
 */
[[noreturn]]
extern void compiler_passthrough(ncc_argv_t *ctx);

/**
 * @brief Compile a source file with ncc transformations.
 *
 * Each backend (bootstrap / main NCC) provides its own implementation.
 *
 * @param ctx Parsed command-line arguments
 */
extern void compile_file(ncc_argv_t *ctx);

/**
 * @brief Compilation context containing all state for a compilation unit.
 */
typedef struct compile_ctx_t {
    ncc_argv_t *argv;           /**< Command-line arguments */
    char       *compiler;       /**< Path to underlying compiler */
    lex_t       lex_state;      /**< Lexer state for current file */
    tnode_t    *tree;           /**< Parse tree (after parsing) */
    bool        has_transforms; /**< True if ncc transformations were applied */
} compile_ctx_t;
