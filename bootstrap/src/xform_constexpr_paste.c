/**
 * @file xform_constexpr_paste.c
 * @brief Transform constexpr_paste(expr, expr) into a synthetic identifier.
 *
 * Concatenates the emitted text of two expressions to produce an identifier.
 * Both arguments may be string literals, bare identifiers, integer
 * expressions, or other compile-time expressions (e.g. typeid()).
 *
 * Examples:
 *   constexpr_paste("item_", 3)             -> item_3
 *   constexpr_paste(field_, typeid(int))     -> field_<hash>
 *   constexpr_paste("n00b_", "list")         -> n00b_list
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
#include "xform_helpers.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>

// Helpers still in xform_constexpr.c (not general enough for xform_helpers)
extern char *compile_and_run(const char *compiler, const char *source,
                             char **err_out);
extern char *emit_declarations(tree_xform_t *ctx, tnode_t *call_node);

// Check whether a string is a valid C identifier fragment (alnum + underscore).
static bool
is_ident_fragment(const char *s)
{
    if (!s || !*s) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') {
            return false;
        }
    }
    return true;
}

// Emit a parse-tree node to a cleaned, trimmed string.
// Strips line directives and leading/trailing whitespace.
// If the result is a quoted string, strips the surrounding quotes.
// Returns a base_alloc'd string that the caller must free.
static char *
emit_and_clean(tree_xform_t *ctx, tnode_t *node)
{
    char *raw = emit_node_to_string(ctx, node);
    if (!raw) {
        return nullptr;
    }

    char *clean = strip_line_directives(raw);
    base_dealloc(raw);

    // Trim leading whitespace
    char *start = clean;
    while (*start == ' ' || *start == '\t' || *start == '\n') {
        start++;
    }

    // Trim trailing whitespace
    char *end = start + strlen(start);
    while (end > start &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n')) {
        end--;
    }

    size_t len    = end - start;
    char  *result = base_alloc(len + 1);
    memcpy(result, start, len);
    result[len] = '\0';
    base_dealloc(clean);

    // Strip surrounding quotes if present (backward compat with string literals)
    size_t rlen = strlen(result);
    if (rlen >= 2 && result[0] == '"' && result[rlen - 1] == '"') {
        memmove(result, result + 1, rlen - 2);
        result[rlen - 2] = '\0';
    }

    return result;
}

// Try to evaluate an expression string as an integer.
// Returns true if successful, writing the value to *out.
// Tries strtoll first, then compile-and-run as a fallback.
static bool
try_eval_integer(tree_xform_t *ctx, const char *expr, const char *file,
                 int line, long long *out)
{
    char     *endptr;
    long long value;

    errno = 0;
    value = strtoll(expr, &endptr, 0);

    // Skip trailing whitespace
    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\n') {
        endptr++;
    }

    if (errno == 0 && *endptr == '\0' && endptr != expr) {
        *out = value;
        return true;
    }

    // Fallback: compile and run.
    compile_ctx_t *cctx     = (compile_ctx_t *)ctx->user_data;
    const char    *compiler = cctx ? cctx->compiler : nullptr;
    if (!compiler) {
        return false;
    }

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
    base_dealloc(err_msg);

    if (!result) {
        return false;
    }

    errno = 0;
    value = strtoll(result, &endptr, 10);
    bool ok = (errno == 0 && *endptr == '\0');
    if (ok) {
        *out = value;
    }
    base_dealloc(result);
    return ok;
}

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
    //   [2] = assignment_expression  (prefix)
    //   [3] = ","
    //   [4] = assignment_expression  (suffix)
    //   [5] = ")"

    // --- Extract prefix from kid[2] ---
    tnode_t *prefix_node = tnode_get_kid(node, 2);
    if (!prefix_node) {
        ncc_error("%s:%d: constexpr_paste: missing first argument\n",
                  file, line);
        exit(1);
    }

    char *prefix = emit_and_clean(ctx, prefix_node);
    if (!prefix || !*prefix) {
        ncc_error("%s:%d: constexpr_paste: cannot emit first argument\n",
                  file, line);
        exit(1);
    }

    if (!is_ident_fragment(prefix)) {
        ncc_error(
            "%s:%d: constexpr_paste: first argument '%s' is not a valid "
            "identifier fragment\n",
            file, line, prefix);
        exit(1);
    }

    // --- Extract suffix from kid[4] ---
    tnode_t *suffix_node = tnode_get_kid(node, 4);
    if (!suffix_node) {
        ncc_error("%s:%d: constexpr_paste: missing second argument\n",
                  file, line);
        exit(1);
    }

    char *suffix_text = emit_and_clean(ctx, suffix_node);
    if (!suffix_text || !*suffix_text) {
        ncc_error("%s:%d: constexpr_paste: cannot emit second argument\n",
                  file, line);
        exit(1);
    }

    // Build the concatenated identifier.
    char id_buf[NCC_IDENT_BUF];
    int  iret;

    // Try integer evaluation first (backward compat: constexpr_paste("item_", (0)))
    long long int_val;
    if (try_eval_integer(ctx, suffix_text, file, line, &int_val)) {
        iret = snprintf(id_buf, sizeof(id_buf), "%s%lld", prefix, int_val);
    } else if (is_ident_fragment(suffix_text)) {
        // Suffix is a bare identifier (e.g. from typeid())
        iret = snprintf(id_buf, sizeof(id_buf), "%s%s", prefix, suffix_text);
    } else {
        ncc_error(
            "%s:%d: constexpr_paste: second argument '%s' is neither a "
            "compile-time integer nor a valid identifier fragment\n",
            file, line, suffix_text);
        exit(1);
    }
    NCC_CHECK_SNPRINTF(iret, id_buf);

    base_dealloc(prefix);
    base_dealloc(suffix_text);

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
