/**
 * @file emit.c
 * @brief Source code emission from parse trees.
 */

#include <stdbool.h>
#include <string.h>
#include "base_alloc_shim.h"
#include "ncc_limits.h"
#include "emit.h"
#include "nt_types.h"
#include "token.h"

void
emit_init(emit_ctx_t *ctx, lex_t *state, FILE *out)
{
    *ctx = (emit_ctx_t){
        .input         = state->input,
        .tokens        = state->toks,
        .num_tokens    = state->num_toks,
        .filename      = state->in_file,
        .cur_file      = state->in_file,
        .out           = out,
        .out_line      = 1,
        .src_line      = 1,
        .at_line_start = true,
        .need_space    = false,
    };
}

void
emit_file_directive(emit_ctx_t *ctx)
{
    if (ctx->filename) {
        fprintf(ctx->out, "#line 1 \"%s\"\n", ctx->filename);
        ctx->out_line      = 1;
        ctx->src_line      = 1;
        ctx->at_line_start = true;
    }
}

/**
 * @brief Emit a #line directive if source and output lines are out of sync.
 */
static void
emit_line_directive_if_needed(emit_ctx_t *ctx,
                              int         src_line,
                              const char *src_file)
{
    // #line 0 is a GNU extension rejected by clang -Werror.
    // Clamp to 1 as a safety net for synthetic tokens.
    if (src_line <= 0) {
        src_line = 1;
    }

    // Check if we need a directive (line changed or file changed)
    bool file_changed = (src_file != ctx->cur_file)
                     && (ctx->cur_file == nullptr
                         || src_file == nullptr
                         || strcmp(src_file, ctx->cur_file) != 0);

    if (src_line != ctx->out_line || file_changed) {
        // If we're not at line start, emit a newline first
        if (!ctx->at_line_start) {
            fputc('\n', ctx->out);
            ctx->out_line++;
            ctx->at_line_start = true;
        }

        // Update tracked file
        ctx->cur_file = src_file;

        // Emit #line directive
        if (src_file) {
            fprintf(ctx->out, "#line %d \"%s\"\n", src_line, src_file);
        }
        else {
            fprintf(ctx->out, "#line %d\n", src_line);
        }
        ctx->out_line      = src_line;
        ctx->src_line      = src_line;
        ctx->at_line_start = true;
        ctx->need_space    = false;
    }
}

/**
 * @brief Count newlines in a string.
 */
static int
count_newlines_in_text(const char *text, int len)
{
    int count = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n') {
            count++;
        }
    }
    return count;
}

/**
 * @brief Emit a single token's text.
 *
 * For synthetic tokens, uses the replacement buffer instead of
 * indexing into the source buffer.
 */
static void
emit_token(emit_ctx_t *ctx, tok_t *tok)
{
    if (!tok) {
        return;
    }

    const char *text;
    int         len;

    if (tok->replacement != nullptr) {
        text = tok->replacement->data;
        len  = tok->replacement->len;
    }
    else {
        text = ctx->input->data + tok->offset;
        len  = tok->len;
    }

    // Check if we need a #line directive.
    // Synthetic tokens (from tree transforms) have src_file == NULL;
    // inherit the current tracked file so we don't lose file context.
    int         tok_line = tok->line_no;
    const char *tok_file = tok->src_file ? tok->src_file : ctx->cur_file;
    if (tok->type != TT_WS && tok->type != TT_COMMENT) {
        emit_line_directive_if_needed(ctx, tok_line, tok_file);
    }

    // Normalize asm keywords to __asm__ for C23 compatibility (C23
    // doesn't recognize bare "asm" as a keyword, but some compilers
    // we proxy to use it)
    if (tok->type == TT_KEYWORD) {
        if ((len == 3
             && memcmp(text, "asm", 3) == 0)
            || (len == 5 && memcmp(text, "__asm", 5) == 0)) {
            fwrite("__asm__", 1, 7, ctx->out);
            goto update_tracking;
        }
    }

    // Write the token text
    fwrite(text, 1, len, ctx->out);

update_tracking:

    // Update line tracking
    int newlines = count_newlines_in_text(text, len);
    ctx->out_line += newlines;
    ctx->src_line = tok_line + newlines;

    // Track if we're at line start
    if (newlines > 0) {
        ctx->at_line_start = (text[len - 1] == '\n');
    }
    else {
        ctx->at_line_start = false;
    }

    // Track if we need space before next token
    ctx->need_space = (tok->type != TT_WS && tok->type != TT_PUNCT);
}

