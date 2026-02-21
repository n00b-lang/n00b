/**
 * @file lex.h
 * @brief Lexer state and tokenization functions.
 *
 * The lex() function properly handles multi-character operators:
 * - Increment/decrement: ++, --
 * - Pointer member: ->
 * - Comparison: ==, !=, <=, >=
 * - Logical: &&, ||
 * - Shift: <<, >>
 * - Compound assignment: +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=
 */
#pragma once

#include <stdbool.h>
#include "token.h"
#include "buf.h"

/**
 * @brief Range where ncc transformations are disabled.
 */
typedef struct {
    int start;  /**< Token index of #pragma ncc off */
    int end;    /**< Token index of #pragma ncc on (or num_toks if unclosed) */
} ncc_off_range_t;

/**
 * @brief Lexer state for tokenizing source code.
 */
typedef struct lex_t {
    ncc_buf_t       *input;        /**< Source buffer being tokenized */
    tok_t           *toks;         /**< Array of tokens produced */
    ncc_off_range_t *ncc_off_ranges; /**< Ranges where ncc is disabled */
    char            *cur;          /**< Current position in input */
    char            *end;          /**< End of input buffer */
    char            *in_file;      /**< Input filename */
    char            *out_file;     /**< Output filename (for transforms) */
    const char      *cur_src_file; /**< Current source file (from #line directives) */
    int              num_toks;     /**< Number of tokens produced */
    int              toks_cap;     /**< Allocated capacity of toks array */
    int              num_ranges;   /**< Number of ncc_off ranges */
    int              offset;       /**< Current byte offset */
    int              line_no;      /**< Current line number (1-based) */
    bool             line_start;       /**< True if at start of line (for preprocessor) */
    bool             in_system_header; /**< True if inside a system header (CPP flag 3) */
} lex_t;

/** @brief Forward declaration for transform context */
typedef struct tok_xform_t tok_xform_t;

/**
 * @brief Initialize lexer state.
 * @param ctx Lexer context to initialize
 * @param input Source buffer to tokenize
 * @param in_file Input filename for error messages
 */
extern void lex_init(lex_t *ctx, ncc_buf_t *input, char *in_file);

/**
 * @brief Ensure there is room for at least one more token.
 *
 * Doubles the backing array when full. Called before every token write.
 */
extern void lex_ensure_tok_space(lex_t *state);

/**
 * @brief Tokenize the source buffer.
 *
 * Populates the token array in the lexer state. Handles multi-character
 * operators as single tokens. Also builds ncc_off ranges from #pragma ncc
 * directives.
 *
 * @param state Initialized lexer state
 */
extern void lex(lex_t *state);

/**
 * @brief Check if a token is in an ncc_off range.
 *
 * @param state Lexer state with ncc_off ranges
 * @param tok_ix Token index to check
 * @return true if the token is within a #pragma ncc off region
 */
extern bool lex_is_ncc_off(lex_t *state, int tok_ix);

/**
 * @brief Find token index by pointer.
 *
 * @param state Lexer state
 * @param tok Token pointer to find
 * @return Token index, or -1 if not found
 */
extern int lex_find_token_index(lex_t *state, tok_t *tok);

/**
 * @brief Check if a token pointer is in an ncc_off range.
 *
 * Convenience function that combines lex_find_token_index and lex_is_ncc_off.
 *
 * @param state Lexer state with ncc_off ranges
 * @param tok Token pointer to check
 * @return true if the token is within a #pragma ncc off region
 */
extern bool lex_tok_is_ncc_off(lex_t *state, tok_t *tok);

/**
 * @brief Advance to next non-comment token.
 * @param ctx Transform context
 * @param skip_ws If true, also skip whitespace tokens
 * @return Next token, or nullptr at end
 */
extern tok_t *advance(tok_xform_t *ctx, bool skip_ws);

/**
 * @brief Move back to previous non-comment token.
 * @param ctx Transform context
 * @param skip_ws If true, also skip whitespace tokens
 * @return Previous token, or nullptr at start
 */
extern tok_t *backup(tok_xform_t *ctx, bool skip_ws);

/**
 * @brief Check if a token matches an identifier string.
 * @param to_match String to compare against
 * @param input Source buffer
 * @param offset Token offset in buffer
 * @param len Token length
 * @return true if token text matches string
 */
extern bool id_check(char *to_match, ncc_buf_t *input, int offset, int len);

/**
 * @brief Extract token text as a new string.
 * @param input Source buffer
 * @param tok Token to extract
 * @return Newly allocated string (caller must free)
 */
extern char *extract(ncc_buf_t *input, tok_t *tok);

/**
 * @brief Check if token is a name (identifier or keyword).
 *
 * Many transforms need to check if a token represents a name, which
 * can be either an identifier (TT_ID) or a keyword used as a name (TT_KEYWORD).
 *
 * @param t Token to check
 * @return true if token is TT_ID or TT_KEYWORD, false otherwise
 */
static inline bool
tok_is_name(tok_t *t)
{
    return t && (t->type == TT_ID || t->type == TT_KEYWORD);
}

