/**
 * @file xform.h
 * @brief Transform framework for source code transformations.
 *
 * Provides the infrastructure for applying transformations to tokenized
 * source code before compilation.
 */
#pragma once

#include <stdbool.h>
#include "lex.h"

/**
 * @brief Keyword usage tracking context.
 *
 * Used to track context during keyword argument transformations.
 */
typedef struct kw_use_ctx_t {
    struct kw_use_ctx_t *next;         /**< Next context in stack */
    bool                 id;           /**< In identifier context */
    bool                 started_kobj; /**< Started keyword object */
    int                  kw_count;     /**< Keyword argument count */
} kw_use_ctx_t;

/**
 * @brief Transform context for processing token streams.
 */
struct tok_xform_t {
    ncc_buf_t    *input;            /**< Source buffer */
    tok_t        *toks;             /**< Token array */
    char         *in_file;          /**< Input filename */
    int           ix;               /**< Current token index */
    int           max;              /**< Maximum token index */
    int           rewrite_start_ix; /**< Start of current rewrite region */
    int           id_nest;          /**< Identifier nesting depth */
    kw_use_ctx_t *kw_stack;         /**< Keyword tracking stack */
    ncc_buf_t    *kw_comma_buf;     /**< Keyword arg comma replacement */
    ncc_buf_t    *kw_tri_paren;     /**< Keyword arg closing parens */
    ncc_buf_t    *kw_post_comma;    /**< Keyword arg post-comma replacement */
};

/** @name Core Transform Functions (xforms.c)
 * @{
 */

/**
 * @brief Apply all transforms to a lexed source file.
 * @param state Lexer state with tokens
 * @return true if any transforms were applied
 */
extern bool apply_transforms(lex_t *state);

/**
 * @brief Finish a rewrite operation.
 * @param ctx Transform context
 * @param success Whether rewrite succeeded
 */
extern void finish_rewrite(tok_xform_t *ctx, bool success);

/**
 * @brief Extract the line containing a token.
 * @param ctx Transform context
 * @param t Token to extract line for
 * @return Newly allocated string containing the line
 */
extern char *extract_line(tok_xform_t *ctx, tok_t *t);

/**
 * @brief Extract a range of tokens as text.
 * @param ctx Transform context
 * @param start_ix Start token index
 * @param end_ix End token index
 * @return Newly allocated string containing the range
 */
extern char *extract_range(tok_xform_t *ctx, int start_ix, int end_ix);

/**
 * @brief Skip forward to a specific punctuation character.
 * @param ctx Transform context
 * @param punc Punctuation character to find
 * @return Token at the punctuation, or nullptr
 */
extern tok_t *skip_forward_to_punct(tok_xform_t *ctx, char punc);

/** @} */

/** @name Keyword Transform (x_keyword.c)
 * @{
 */

/**
 * @brief Apply keyword argument transformation.
 * @param ctx Transform context
 * @param t Current token
 * @return true if transform was applied
 */
extern bool keyword_xform(tok_xform_t *ctx, tok_t *t);

/**
 * @brief Track keyword usage for context.
 * @param ctx Transform context
 * @param t Current token
 */
extern void kw_tracking(tok_xform_t *ctx, tok_t *t);

/** @} */

