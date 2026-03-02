// xform_constexpr.c — Compile-time evaluation transforms.
//
// Transforms constexpr_eval, constexpr_max, constexpr_min, constexpr_strcmp,
// constexpr_strlen pseudo-functions into integer literals by compiling and
// running a helper program at compile time.
//
//   constexpr_eval(sizeof(my_struct))      → 48LL
//   constexpr_max(sizeof(A), sizeof(B))    → 128LL
//   constexpr_min(sizeof(char), sizeof(int)) → 1LL
//   constexpr_strcmp("abc", "def")          → -1LL
//   constexpr_strlen("hello")              → 5LL
//
// Registered as pre-order on "postfix_expression".

#include "xform/xform_helpers.h"
#include "slay/pprint.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// ============================================================================
// Helpers: parse template
// ============================================================================

static n00b_parse_tree_t *
parse_template(n00b_grammar_t *g, const char *nt_name, const char *src)
{
    n00b_result_t(n00b_parse_tree_ptr_t) r =
        n00b_xform_parse_template(g, nt_name, src, NULL);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "xform_constexpr: template parse failed for '%s':\n  %s\n",
                nt_name, src);
        return NULL;
    }
    return n00b_result_get(r);
}

// ============================================================================
// compile_and_run — compile a C program, run it, return stdout
// ============================================================================

char *
compile_and_run(const char *compiler, const char *source, char **err_out)
{
    if (err_out) {
        *err_out = NULL;
    }

    // Create a private temp directory (0700) to prevent symlink attacks.
    char dir_template[] = "/tmp/ncc_ce_XXXXXX";
    if (!mkdtemp(dir_template)) {
        return NULL;
    }

    char src_path[sizeof(dir_template) + 16];
    char bin_path[sizeof(dir_template) + 16];
    snprintf(src_path, sizeof(src_path), "%s/src.c", dir_template);
    snprintf(bin_path, sizeof(bin_path), "%s/bin", dir_template);

    int src_fd = open(src_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (src_fd < 0) {
        rmdir(dir_template);
        return NULL;
    }

    size_t  src_len     = strlen(source);
    size_t  src_written = 0;

    while (src_written < src_len) {
        ssize_t n = write(src_fd, source + src_written, src_len - src_written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(src_fd);
            unlink(src_path);
            rmdir(dir_template);
            return NULL;
        }
        src_written += (size_t)n;
    }
    close(src_fd);

    // Pipe to capture compiler stderr.
    int err_pipe[2];
    if (pipe(err_pipe) < 0) {
        unlink(src_path);
        rmdir(dir_template);
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        unlink(src_path);
        rmdir(dir_template);
        return NULL;
    }

    if (pid == 0) {
        // Child: compile.
        close(err_pipe[0]);
        dup2(err_pipe[1], STDERR_FILENO);
        close(err_pipe[1]);
        execlp(compiler, compiler,
               "-x", "c", "-std=gnu23", "-w",
               "-o", bin_path, src_path,
               (char *)NULL);
        _exit(127);
    }

    close(err_pipe[1]);

    // Read compiler stderr.
    char  *compile_err = NULL;
    size_t err_size;
    FILE  *ef = open_memstream(&compile_err, &err_size);

    char    ebuf[256];
    ssize_t en;
    while ((en = read(err_pipe[0], ebuf, sizeof(ebuf))) > 0) {
        fwrite(ebuf, 1, (size_t)en, ef);
    }
    close(err_pipe[0]);
    fclose(ef);

    int status;
    waitpid(pid, &status, 0);
    unlink(src_path);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(bin_path);
        rmdir(dir_template);
        if (err_out && compile_err && *compile_err) {
            *err_out = compile_err;
        }
        else {
            free(compile_err);
        }
        return NULL;
    }
    free(compile_err);

    // Run the compiled binary, capture stdout.
    int pipe_fd[2];
    if (pipe(pipe_fd) < 0) {
        unlink(bin_path);
        rmdir(dir_template);
        return NULL;
    }

    pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        unlink(bin_path);
        rmdir(dir_template);
        return NULL;
    }

    if (pid == 0) {
        // Child: run binary.
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[1]);
        execl(bin_path, bin_path, (char *)NULL);
        _exit(127);
    }

    close(pipe_fd[1]);

    char  *result = NULL;
    size_t result_size;
    FILE  *f = open_memstream(&result, &result_size);

    char    buf[256];
    ssize_t n;
    while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, f);
    }
    close(pipe_fd[0]);
    fclose(f);

    waitpid(pid, &status, 0);
    unlink(bin_path);
    rmdir(dir_template);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(result);
        return NULL;
    }

    return result;
}