/**
 * @brief Set of NT types that need comma separators between children.
 *
 * After tree flattening, these NTs have children without comma tokens.
 * The emitter needs to insert commas between children.
 */
#define NT_SET_COMMA_SEPARATED          \
    (NT_BIT(NT_argument_expression_list) | \
     NT_BIT(NT_enumerator_list)         | \
     NT_BIT(NT_generic_assoc_list)      | \
     NT_BIT(NT_init_declarator_list)    | \
     NT_BIT(NT_parameter_list)          | \
     NT_BIT(NT_member_declarator_list)  | \
     NT_BIT(NT_keyword_param_list)      | \
     NT_BIT(NT_attribute_list))

/**
 * @brief Check if a non-terminal represents a binary expression context.
 *
 * Binary expression NTs indicate the operator should have spaces around it.
 */
static bool
is_binary_expression_nt(nt_type_t nt_id)
{
    switch (nt_id) {
    case NT_additive_expression:
    case NT_multiplicative_expression:
    case NT_shift_expression:
    case NT_relational_expression:
    case NT_equality_expression:
    case NT_AND_expression:
    case NT_exclusive_OR_expression:
    case NT_inclusive_OR_expression:
    case NT_logical_AND_expression:
    case NT_logical_OR_expression:
    case NT_assignment_expression:
    case NT_conditional_expression:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Check if token is an operator that needs spacing in binary context.
 */
static bool
is_spacing_operator(const char *text, int len)
{
    if (len == 1) {
        switch (text[0]) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '<':
        case '>':
        case '&':
        case '|':
        case '^':
        case '=':
        case '?':
        case ':':
            return true;
        default:
            return false;
        }
    }
    else if (len == 2) {
        if ((text[0] == '<' && text[1] == '<') || // <<
            (text[0] == '>' && text[1] == '>') || // >>
            (text[0] == '=' && text[1] == '=') || // ==
            (text[0] == '!' && text[1] == '=') || // !=
            (text[0] == '<' && text[1] == '=') || // <=
            (text[0] == '>' && text[1] == '=') || // >=
            (text[0] == '&' && text[1] == '&') || // &&
            (text[0] == '|' && text[1] == '|') || // ||
            (text[0] == '+' && text[1] == '=') || // +=
            (text[0] == '-' && text[1] == '=') || // -=
            (text[0] == '*' && text[1] == '=') || // *=
            (text[0] == '/' && text[1] == '=') || // /=
            (text[0] == '%' && text[1] == '=') || // %=
            (text[0] == '&' && text[1] == '=') || // &=
            (text[0] == '|' && text[1] == '=') || // |=
            (text[0] == '^' && text[1] == '=')) { // ^=
            return true;
        }
    }
    else if (len == 3) {
        if ((text[0] == '<' && text[1] == '<' && text[2] == '=') || // <<=
            (text[0] == '>' && text[1] == '>' && text[2] == '=')) { // >>=
            return true;
        }
    }
    return false;
}

/**
 * @brief Info about the previously emitted token, including context.
 */
typedef struct {
    tok_t    *tok;          /**< The previous token */
    nt_type_t parent_nt_id; /**< Parent non-terminal ID of the token */
} emit_prev_t;

/**
 * @brief Emit a terminal node's tokens.
 */
static void
emit_terminal(emit_ctx_t *ctx, tnode_t *node, emit_prev_t *prev)
{
    int num_toks_to_emit = node->num_toks > 0 ? node->num_toks : 1;

    if (node->tptr->synthetic) {
        tok_t *tok = node->tptr;
        if (!tok->skip_emit) {
            if (prev->tok != nullptr) {
                if ((prev->tok->type == TT_ID || prev->tok->type == TT_KEYWORD || prev->tok->type == TT_NUM) && (tok->type == TT_ID || tok->type == TT_KEYWORD || tok->type == TT_NUM)) {
                    fputc(' ', ctx->out);
                }
            }
            emit_token(ctx, tok);
            prev->tok          = tok;
            prev->parent_nt_id = node->parent ? node->parent->nt_id : NT_NONE;
        }
        return;
    }

    // Pointer arithmetic: tptr points into ctx->tokens array.
    int tok_index = (int)(node->tptr - ctx->tokens);
    if (tok_index < 0 || tok_index >= ctx->num_tokens) {
        tok_index = -1;
    }

    int emitted = 0;
    for (int t = tok_index; t >= 0 && t < ctx->num_tokens && emitted < num_toks_to_emit; t++) {
        tok_t *tok = &ctx->tokens[t];

        if (tok->type == TT_WS || tok->type == TT_COMMENT) {
            continue;
        }
        if (tok->skip_emit) {
            emitted++;
            continue;
        }

        if (prev->tok != nullptr) {
            tok_t *ptok       = prev->tok;
            bool   need_space = false;

            int         prev_len;
            const char *prev_text = get_token_text(ctx->input, ptok, &prev_len);
            int         tok_len;
            const char *tok_text = get_token_text(ctx->input, tok, &tok_len);

            bool tok_is_closing = (tok->type == TT_PUNCT && tok_len == 1 && (tok_text[0] == ')' || tok_text[0] == ']' || tok_text[0] == '}'));

            nt_type_t parent_nt_id       = node->parent ? node->parent->nt_id : NT_NONE;
            bool      in_binary_ctx      = is_binary_expression_nt(parent_nt_id);
            bool      prev_in_binary_ctx = is_binary_expression_nt(prev->parent_nt_id);

            if ((ptok->type == TT_ID || ptok->type == TT_KEYWORD || ptok->type == TT_NUM) && (tok->type == TT_ID || tok->type == TT_KEYWORD || tok->type == TT_NUM)) {
                need_space = true;
            }
            if (ptok->type == TT_PUNCT && prev_len == 1 && prev_text[0] == ',') {
                need_space = true;
            }
            if (ptok->type == TT_PUNCT && prev_len == 1 && prev_text[0] == ';' && !tok_is_closing && !(tok->type == TT_PUNCT && tok_len == 1 && tok_text[0] == ';')) {
                need_space = true;
            }
            if (ptok->type == TT_PUNCT && is_spacing_operator(prev_text, prev_len)) {
                if (prev_in_binary_ctx && !tok_is_closing) {
                    need_space = true;
                }
            }
            if (tok->type == TT_PUNCT && is_spacing_operator(tok_text, tok_len)) {
                if (in_binary_ctx) {
                    need_space = true;
                }
            }
            if (ptok->type == TT_PUNCT && prev_len == 1 && prev_text[0] == '{' && !tok_is_closing) {
                need_space = true;
            }
            if (tok->type == TT_PUNCT && tok_len == 1 && tok_text[0] == '}') {
                if (ptok->type != TT_PUNCT || (prev_len == 1 && prev_text[0] == ';')) {
                    need_space = true;
                }
            }
            if (ptok->type == TT_PUNCT && prev_len == 1 && prev_text[0] == ')' && tok->type == TT_PUNCT && tok_len == 1 && tok_text[0] == '{') {
                need_space = true;
            }
            if (ptok->type == TT_KEYWORD && tok->type == TT_PUNCT && tok_len == 1 && tok_text[0] == '(') {
                if (prev_len == 2 && strncmp(prev_text, "if", 2) == 0) {
                    need_space = true;
                }
                else if (prev_len == 5 && strncmp(prev_text, "while", 5) == 0) {
                    need_space = true;
                }
                else if (prev_len == 3 && strncmp(prev_text, "for", 3) == 0) {
                    need_space = true;
                }
                else if (prev_len == 6 && strncmp(prev_text, "switch", 6) == 0) {
                    need_space = true;
                }
            }
            if (ptok->type == TT_PUNCT && prev_len == 1 && prev_text[0] == ')') {
                if (tok->type == TT_KEYWORD) {
                    need_space = true;
                }
                else if (tok->type == TT_PUNCT && tok_len == 1 && tok_text[0] == '{') {
                    if (parent_nt_id != NT_postfix_expression) {
                        need_space = true;
                    }
                }
                else if (tok->type == TT_ID || tok->type == TT_NUM) {
                    if (parent_nt_id != NT_cast_expression && parent_nt_id != NT_primary_expression && parent_nt_id != NT_postfix_expression) {
                        need_space = true;
                    }
                }
            }
            if (ptok->type == TT_PUNCT && prev_len == 1 && prev_text[0] == '}' && tok->type == TT_KEYWORD) {
                need_space = true;
            }
            if (ptok->type == TT_KEYWORD && prev_len == 2 && strncmp(prev_text, "do", 2) == 0 && tok->type == TT_PUNCT && tok_len == 1 && tok_text[0] == '{') {
                need_space = true;
            }

            if (need_space) {
                fputc(' ', ctx->out);
            }
        }

        emit_token(ctx, tok);
        prev->tok          = tok;
        prev->parent_nt_id = node->parent ? node->parent->nt_id : NT_NONE;
        emitted++;
    }
}

/**
 * @brief Iterative pre-order tree walk for emission.
 *
 * Uses an explicit heap stack to avoid stack overflow on deeply nested trees
 * (e.g. initializer lists with 50K+ elements).
 */
typedef struct {
    tnode_t *node;
    int      kid_index;
    bool     needs_commas;
} emit_frame_t;

static void
emit_tree_iterative(emit_ctx_t *ctx, tnode_t *root, emit_prev_t *prev)
{
    if (!root) {
        return;
    }

    int           cap   = 256;
    emit_frame_t *stack = base_alloc(cap * sizeof(emit_frame_t));
    int           sp    = 0;

    stack[sp++] = (emit_frame_t){.node = root, .kid_index = -1};

    while (sp > 0) {
        emit_frame_t *f   = &stack[sp - 1];
        tnode_t      *cur = f->node;

        if (!cur || IS_ELIDED(cur)) {
            sp--;
            continue;
        }

        // First visit
        if (f->kid_index == -1) {
            if (cur->tptr) {
                emit_terminal(ctx, cur, prev);
                sp--;
                continue;
            }
            f->needs_commas = NT_IN_SET(cur->nt_id, NT_SET_COMMA_SEPARATED)
                           && cur->origin != nullptr
                           && cur->origin->rewrite_name != nullptr
                           && strcmp(cur->origin->rewrite_name, "flatten") == 0;
            f->kid_index = 0;
        }

        // Done with kids?
        if (f->kid_index >= cur->num_kids) {
            sp--;
            continue;
        }

        int      i   = f->kid_index++;
        tnode_t *kid = tnode_get_kid(cur, i);

        if (!kid || IS_ELIDED(kid)) {
            continue;
        }

        if (f->needs_commas && i > 0 && prev->tok != nullptr) {
            fputc(',', ctx->out);
            fputc(' ', ctx->out);
            ctx->at_line_start = false;
            static ncc_buf_t comma_buf = {.data = ",", .len = 1};
            static tok_t     comma_tok = {.type = TT_PUNCT, .len = 1, .replacement = &comma_buf, .synthetic = 1};
            prev->tok          = &comma_tok;
            prev->parent_nt_id = cur->nt_id;
        }

        if (sp >= cap) {
            cap *= 2;
            if (cap > (1 << 20)) {
                fprintf(stderr, "emit: tree walk exceeded %d nodes "
                        "(likely cycle from corrupt tree)\n", 1 << 20);
                base_dealloc(stack);
                return;
            }
            stack = base_realloc(stack, cap * sizeof(emit_frame_t));
        }
        stack[sp++] = (emit_frame_t){.node = kid, .kid_index = -1};
    }

    base_dealloc(stack);
}

void
emit_tree(emit_ctx_t *ctx, tnode_t *tree)
{
    if (!tree) {
        return;
    }

    emit_prev_t prev = {.tok = nullptr, .parent_nt_id = NT_NONE};
    emit_tree_iterative(ctx, tree, &prev);
}

void
emit_finish(emit_ctx_t *ctx)
{
    // Ensure output ends with a newline
    if (!ctx->at_line_start) {
        fputc('\n', ctx->out);
        ctx->out_line++;
        ctx->at_line_start = true;
    }

    fflush(ctx->out);
}
