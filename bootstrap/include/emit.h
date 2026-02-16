/**
 * @file emit.h
 * @brief Source code emission from parse trees.
 *
 * Walks a tnode_t parse tree and outputs human-readable source code,
 * emitting #line directives to maintain source location information.
 */
#pragma once

#include <stdio.h>
#include "lex.h"
#include "types.h"

/**
 * @brief Emission context for source code output.
 */
typedef struct {
    ncc_buf_t  *input;        /**< Source buffer */
    tok_t      *tokens;       /**< Token array */
    int         num_tokens;   /**< Number of tokens */
    char       *filename;     /**< Main source filename */
    const char *cur_file;     /**< Current file (from #line directives) */
    FILE       *out;          /**< Output stream */
    int         out_line;     /**< Current output line number */
    int         src_line;     /**< Current source line number */
    bool        at_line_start; /**< True if at start of output line */
    bool        need_space;   /**< True if space needed before next token */
} emit_ctx_t;

/**
 * @brief Initialize emission context from lexer state.
 * @param ctx Emission context to initialize
 * @param state Lexer state with tokens
 * @param out Output stream (e.g., stdout or file)
 */
extern void emit_init(emit_ctx_t *ctx, lex_t *state, FILE *out);

/**
 * @brief Emit source code for a parse tree.
 *
 * Walks the parse tree and outputs the source code, emitting #line
 * directives when the source line changes unexpectedly.
 *
 * @param ctx Emission context
 * @param tree Parse tree root
 */
extern void emit_tree(emit_ctx_t *ctx, tnode_t *tree);

/**
 * @brief Emit just the file directive at the start.
 * @param ctx Emission context
 */
extern void emit_file_directive(emit_ctx_t *ctx);

/**
 * @brief Finalize emission (flush any pending output).
 * @param ctx Emission context
 */
extern void emit_finish(emit_ctx_t *ctx);