// ============================================================================
// pprint_subtree — emit a parse subtree as C source via n00b_pprint
// ============================================================================

char *
pprint_subtree(n00b_grammar_t *g, n00b_parse_tree_t *node)
{
    n00b_pprint_opts_t opts = {
        .line_width        = 200,
        .indent_size       = 4,
        .indent_style      = N00B_PPRINT_SPACES,
        .use_unicode_width = false,
        .out               = NULL,
        .newline           = "\n",
        .style             = NULL,
    };

    return n00b_pprint(g, node, opts);
}

// ============================================================================
// collect_arguments — extract args from argument_expression_list
// ============================================================================

n00b_parse_tree_t **
collect_arguments(n00b_parse_tree_t *arglist, int *nargs)
{
    *nargs = 0;

    if (!arglist) {
        return NULL;
    }

    // Walk the left-recursive argument_expression_list.
    // Shape: arg_list -> arg_list "," assignment_expression
    //    or: arg_list -> assignment_expression
    //    or: arg_list -> keyword_argument (treated like assignment_expression)
    int                 cap   = 8;
    n00b_parse_tree_t **stack = malloc(sizeof(n00b_parse_tree_t *) * (size_t)cap);
    int                 top   = 0;

    n00b_parse_tree_t *cur = arglist;

    for (;;) {
        size_t nc = n00b_tree_num_children(cur);

        if (nc >= 3) {
            n00b_parse_tree_t *child0 = n00b_tree_child(cur, 0);

            if (n00b_xform_nt_name_is(child0, "argument_expression_list")) {
                // Left-recursive: child[0] = nested arglist, child[1] = ",",
                //                 child[2] = assignment_expression
                n00b_parse_tree_t *right_arg = n00b_tree_child(cur, 2);
                if (top >= cap) {
                    cap *= 2;
                    stack = realloc(stack, sizeof(n00b_parse_tree_t *) * (size_t)cap);
                }
                stack[top++] = right_arg;
                cur = child0;
                continue;
            }
        }

        // Base case: single argument (assignment_expression or keyword_argument).
        if (nc >= 1) {
            n00b_parse_tree_t *arg = n00b_tree_child(cur, 0);
            if (top >= cap) {
                cap *= 2;
                stack = realloc(stack, sizeof(n00b_parse_tree_t *) * (size_t)cap);
            }
            stack[top++] = arg;
        }
        break;
    }

    // Reverse into result array (stack is in right-to-left order).
    n00b_parse_tree_t **args = malloc(sizeof(n00b_parse_tree_t *) * (size_t)top);
    for (int i = 0; i < top; i++) {
        args[i] = stack[top - 1 - i];
    }
    *nargs = top;
    free(stack);
    return args;
}

// ============================================================================
// collect_file_scope_declarations — walk parse tree for user type definitions
// ============================================================================

// Check whether a subtree contains a specific node (pointer comparison).
static bool
contains_node(n00b_parse_tree_t *root, n00b_parse_tree_t *target)
{
    if (!root || !target) {
        return false;
    }
    if (root == target) {
        return true;
    }
    if (n00b_tree_is_leaf(root)) {
        return false;
    }

    size_t nc = n00b_tree_num_children(root);
    for (size_t i = 0; i < nc; i++) {
        if (contains_node(n00b_tree_child(root, i), target)) {
            return true;
        }
    }
    return false;
}

// Check if the first leaf token of a node has the system_header flag.
static bool
node_in_system_header(n00b_parse_tree_t *node)
{
    if (!node) {
        return false;
    }
    if (n00b_tree_is_leaf(node)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);
        return tok && tok->system_header;
    }
    size_t nc = n00b_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *c = n00b_tree_child(node, i);
        if (n00b_tree_is_leaf(c)) {
            n00b_token_info_t *tok = n00b_tree_leaf_value(c);
            if (tok) {
                return tok->system_header;
            }
        }
        else {
            return node_in_system_header(c);
        }
    }
    return false;
}

