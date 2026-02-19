/**
 * @file xform_constexpr_paste.c
 * @brief Transform constexpr_paste("prefix", expr) into a synthetic identifier.
 *
 * Evaluates the integer expression at compile time and concatenates
 * the string prefix with the result to produce an identifier token.
 * For example: constexpr_paste("item_", 3) -> item_3
 */

#include "branch_symbols.h"
#include "compile.h"
#include "ncc_limits.h"
#include "transform.h"
#include "rewrite.h"
#include "types.h"
#include "nt_types.h"
#include "emit.h"
#include "st.h"
#include "lex.h"
#include "token.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>

// Helpers from xform_constexpr.c (made non-static for reuse)
extern char *emit_node_to_string(tree_xform_t *ctx, tnode_t *node);
extern char *strip_line_directives(const char *src);
extern char *compile_and_run(const char *compiler, const char *source,
                             char **err_out);
extern char *emit_declarations(tree_xform_t *ctx, tnode_t *call_node);

static tnode_t *
xform_constexpr_paste(tree_xform_t *ctx, tnode_t *node)
{
    if (node->branch != BRANCH(synthetic_identifier, CONSTEXPR_PASTE)) {
        return nullptr;
    }

    int         line = get_node_line(node);
    const char *file = ctx->lex ? ctx->lex->in_file : "<unknown>";

    // Kids layout:
    //   [0] = constexpr_paste (identifier token)
    //   [1] = "("
    //   [2] = string_literal
    //   [3] = ","
    //   [4] = assignment_expression
    //   [5] = ")"

    // --- Extract prefix string from kid[2] (string_literal) ---
    tnode_t *str_node = tnode_get_kid(node, 2);
    if (!str_node) {
        ncc_error("%s:%d: constexpr_paste: missing string literal\n",
                  file, line);
        exit(1);
    }

    // Drill down to the terminal token
    tnode_t *str_term = str_node;
    while (str_term && !str_term->tptr) {
        str_term = tnode_get_kid(str_term, 0);
    }
    if (!str_term || !str_term->tptr) {
        ncc_error("%s:%d: constexpr_paste: cannot find string token\n",
                  file, line);
        exit(1);
    }

    char *raw_str = extract(ctx->input, str_term->tptr);
    if (!raw_str) {
        ncc_error("%s:%d: constexpr_paste: cannot extract string\n",
                  file, line);
        exit(1);
    }

    // Strip surrounding quotes
    int slen = strlen(raw_str);
    if (slen >= 2 && raw_str[0] == '"' && raw_str[slen - 1] == '"') {
        memmove(raw_str, raw_str + 1, slen - 2);
        raw_str[slen - 2] = '\0';
    }

    // Validate prefix is a valid identifier fragment
    for (char *p = raw_str; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') {
            ncc_error(
                "%s:%d: constexpr_paste() string literal contains "
                "invalid identifier character '%c'\n",
                file, line, *p);
            exit(1);
        }
    }

    // --- Evaluate integer expression from kid[4] ---
    tnode_t *expr_node = tnode_get_kid(node, 4);
    if (!expr_node) {
        ncc_error("%s:%d: constexpr_paste: missing expression\n",
                  file, line);
        exit(1);
    }

    char *expr_str = emit_node_to_string(ctx, expr_node);
    if (!expr_str) {
        ncc_error("%s:%d: constexpr_paste: cannot emit expression\n",
                  file, line);
        exit(1);
    }

    char *clean_expr = strip_line_directives(expr_str);
    base_dealloc(expr_str);

    // Trim leading/trailing whitespace from the expression
    char *trimmed = clean_expr;
    while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\n') {
        trimmed++;
    }
    char *end = trimmed + strlen(trimmed);
    while (end > trimmed &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n')) {
        end--;
    }
    size_t trim_len = end - trimmed;
    char  *expr     = base_alloc(trim_len + 1);
    memcpy(expr, trimmed, trim_len);
    expr[trim_len] = '\0';

    // Fast path: try to parse as a plain integer literal
    char     *endptr;
    long long value;
    errno = 0;
    value = strtoll(expr, &endptr, 0);

    // Skip trailing whitespace
    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\n') {
        endptr++;
    }

    if (errno != 0 || *endptr != '\0' || endptr == expr) {
        // Fallback: compile and run the expression.
        // The expression is expected to be pure integer arithmetic
        // (typically from macro counter expansion like (0), ((0)+1), etc.)
        // so we do NOT include project declarations — only standard headers.
        compile_ctx_t *cctx     = (compile_ctx_t *)ctx->user_data;
        const char    *compiler = cctx ? cctx->compiler : nullptr;
        if (!compiler) {
            ncc_error("%s:%d: constexpr_paste: no compiler available\n",
                      file, line);
            exit(1);
        }

        // Build a minimal C program that prints the expression value
        char  *source = nullptr;
        size_t source_size;
        FILE  *sf = open_memstream(&source, &source_size);
        fprintf(sf, "#include <stdio.h>\n");
        fprintf(sf, "#include <stddef.h>\n");
        fprintf(sf, "#include <stdint.h>\n");
        fprintf(sf, "int main(void) {\n");
        fprintf(sf, "    printf(\"%%lld\", (long long)(%s));\n", expr);
        fprintf(sf, "    return 0;\n");
        fprintf(sf, "}\n");
        fclose(sf);

        char *err_msg = nullptr;
        char *result  = compile_and_run(compiler, source, &err_msg);
        base_dealloc(source);

        if (!result) {
            ncc_error(
                "%s:%d: constexpr_paste: failed to evaluate expression '%s'\n",
                file, line, expr);
            if (err_msg) {
                fprintf(stderr, "%s\n", err_msg);
                base_dealloc(err_msg);
            }
            exit(1);
        }
        base_dealloc(err_msg);

        errno = 0;
        value = strtoll(result, &endptr, 10);
        if (errno != 0 || *endptr != '\0') {
            ncc_error(
                "%s:%d: constexpr_paste: expression '%s' did not produce "
                "an integer (got '%s')\n",
                file, line, expr, result);
            base_dealloc(result);
            exit(1);
        }
        base_dealloc(result);
    }

    base_dealloc(expr);

    base_dealloc(clean_expr);

    // --- Concatenate prefix + value ---
    char id_buf[NCC_IDENT_BUF];
    int  iret = snprintf(id_buf, sizeof(id_buf), "%s%lld", raw_str, value);
    NCC_CHECK_SNPRINTF(iret, id_buf);
    base_dealloc(raw_str);

    // --- Produce synthetic identifier and replace ---
    tnode_t *id_node = synth_terminal(id_buf, TT_ID, line);
    return replace_node(node, id_node, "constexpr_paste");
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void
register_constexpr_paste_xform(xform_registry_t *reg)
{
    xform_register_post(reg,
                        NT_synthetic_identifier,
                        xform_constexpr_paste,
                        "constexpr_paste");
}
