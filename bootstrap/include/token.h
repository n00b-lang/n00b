/**
 * @file token.h
 * @brief Core token definitions used by all lexer and parser modules.
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include "buf.h"

/**
 * @brief Token type identifier.
 */
typedef int8_t ttype_t;

/** @name Token Type Constants
 * @{
 */
#define TT_ERR     -1 /**< Error/invalid token */
#define TT_WS      0  /**< Whitespace (spaces, tabs, newlines) */
#define TT_ID      1  /**< Identifier */
#define TT_KEYWORD 2  /**< C keyword (if, while, int, etc.) */
#define TT_NUM     3  /**< Numeric literal */
#define TT_COMMENT 4  /**< Comment (line or block style) */
#define TT_STR     5  /**< String literal */
#define TT_CHR     6  /**< Character literal */
#define TT_PUNCT   7  /**< Punctuation/operator */
#define TT_PREPROC 8  /**< Preprocessor directive */
#define TT_UNKNOWN 9  /**< Unknown/unrecognized character */
/** @} */

/**
 * @brief Base offset for synthetic tokens.
 *
 * Synthetic tokens use negative offsets starting from this base,
 * combined with a sequence number. This allows tok_cmp() to
 * distinguish synthetic tokens and sort them by their line_no.
 */
#define SYNTHETIC_OFFSET_BASE INT32_MIN

/**
 * @brief Token structure representing a lexical token.
 *
 * Tokens reference positions in the original source buffer rather than
 * copying text, for efficiency. The replacement field allows transforms
 * to substitute new text for the original token.
 *
 * For synthetic tokens (created by rewrite operations):
 * - synthetic = 1
 * - replacement contains the token text (required)
 * - offset = SYNTHETIC_OFFSET_BASE + sequence_number
 * - line_no = source line for #line directive positioning
 */
typedef struct {
    ncc_buf_t  *replacement;    /**< Replacement text (nullptr if unchanged, required for synthetics) */
    const char *src_file;       /**< Source filename (from #line directives, may be shared) */
    int         offset;         /**< Byte offset in source buffer (negative for synthetics) */
    int         len;            /**< Length of token in bytes */
    int         line_no;        /**< Source line number (1-based) */
    ttype_t     type;           /**< Token type (TT_* constant) */
    uint8_t     skip_emit  : 1; /**< If set, token is suppressed in output */
    uint8_t     synthetic  : 1; /**< If set, token was synthesized (not from source) */
    uint8_t     prefix_len : 6; /**< Length of literal prefix (e.g., L, u, U, u8) for string/char literals */
} tok_t;

/**
 * @brief Extract the prefix from a literal token.
 *
 * For prefixed string/character literals (e.g., L"hello", u8'x'),
 * returns an allocated string containing just the prefix portion.
 *
 * @param input Source buffer
 * @param tok Token to extract prefix from
 * @return Allocated string with prefix, or nullptr if no prefix.
 *         Caller must free the returned string.
 */
static inline char *
tok_get_prefix(ncc_buf_t *input, tok_t *tok)
{
    if (tok->prefix_len == 0) {
        return nullptr;
    }
    char *prefix = base_alloc(tok->prefix_len + 1);
    memcpy(prefix, input->data + tok->offset, tok->prefix_len);
    prefix[tok->prefix_len] = '\0';
    return prefix;
}

/**
 * @brief Extract token text as a new string.
 * @param input Source buffer
 * @param tok Token to extract
 * @return Newly allocated string (caller must free)
 */
extern char *extract(ncc_buf_t *input, tok_t *tok);

static inline const char *
get_token_text(ncc_buf_t *input, tok_t *tok, int *len)
{
    if (!tok) {
        return nullptr;
    }
    if (tok->replacement) {
        if (len) {
            *len = tok->replacement->len;
        }
        return tok->replacement->data;
    }

    if (len) {
        *len = tok->len;
    }
    return extract(input, tok);
}

/**
 * @brief Get pointer to token text without allocation.
 *
 * Returns a pointer to the token's text data and sets *len to the length.
 * For synthetic tokens, returns the replacement buffer data.
 * For normal tokens, returns a pointer into the input buffer.
 *
 * @param input Source buffer
 * @param tok   Token to get text from
 * @param len   Output: length of the token text
 * @return Pointer to token text (not null-terminated)
 */
static inline const char *
tok_text_ptr(const ncc_buf_t *input, const tok_t *tok, int *len)
{
    if (tok->replacement) {
        *len = tok->replacement->len;
        return tok->replacement->data;
    }
    *len = tok->len;
    return input->data + tok->offset;
}