// DFS search for a leaf with text matching `keyword` under node.
static bool
has_keyword_leaf(n00b_parse_tree_t *node, const char *keyword)
{
    if (!node) {
        return false;
    }
    if (n00b_tree_is_leaf(node)) {
        return n00b_xform_leaf_text_eq(node, keyword);
    }
    size_t nc = n00b_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (has_keyword_leaf(n00b_tree_child(node, i), keyword)) {
            return true;
        }
    }
    return false;
}

// Check if a declaration has a typedef keyword in its declaration_specifiers.
static bool
has_typedef_keyword(n00b_parse_tree_t *decl)
{
    size_t nc = n00b_tree_num_children(decl);
    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *c = n00b_tree_child(decl, i);
        if (n00b_xform_nt_name_is(c, "declaration_specifiers")) {
            return has_keyword_leaf(c, "typedef");
        }
    }
    return false;
}

// Check if a declaration contains a struct/union/enum definition (with body).
static bool
has_struct_body(n00b_parse_tree_t *node)
{
    if (!node) {
        return false;
    }
    if (n00b_tree_is_leaf(node)) {
        return false;
    }

    if (n00b_xform_nt_name_is(node, "struct_or_union_specifier")
        || n00b_xform_nt_name_is(node, "enum_specifier")) {
        // Check if it has a "{" child — meaning it's a definition, not just
        // a forward reference.
        size_t nc = n00b_tree_num_children(node);
        for (size_t i = 0; i < nc; i++) {
            n00b_parse_tree_t *c = n00b_tree_child(node, i);
            if (n00b_tree_is_leaf(c) && n00b_xform_leaf_text_eq(c, "{")) {
                return true;
            }
        }
        return false;
    }

    size_t nc = n00b_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (has_struct_body(n00b_tree_child(node, i))) {
            return true;
        }
    }
    return false;
}

// Is this an interesting declaration for constexpr? (typedef or struct def)
static bool
is_type_declaration(n00b_parse_tree_t *decl)
{
    return has_typedef_keyword(decl) || has_struct_body(decl);
}

// Check if a node is a group wrapper ($$group_*).
static bool
is_group_wrapper(n00b_parse_tree_t *node)
{
    if (!node || n00b_tree_is_leaf(node)) {
        return false;
    }
    n00b_nt_node_t pn = n00b_tree_node_value(node);
    return pn.name.data
        && pn.name.data[0] == '$' && pn.name.data[1] == '$';
}

// Recursively walk translation_unit children, flattening group wrappers,
// and collect type declarations (typedefs, struct/union/enum defs).
static void
collect_decls_recursive(n00b_parse_tree_t *node,
                        n00b_parse_tree_t *call_node,
                        n00b_grammar_t    *grammar,
                        FILE              *f)
{
    if (!node || n00b_tree_is_leaf(node)) {
        return;
    }

    // If this is a group wrapper, recurse into its children.
    if (is_group_wrapper(node)) {
        size_t nc = n00b_tree_num_children(node);
        for (size_t i = 0; i < nc; i++) {
            collect_decls_recursive(n00b_tree_child(node, i),
                                    call_node, grammar, f);
        }
        return;
    }

    // This should be an external_declaration. Find its declaration child.
    n00b_parse_tree_t *decl = n00b_xform_find_child_nt(node, "declaration");
    if (!decl) {
        return;
    }

    // Skip system header declarations.
    if (node_in_system_header(decl)) {
        return;
    }

    // Skip the declaration that contains the constexpr call.
    if (call_node && contains_node(node, call_node)) {
        return;
    }

    // Only emit type declarations (typedef, struct/union/enum defs).
    if (!is_type_declaration(decl)) {
        return;
    }

    char *text = pprint_subtree(grammar, node);
    if (text) {
        fputs(text, f);
        fputc('\n', f);
        free(text);
    }
}

char *
collect_file_scope_declarations(n00b_xform_ctx_t  *ctx,
                                n00b_parse_tree_t *call_node)
{
    n00b_parse_tree_t *root = ctx->root;
    if (!root) {
        return strdup("");
    }

    char  *output = NULL;
    size_t output_size;
    FILE  *f = open_memstream(&output, &output_size);

    size_t nc = n00b_tree_num_children(root);
    for (size_t i = 0; i < nc; i++) {
        collect_decls_recursive(n00b_tree_child(root, i),
                                call_node, ctx->grammar, f);
    }

    fclose(f);
    return output ? output : strdup("");
}

