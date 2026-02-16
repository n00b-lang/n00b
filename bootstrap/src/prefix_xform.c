/**
 * @file prefix_xform.c
 * @brief Literal modifier (prefix) transformation implementation.
 */

#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>

#include "prefix_xform.h"
#include "buf.h"

/* ============================================================================
 * Registry Management
 * ============================================================================ */

void
prefix_registry_init(prefix_registry_t *reg)
{
    reg->handlers = nullptr;
}

void
prefix_registry_free(prefix_registry_t *reg)
{
    prefix_handler_entry_t *entry = reg->handlers;
    while (entry) {
        prefix_handler_entry_t *next = entry->next;
        base_dealloc(entry);
        entry = next;
    }
    reg->handlers = nullptr;
}

bool
prefix_register(prefix_registry_t *reg, const char *prefix, prefix_handler_fn handler)
{
    prefix_handler_entry_t *entry = base_calloc(1, sizeof(prefix_handler_entry_t));
    if (!entry) {
        return false;
    }

    entry->prefix  = prefix;
    entry->handler = handler;
    entry->next    = reg->handlers;
    reg->handlers  = entry;

    return true;
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

bool
prefix_is_c_standard(const char *prefix)
{
    // C standard string prefixes that should NOT be transformed
    if (strcmp(prefix, "u") == 0)   return true;  // char16_t string
    if (strcmp(prefix, "U") == 0)   return true;  // char32_t string
    if (strcmp(prefix, "L") == 0)   return true;  // wchar_t string
    if (strcmp(prefix, "u8") == 0)  return true;  // UTF-8 string
    return false;
}

int
prefix_find_matching_brace(lex_t *state, int start_ix)
{
    int depth = 1;
    int i     = start_ix + 1;

    while (i < state->num_toks && depth > 0) {
        tok_t *t = &state->toks[i];

        if (t->type == TT_PUNCT) {
            char c = state->input->data[t->offset + t->prefix_len];
            if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) {
                    return i;
                }
            }
        }

        i++;
    }

    return -1; // Unbalanced braces
}

/**
 * @brief Get the prefix text from a token.
 */
static char *
get_prefix(lex_t *state, tok_t *tok)
{
    if (tok->prefix_len == 0) {
        return nullptr;
    }

    char *prefix = base_calloc(1, tok->prefix_len + 1);
    memcpy(prefix, state->input->data + tok->offset, tok->prefix_len);
    return prefix;
}

/**
 * @brief Find handler for a given prefix.
 */
static prefix_handler_fn
find_handler(prefix_registry_t *reg, const char *prefix)
{
    prefix_handler_entry_t *entry = reg->handlers;
    while (entry) {
        if (strcmp(entry->prefix, prefix) == 0) {
            return entry->handler;
        }
        entry = entry->next;
    }
    return nullptr;
}

/* ============================================================================
 * Transform Application
 * ============================================================================ */

bool
prefix_apply_transforms(prefix_registry_t *reg, lex_t *state)
{
    for (int i = 0; i < state->num_toks; i++) {
        tok_t *t = &state->toks[i];

        // Check for prefixed brace literal
        if (t->type == TT_PUNCT && t->prefix_len > 0) {
            char c = state->input->data[t->offset + t->prefix_len];

            if (c == '{') {
                // It's a prefixed brace literal
                char *prefix = get_prefix(state, t);
                if (!prefix) {
                    continue;
                }

                // Skip C standard prefixes
                if (prefix_is_c_standard(prefix)) {
                    base_dealloc(prefix);
                    continue;
                }

                // Find the handler
                prefix_handler_fn handler = find_handler(reg, prefix);
                if (handler) {
                    // Find matching close brace
                    int end_ix = prefix_find_matching_brace(state, i);
                    if (end_ix < 0) {
                        base_dealloc(prefix);
                        return false; // Unbalanced braces
                    }

                    // Call the handler
                    if (!handler(state, i, end_ix)) {
                        base_dealloc(prefix);
                        return false;
                    }
                }
                // If no handler registered, leave unchanged (pass through)

                base_dealloc(prefix);
            }
        }

        // Check for prefixed string literal
        if (t->type == TT_STR && t->prefix_len > 0) {
            char *prefix = get_prefix(state, t);
            if (!prefix) {
                continue;
            }

            // Skip C standard prefixes
            if (prefix_is_c_standard(prefix)) {
                base_dealloc(prefix);
                continue;
            }

            // Find the handler
            prefix_handler_fn handler = find_handler(reg, prefix);
            if (handler) {
                // For strings, end_ix is the same as start_ix (single token)
                // But we need to handle token-pasted strings (adjacent strings)
                int end_ix = i;

                // Look for adjacent string literals (token pasting)
                while (end_ix + 1 < state->num_toks) {
                    tok_t *next = &state->toks[end_ix + 1];
                    if (next->type == TT_STR) {
                        end_ix++;
                    } else if (next->type == TT_WS || next->type == TT_COMMENT) {
                        // Skip whitespace/comments, check next
                        int check = end_ix + 2;
                        while (check < state->num_toks &&
                               (state->toks[check].type == TT_WS ||
                                state->toks[check].type == TT_COMMENT)) {
                            check++;
                        }
                        if (check < state->num_toks &&
                            state->toks[check].type == TT_STR) {
                            end_ix = check;
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }

                // Call the handler
                if (!handler(state, i, end_ix)) {
                    base_dealloc(prefix);
                    return false;
                }
            }

            base_dealloc(prefix);
        }
    }

    return true;
}

// Backend-specific prefix registrations (provided by bootstrap or ncc build)
extern void ncc_register_extra_prefixes(prefix_registry_t *reg);

void
prefix_register_builtins(prefix_registry_t *reg)
{
    ncc_register_extra_prefixes(reg);
}