// ============================================================================
// strip_line_directives — safety filter for # NNN lines
// ============================================================================

static char *
strip_line_directives(const char *src)
{
    if (!src) {
        return strdup("");
    }

    char  *out = NULL;
    size_t out_size;
    FILE  *f = open_memstream(&out, &out_size);

    const char *p = src;
    while (*p) {
        if (*p == '#' && (p == src || p[-1] == '\n')) {
            const char *s = p + 1;
            while (*s == ' ' || *s == '\t') {
                s++;
            }
            if (isdigit((unsigned char)*s)) {
                // Skip the entire line.
                while (*p && *p != '\n') {
                    p++;
                }
                if (*p == '\n') {
                    p++;
                }
                continue;
            }
        }
        fputc(*p, f);
        p++;
    }

    fclose(f);
    return out ? out : strdup("");
}

// ============================================================================
// get_callee_name — extract the function name from a postfix_expression CALL
// ============================================================================

// Walk to the leftmost leaf of a subtree to find the callee token.
static const char *
get_first_leaf_text(n00b_parse_tree_t *node)
{
    if (!node) {
        return NULL;
    }
    if (n00b_tree_is_leaf(node)) {
        return n00b_xform_leaf_text(node);
    }
    size_t nc = n00b_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        const char *t = get_first_leaf_text(n00b_tree_child(node, i));
        if (t) {
            return t;
        }
    }
    return NULL;
}

// ============================================================================
// Main transform: constexpr_eval, constexpr_max, constexpr_min,
//                 constexpr_strcmp, constexpr_strlen
//
// Registered as pre-order on "postfix_expression".
// Matches the function call tree shape:
//   postfix_expression
//     ├── postfix_expression → ... → "constexpr_eval"
//     ├── "("
//     ├── argument_expression_list
//     └── ")"
// ============================================================================

static n00b_parse_tree_t *
xform_constexpr(n00b_xform_ctx_t  *ctx,
                n00b_parse_tree_t *node)
{

    // A function call has 4 children: callee, "(", arglist, ")".
    size_t nc = n00b_tree_num_children(node);
    if (nc < 3) {
        return NULL;
    }

    // Check child[1] is "(" — quick filter for CALL shape.
    n00b_parse_tree_t *child1 = n00b_tree_child(node, 1);
    if (!child1 || !n00b_xform_leaf_text_eq(child1, "(")) {
        return NULL;
    }

    // Extract callee name from child[0] (dig to first leaf).
    n00b_parse_tree_t *callee_node = n00b_tree_child(node, 0);
    const char        *callee      = get_first_leaf_text(callee_node);
    if (!callee) {
        return NULL;
    }

    enum {
        CE_NONE,
        CE_EVAL,
        CE_MAX,
        CE_MIN,
        CE_STRCMP,
        CE_STRLEN,
    } mode = CE_NONE;

    if (strcmp(callee, "constexpr_eval") == 0) {
        mode = CE_EVAL;
    }
    else if (strcmp(callee, "constexpr_max") == 0) {
        mode = CE_MAX;
    }
    else if (strcmp(callee, "constexpr_min") == 0) {
        mode = CE_MIN;
    }
    else if (strcmp(callee, "constexpr_strcmp") == 0) {
        mode = CE_STRCMP;
    }
    else if (strcmp(callee, "constexpr_strlen") == 0) {
        mode = CE_STRLEN;
    }

    if (mode == CE_NONE) {
        return NULL;
    }

    uint32_t line, col;
    n00b_xform_first_leaf_pos(node, &line, &col);

    // Extract compiler path and constexpr headers from user_data.
    typedef struct {
        const char *compiler;
        const char *constexpr_headers;
    } ncc_xform_data_t;

    ncc_xform_data_t *xdata    = (ncc_xform_data_t *)ctx->user_data;
    const char       *compiler = xdata ? xdata->compiler : NULL;
    if (!compiler) {
        fprintf(stderr,
                "ncc: %u:%u: constexpr: no compiler available\n",
                line, col);
        exit(1);
    }

    // Find the argument_expression_list (may be wrapped in a group node).
    n00b_parse_tree_t *arglist = n00b_xform_find_child_nt(
        node, "argument_expression_list");

    if (!arglist) {
        fprintf(stderr,
                "ncc: %u:%u: constexpr: no argument list found\n",
                line, col);
        exit(1);
    }

    // Collect arguments.
    int                 nargs = 0;
    n00b_parse_tree_t **args  = collect_arguments(arglist, &nargs);

    // Validate argument counts.
    if (mode == CE_EVAL || mode == CE_STRLEN) {
        if (nargs != 1) {
            fprintf(stderr,
                    "ncc: %u:%u: constexpr_%s expects 1 argument, got %d\n",
                    line, col,
                    mode == CE_EVAL ? "eval" : "strlen",
                    nargs);
            free(args);
            exit(1);
        }
    }
    else if (mode == CE_STRCMP) {
        if (nargs != 2) {
            fprintf(stderr,
                    "ncc: %u:%u: constexpr_strcmp expects 2 arguments, got %d\n",
                    line, col, nargs);
            free(args);
            exit(1);
        }
    }
    else {
        // max/min need >= 2.
        if (nargs < 2) {
            fprintf(stderr,
                    "ncc: %u:%u: constexpr_%s expects >= 2 arguments, got %d\n",
                    line, col,
                    mode == CE_MAX ? "max" : "min",
                    nargs);
            free(args);
            exit(1);
        }
    }

    // Build the program body.
    char  *body = NULL;
    size_t body_size;
    FILE  *body_f = open_memstream(&body, &body_size);
    fprintf(body_f, "int main(void) {\n");

    if (mode == CE_EVAL) {
        char *expr_str = pprint_subtree(ctx->grammar, args[0]);
        char *clean    = strip_line_directives(expr_str);
        fprintf(body_f,
                "    printf(\"%%lld\\n\", (long long)(%s));\n",
                clean);
        free(expr_str);
        free(clean);
    }
    else if (mode == CE_STRLEN) {
        char *expr_str = pprint_subtree(ctx->grammar, args[0]);
        char *clean    = strip_line_directives(expr_str);
        fprintf(body_f,
                "    printf(\"%%lld\\n\", (long long)strlen(%s));\n",
                clean);
        free(expr_str);
        free(clean);
    }
    else if (mode == CE_STRCMP) {
        char *e0 = pprint_subtree(ctx->grammar, args[0]);
        char *c0 = strip_line_directives(e0);
        char *e1 = pprint_subtree(ctx->grammar, args[1]);
        char *c1 = strip_line_directives(e1);
        fprintf(body_f,
                "    printf(\"%%lld\\n\", (long long)strcmp(%s, %s));\n",
                c0, c1);
        free(e0);
        free(c0);
        free(e1);
        free(c1);
    }
    else {
        // max or min.
        for (int i = 0; i < nargs; i++) {
            char *expr_str = pprint_subtree(ctx->grammar, args[i]);
            char *clean    = strip_line_directives(expr_str);
            fprintf(body_f,
                    "    long long _v%d = (long long)(%s);\n",
                    i, clean);
            free(expr_str);
            free(clean);
        }
        fprintf(body_f, "    long long _result = _v0;\n");
        const char *cmp = (mode == CE_MAX) ? ">" : "<";
        for (int i = 1; i < nargs; i++) {
            fprintf(body_f,
                    "    if (_v%d %s _result) _result = _v%d;\n",
                    i, cmp, i);
        }
        fprintf(body_f, "    printf(\"%%lld\\n\", _result);\n");
    }

    fprintf(body_f, "    return 0;\n}\n");
    fclose(body_f);

    // Build headers preamble.
    char  *headers = NULL;
    size_t headers_size;
    FILE  *hdr_f = open_memstream(&headers, &headers_size);

    fprintf(hdr_f, "#include <stdio.h>\n");
    fprintf(hdr_f, "#include <stdint.h>\n");
    fprintf(hdr_f, "#include <stddef.h>\n");
    fprintf(hdr_f, "#include <limits.h>\n");
    fprintf(hdr_f, "#include <stdalign.h>\n");
    fprintf(hdr_f, "#include <string.h>\n");
    fprintf(hdr_f, "#include <pthread.h>\n");

    // --ncc-constexpr-include flag takes precedence over env var.
    const char *extra = (xdata && xdata->constexpr_headers)
                            ? xdata->constexpr_headers
                            : getenv("NCC_CONSTEXPR_HEADERS");
    if (extra) {
        char *copy = strdup(extra);
        char *tok  = strtok(copy, ",");
        while (tok) {
            while (*tok == ' ') {
                tok++;
            }
            if (*tok) {
                size_t tlen = strlen(tok);
                while (tlen > 0 && tok[tlen - 1] == ' ') {
                    tok[--tlen] = '\0';
                }
                bool valid = false;
                if (tlen >= 3) {
                    char first = tok[0];
                    char last  = tok[tlen - 1];
                    if ((first == '<' && last == '>')
                        || (first == '"' && last == '"')) {
                        valid = true;
                        for (size_t i = 1; i < tlen - 1; i++) {
                            if (tok[i] == '\n' || tok[i] == '\r') {
                                valid = false;
                                break;
                            }
                        }
                    }
                }
                if (valid) {
                    fprintf(hdr_f, "#include %s\n", tok);
                }
                else {
                    fprintf(stderr,
                            "ncc: constexpr: invalid NCC_CONSTEXPR_HEADERS "
                            "token: '%s' (must be <...> or \"...\")\n",
                            tok);
                    exit(1);
                }
            }
            tok = strtok(NULL, ",");
        }
        free(copy);
    }

    fclose(hdr_f);

    // Two-try strategy: first without declarations, then with.
    char  *program = NULL;
    size_t prog_size;
    FILE  *prog_f = open_memstream(&program, &prog_size);
    fprintf(prog_f, "%s%s", headers, body);
    fclose(prog_f);

    char *compile_err = NULL;
    char *output      = compile_and_run(compiler, program, &compile_err);

    if (!output) {
        // Retry with file-scope declarations.
        free(program);
        free(compile_err);
        compile_err = NULL;

        char *raw_decls = collect_file_scope_declarations(ctx, node);
        char *decls     = strip_line_directives(raw_decls);
        free(raw_decls);

        if (decls && *decls) {
            prog_f = open_memstream(&program, &prog_size);
            fprintf(prog_f, "%s%s\n%s", headers, decls, body);
            fclose(prog_f);

            output = compile_and_run(compiler, program, &compile_err);
            free(program);
        }
        else {
            program = NULL;
        }

        free(decls);
    }
    else {
        free(program);
    }

    if (!output) {
        if (compile_err) {
            fprintf(stderr,
                    "ncc: %u:%u: constexpr: compilation failed:\n%s",
                    line, col, compile_err);
            free(compile_err);
        }
        else {
            fprintf(stderr,
                    "ncc: %u:%u: constexpr: compilation/execution failed\n",
                    line, col);
        }
        free(headers);
        free(body);
        free(args);
        exit(1);
    }

    free(headers);
    free(body);
    free(compile_err);

    // Parse the integer result.
    char     *endptr;
    long long value = strtoll(output, &endptr, 10);
    if (endptr == output) {
        fprintf(stderr,
                "ncc: %u:%u: constexpr: failed to parse result '%s'\n",
                line, col, output);
        free(output);
        free(args);
        exit(1);
    }
    free(output);
    free(args);

    // Format as literal with LL suffix. Negative values need parentheses
    // because `-1LL` is a unary expression, not a primary expression.
    char result_str[64];
    if (value < 0) {
        snprintf(result_str, sizeof(result_str), "(%lldLL)", value);
    }
    else {
        snprintf(result_str, sizeof(result_str), "%lldLL", value);
    }

    // Parse as primary_expression.
    n00b_parse_tree_t *replacement = parse_template(
        ctx->grammar, "primary_expression", result_str);

    if (!replacement) {
        fprintf(stderr,
                "ncc: %u:%u: constexpr: failed to parse literal '%s'\n",
                line, col, result_str);
        exit(1);
    }

    ctx->nodes_replaced++;
    return replacement;
}

// ============================================================================
// Registration
// ============================================================================

void
n00b_register_constexpr_xform(n00b_xform_registry_t *reg)
{
    n00b_xform_register(reg, "postfix_expression", xform_constexpr,
                        "constexpr");
}
