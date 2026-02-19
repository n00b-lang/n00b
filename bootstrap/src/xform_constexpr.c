/**
 * @file xform_constexpr.c
 * @brief Compile-time evaluation transforms: constexpr_eval, constexpr_max, constexpr_min.
 *
 * These transforms replace call-like expressions with integer literal results
 * computed at compile time by actually compiling and running the expression.
 *
 *   constexpr_eval(expr)          → integer literal (single expression)
 *   constexpr_max(e1, e2, ...)    → integer literal (max of 2+ expressions)
 *   constexpr_min(e1, e2, ...)    → integer literal (min of 2+ expressions)
 *
 * The expressions can be any valid C constant expression including sizeof(),
 * alignof(), arithmetic, bitwise ops, casts, etc.
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * @brief Extract the callee name from a CALL postfix_expression.
 *
 * For postfix_expression_1 (CALL):
 *   kid[0] = postfix_expression wrapping an identifier
 *   kid[1] = "("
 *   kid[2] = argument_expression_list (optional)
 *   kid[3] = ")"
 *
 * We drill into kid[0] to find the identifier token.
 */
/**
 * @brief Recursively find an identifier token in a subtree.
 *
 * Walks into primary_expression / identifier nodes to find the
 * terminal token. Only matches simple identifier paths (not member
 * access or subscript expressions).
 */
static tok_t *
find_identifier_tok(tnode_t *node)
{
    if (!node) {
        return nullptr;
    }
    if (node->tptr) {
        return node->tptr;
    }
    // Only walk into: postfix_expression(PRIMARY), primary_expression(identifier),
    // identifier, provided_identifier — to avoid matching method calls.
    if (node->nt_id == NT_postfix_expression
        && node->branch != BRANCH(postfix_expression, PRIMARY)) {
        return nullptr;
    }
    for (int i = 0; i < node->num_kids; i++) {
        tok_t *tok = find_identifier_tok(tnode_get_kid(node, i));
        if (tok) {
            return tok;
        }
    }
    return nullptr;
}

static char *
get_callee_name(tree_xform_t *ctx, tnode_t *node)
{
    tnode_t *callee = tnode_get_kid(node, 0);
    if (!callee) {
        return nullptr;
    }

    tok_t *tok = find_identifier_tok(callee);
    if (!tok) {
        return nullptr;
    }

    return extract(ctx->input, tok);
}

/**
 * @brief Emit a subtree to a dynamically allocated string.
 */
char *
emit_node_to_string(tree_xform_t *ctx, tnode_t *node)
{
    char  *output = nullptr;
    size_t size;
    FILE  *f = open_memstream(&output, &size);

    emit_ctx_t ectx;
    emit_init(&ectx, ctx->lex, f);
    emit_tree(&ectx, node);
    emit_finish(&ectx);

    fclose(f);
    return output;
}

/**
 * @brief Strip #line directives from emitted source text.
 *
 * The emitter inserts #line directives which would confuse the temporary
 * compilation. We remove them here.
 */
char *
strip_line_directives(const char *src)
{
    char  *out = nullptr;
    size_t size;
    FILE  *f = open_memstream(&out, &size);

    const char *p = src;
    while (*p) {
        // Check for #line or # <digit> at start of line
        if (*p == '#') {
            const char *q = p + 1;
            // Skip whitespace after #
            while (*q == ' ' || *q == '\t') {
                q++;
            }
            // Check for "line" keyword or digit
            bool is_line_directive = false;
            if (*q >= '0' && *q <= '9') {
                is_line_directive = true;
            }
            else if (strncmp(q, "line", 4) == 0
                     && (q[4] == ' ' || q[4] == '\t')) {
                is_line_directive = true;
            }

            if (is_line_directive) {
                // Skip entire line
                while (*p && *p != '\n') {
                    p++;
                }
                if (*p == '\n') {
                    p++;
                }
                continue;
            }
        }

        // Copy line normally
        while (*p && *p != '\n') {
            fputc(*p, f);
            p++;
        }
        if (*p == '\n') {
            fputc('\n', f);
            p++;
        }
    }

    fclose(f);
    return out;
}

/**
 * @brief Check if a tree contains a node with the given ID (iterative DFS).
 */
static bool
tree_contains_id(tnode_t *root, int target_id)
{
    if (!root) {
        return false;
    }
    if (root->id == target_id) {
        return true;
    }

    int       cap = NCC_CAP_MEDIUM;
    int       top = 0;
    tnode_t **stk = base_alloc(cap * sizeof(tnode_t *));
    if (!stk) {
        return false;
    }

    stk[top++] = root;

    while (top > 0) {
        tnode_t *n = stk[--top];
        for (int i = n->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (!kid) {
                continue;
            }
            if (kid->id == target_id) {
                base_dealloc(stk);
                return true;
            }
            if (top >= cap) {
                cap *= 2;
                stk = base_realloc(stk, cap * sizeof(tnode_t *));
            }
            stk[top++] = kid;
        }
    }

    base_dealloc(stk);
    return false;
}

/**
 * @brief Find the first terminal token in a subtree (iterative DFS).
 */
static tok_t *
find_first_token(tnode_t *node)
{
    if (!node) {
        return nullptr;
    }
    if (node->tptr) {
        return node->tptr;
    }

    int       cap = NCC_CAP_MEDIUM;
    int       top = 0;
    tnode_t **stk = base_alloc(cap * sizeof(tnode_t *));
    if (!stk) {
        return nullptr;
    }

    stk[top++] = node;

    while (top > 0) {
        tnode_t *n = stk[--top];
        for (int i = n->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (!kid) {
                continue;
            }
            if (kid->tptr) {
                base_dealloc(stk);
                return kid->tptr;
            }
            if (top >= cap) {
                cap *= 2;
                stk = base_realloc(stk, cap * sizeof(tnode_t *));
            }
            stk[top++] = kid;
        }
    }

    base_dealloc(stk);
    return nullptr;
}

/**
 * @brief Check if a node is in an ncc_off zone (system header).
 */
static bool
is_from_system_header(lex_t *lex, tnode_t *node)
{
    if (!lex || !node) {
        return false;
    }
    tok_t *tok = find_first_token(node);
    if (!tok) {
        return false;
    }
    return lex_tok_is_ncc_off(lex, tok);
}

/**
 * @brief Emit file-scope declarations from the symbol table.
 *
 * Walks the file scope's symbol chain and emits type definitions,
 * tag definitions, and const variable definitions that may be needed
 * for evaluating constexpr expressions.
 *
 * @param ctx        Transform context
 * @param call_node  The constexpr call node being transformed; any declaration
 *                   that is an ancestor of this node is skipped to avoid
 *                   emitting the still-untransformed containing declaration.
 */
char *
emit_declarations(tree_xform_t *ctx, tnode_t *call_node)
{
    if (!ctx->symtab) {
        return base_strdup("");
    }

    // Find the file scope (depth 0)
    scope_t *scope = ctx->symtab->current_scope;
    while (scope && scope->parent) {
        scope = scope->parent;
    }

    if (!scope || !scope->first_in_scope) {
        return base_strdup("");
    }

    // Collect symbols into an array so we can reverse them
    // (scope chain is in reverse insertion order)
    int           capacity = NCC_CAP_LARGE;
    int           count    = 0;
    sym_entry_t **syms     = base_alloc(capacity * sizeof(sym_entry_t *));
    if (!syms) {
        return base_strdup("");
    }

    for (sym_entry_t *sym = scope->first_in_scope; sym; sym = sym->next_in_scope) {
        if (count >= capacity) {
            capacity *= 2;
            syms = base_realloc(syms, capacity * sizeof(sym_entry_t *));
        }
        syms[count++] = sym;
    }

    // Emit in reverse order (source order)
    char  *output = nullptr;
    size_t size;
    FILE  *f = open_memstream(&output, &size);

    emit_ctx_t ectx;
    emit_init(&ectx, ctx->lex, f);

    for (int i = count - 1; i >= 0; i--) {
        sym_entry_t *sym  = syms[i];
        tnode_t     *node = sym->def.node;

        if (!node) {
            continue;
        }

        // Skip declarations from system headers — the generated program
        // includes system headers itself, so re-emitting these causes
        // redefinition errors.
        if (is_from_system_header(ctx->lex, node)) {
            continue;
        }

        // Skip declarations that contain the constexpr call we're
        // currently transforming — their parse tree still has the
        // untransformed constexpr_* call in it, which would fail
        // to compile in the temp program.
        if (call_node && tree_contains_id(node, call_node->id)) {
            continue;
        }

        switch (sym->kind) {
        case SYM_TYPEDEF:
        case SYM_TAG:
            emit_tree(&ectx, node);
            fprintf(f, "\n");
            break;
        case SYM_ENUM_CONST:
            // Part of an enum tag definition — already emitted
            break;
        case SYM_VARIABLE:
            // Only emit const variables (useful for constexpr)
            // Skip function definitions (they have compound_statement children)
            break;
        }
    }

    emit_finish(&ectx);
    fclose(f);
    base_dealloc(syms);

    // Strip #line directives
    char *clean = strip_line_directives(output);
    base_dealloc(output);
    return clean;
}

/**
 * @brief Compile and run a C program, returning stdout as a string.
 *
 * @param compiler   Path to the C compiler
 * @param source     Complete C source code to compile and run
 * @param err_out    If non-null and compilation fails, receives compiler stderr
 *                   with temp file paths stripped. Caller must free.
 * @return Dynamically allocated string with program output, or nullptr on failure
 */
char *
compile_and_run(const char *compiler, const char *source, char **err_out)
{
    if (err_out) {
        *err_out = nullptr;
    }

    // Create a private temp directory (0700) to prevent symlink attacks.
    // Both source and binary files are placed inside it.
    char dir_template[] = "/tmp/ncc_ce_XXXXXX";
    if (!mkdtemp(dir_template)) {
        return nullptr;
    }

    char src_path[sizeof(dir_template) + 8];
    char bin_path[sizeof(dir_template) + 8];
    snprintf(src_path, sizeof(src_path), "%s/src", dir_template);
    snprintf(bin_path, sizeof(bin_path), "%s/bin", dir_template);

    int src_fd = open(src_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (src_fd < 0) {
        rmdir(dir_template);
        return nullptr;
    }

    write(src_fd, source, strlen(source));
    close(src_fd);

    // Pipe to capture compiler stderr
    int err_pipe[2];
    if (pipe(err_pipe) < 0) {
        unlink(src_path);
        unlink(bin_path);
        rmdir(dir_template);
        return nullptr;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        unlink(src_path);
        unlink(bin_path);
        rmdir(dir_template);
        return nullptr;
    }

    if (pid == 0) {
        // Child: compile
        close(err_pipe[0]);
        dup2(err_pipe[1], STDERR_FILENO);
        close(err_pipe[1]);
        execlp(compiler,
               compiler,
               "-x",
               "c",
               "-std=c23",
               "-o",
               bin_path,
               src_path,
               (char *)nullptr);
        _exit(127);
    }

    close(err_pipe[1]);

    // Read compiler stderr
    char  *compile_err = nullptr;
    size_t err_size;
    FILE  *ef = open_memstream(&compile_err, &err_size);

    char    ebuf[256];
    ssize_t en;
    while ((en = read(err_pipe[0], ebuf, sizeof(ebuf))) > 0) {
        fwrite(ebuf, 1, en, ef);
    }
    close(err_pipe[0]);
    fclose(ef);

    int status;
    waitpid(pid, &status, 0);
    unlink(src_path);  // Source no longer needed after compilation

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(bin_path);
        rmdir(dir_template);
        if (err_out && compile_err && *compile_err) {
            *err_out = compile_err;
        }
        else {
            base_dealloc(compile_err);
        }
        return nullptr;
    }
    base_dealloc(compile_err);

    // Run the compiled binary, capture stdout
    int pipe_fd[2];
    if (pipe(pipe_fd) < 0) {
        unlink(bin_path);
        rmdir(dir_template);
        return nullptr;
    }

    pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        unlink(bin_path);
        rmdir(dir_template);
        return nullptr;
    }

    if (pid == 0) {
        // Child: run binary
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[1]);
        execl(bin_path, bin_path, (char *)nullptr);
        _exit(127);
    }

    close(pipe_fd[1]);

    // Read output
    char  *result = nullptr;
    size_t result_size;
    FILE  *f = open_memstream(&result, &result_size);

    char    buf[256];
    ssize_t n;
    while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, n, f);
    }
    close(pipe_fd[0]);
    fclose(f);

    waitpid(pid, &status, 0);
    unlink(bin_path);
    rmdir(dir_template);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        base_dealloc(result);
        return nullptr;
    }

    return result;
}

/**
 * @brief Build a numeric literal replacement node.
 *
 * Creates: postfix_expression_9(primary_expression_1(constant(TT_NUM)))
 */
static tnode_t *
build_numeric_literal(const char *value_str, int line)
{
    tnode_t *const_node = synth_nonterminal("constant");
    const_node->nt_id   = NT_constant;
    add_child(const_node, synth_terminal(value_str, TT_NUM, line));

    tnode_t *primary = synth_nonterminal("primary_expression_1");
    primary->nt_id   = NT_primary_expression;
    primary->branch  = 1;
    add_child(primary, const_node);

    tnode_t *postfix = synth_nonterminal("postfix_expression_9");
    postfix->nt_id   = NT_postfix_expression;
    postfix->branch  = BRANCH(postfix_expression, PRIMARY);
    add_child(postfix, primary);

    return postfix;
}

// ---------------------------------------------------------------------------
// Main Transform
// ---------------------------------------------------------------------------

static tnode_t *
xform_constexpr(tree_xform_t *ctx, tnode_t *node)
{
    if (node->branch != BRANCH(postfix_expression, CALL)) {
        return nullptr;
    }

    char *name = get_callee_name(ctx, node);
    if (!name) {
        return nullptr;
    }

    enum {
        CE_NONE,
        CE_EVAL,
        CE_MAX,
        CE_MIN,
        CE_STRCMP,
        CE_STRLEN,
    } mode = CE_NONE;

    if (strcmp(name, "constexpr_eval") == 0) {
        mode = CE_EVAL;
    }
    else if (strcmp(name, "constexpr_max") == 0) {
        mode = CE_MAX;
    }
    else if (strcmp(name, "constexpr_min") == 0) {
        mode = CE_MIN;
    }
    else if (strcmp(name, "constexpr_strcmp") == 0) {
        mode = CE_STRCMP;
    }
    else if (strcmp(name, "constexpr_strlen") == 0) {
        mode = CE_STRLEN;
    }
    base_dealloc(name);

    if (mode == CE_NONE) {
        return nullptr;
    }

    int         line = get_node_line(node);
    const char *file = ctx->lex ? ctx->lex->in_file : "<unknown>";

    // Get the compiler path from user_data (compile_ctx_t *)
    compile_ctx_t *cctx     = (compile_ctx_t *)ctx->user_data;
    const char    *compiler = cctx ? cctx->compiler : nullptr;
    if (!compiler) {
        ncc_error("%s:%d: constexpr: no compiler available\n", file, line);
        exit(1);
    }

    // Extract argument list
    // The CALL node structure is:
    //   kid[0] = callee (postfix_expression)
    //   kid[1] = "("
    //   kid[2] = argument_expression_list (or ")" if no args)
    //   kid[3] = ")" (if args present)
    tnode_t *arglist = nullptr;
    tnode_t *kid2    = tnode_get_kid(node, 2);

    if (kid2 && kid2->nt_id == NT_argument_expression_list) {
        arglist = kid2;
    }

    // Collect arguments — handles both flattened and unflattened trees.
    // Unflattened: argument_expression_list_0 has kid[0]=argument_expression_list,
    //   kid[1]=",", kid[2]=assignment_expression (left-recursive).
    // Unflattened: argument_expression_list_1 has kid[0]=assignment_expression.
    // Flattened: direct children are all assignment_expression nodes.
    int       arg_capacity = NCC_CAP_SMALL;
    int       nargs        = 0;
    tnode_t **args         = base_alloc(arg_capacity * sizeof(tnode_t *));
    if (!args) {
        ncc_error("%s:%d: constexpr: out of memory\n", file, line);
        exit(1);
    }

    // Recursive helper: collect assignment_expression nodes from arglist
    // Use a simple iterative approach: walk left-recursive spine
    if (arglist) {
        tnode_t  *cur       = arglist;
        // Iteratively unwrap left-recursive argument_expression_list_0 nodes
        // until we reach argument_expression_list_1 (a single expression)
        // Stack for unwinding the left-recursive list
        int       stack_cap = NCC_CAP_SMALL;
        int       stack_len = 0;
        tnode_t **stack     = base_alloc(stack_cap * sizeof(tnode_t *));
        if (!stack) {
            base_dealloc(args);
            ncc_error("%s:%d: constexpr: out of memory\n", file, line);
            exit(1);
        }

        while (cur && cur->nt_id == NT_argument_expression_list) {
            // Check if this is a flattened list (all kids are assignment_expression)
            bool is_flat = true;
            for (int i = 0; i < cur->num_kids; i++) {
                tnode_t *k = tnode_get_kid(cur, i);
                if (k && k->nt_id == NT_argument_expression_list) {
                    is_flat = false;
                    break;
                }
            }

            if (is_flat) {
                // Flattened or single-arg: all children are args
                for (int i = 0; i < cur->num_kids; i++) {
                    tnode_t *k = tnode_get_kid(cur, i);
                    if (k && k->nt_id == NT_assignment_expression) {
                        if (nargs >= arg_capacity) {
                            arg_capacity *= 2;
                            args = base_realloc(args, arg_capacity * sizeof(tnode_t *));
                        }
                        args[nargs++] = k;
                    }
                }
                break;
            }

            // Unflattened: kid[0] = nested argument_expression_list,
            //              kid[1] = ",", kid[2] = assignment_expression
            // Push kid[2] (rightmost arg) onto stack for later
            tnode_t *right_arg = tnode_get_kid(cur, 2);
            if (right_arg && right_arg->nt_id == NT_assignment_expression) {
                if (stack_len >= stack_cap) {
                    stack_cap *= 2;
                    stack = base_realloc(stack, stack_cap * sizeof(tnode_t *));
                }
                stack[stack_len++] = right_arg;
            }
            cur = tnode_get_kid(cur, 0);
        }

        // Pop stack (reverse order to get left-to-right)
        for (int i = stack_len - 1; i >= 0; i--) {
            if (nargs >= arg_capacity) {
                arg_capacity *= 2;
                args = base_realloc(args, arg_capacity * sizeof(tnode_t *));
            }
            args[nargs++] = stack[i];
        }
        base_dealloc(stack);
    }

    // Validate argument counts
    if (mode == CE_EVAL || mode == CE_STRLEN) {
        if (nargs != 1) {
            ncc_error("%s:%d: constexpr_%s expects exactly 1 argument, got %d\n",
                      file,
                      line,
                      mode == CE_EVAL ? "eval" : "strlen",
                      nargs);
            base_dealloc(args);
            exit(1);
        }
    }
    else if (mode == CE_STRCMP) {
        if (nargs != 2) {
            ncc_error("%s:%d: constexpr_strcmp expects exactly 2 arguments, got %d\n",
                      file,
                      line,
                      nargs);
            base_dealloc(args);
            exit(1);
        }
    }
    else {
        // max/min need at least 2
        if (nargs < 2) {
            ncc_error("%s:%d: constexpr_%s expects at least 2 arguments, got %d\n",
                      file,
                      line,
                      mode == CE_MAX ? "max" : "min",
                      nargs);
            base_dealloc(args);
            exit(1);
        }
    }

    // Collect declarations from symbol table, skipping the declaration
    // that contains this constexpr call (it still has the untransformed tree)
    char *decls = emit_declarations(ctx, node);

    // Build the program body (everything after headers+decls)
    char  *body = nullptr;
    size_t body_size;
    FILE  *body_f = open_memstream(&body, &body_size);
    fprintf(body_f, "int main(void) {\n");

    if (mode == CE_EVAL) {
        char *expr_str = emit_node_to_string(ctx, args[0]);
        char *clean    = strip_line_directives(expr_str);
        fprintf(body_f,
                "    printf(\"%%lld\\n\", (long long)(%s));\n",
                clean);
        base_dealloc(expr_str);
        base_dealloc(clean);
    }
    else if (mode == CE_STRLEN) {
        char *expr_str = emit_node_to_string(ctx, args[0]);
        char *clean    = strip_line_directives(expr_str);
        fprintf(body_f,
                "    printf(\"%%lld\\n\", (long long)strlen(%s));\n",
                clean);
        base_dealloc(expr_str);
        base_dealloc(clean);
    }
    else if (mode == CE_STRCMP) {
        char *expr_str0 = emit_node_to_string(ctx, args[0]);
        char *clean0    = strip_line_directives(expr_str0);
        char *expr_str1 = emit_node_to_string(ctx, args[1]);
        char *clean1    = strip_line_directives(expr_str1);
        fprintf(body_f,
                "    printf(\"%%lld\\n\", (long long)strcmp(%s, %s));\n",
                clean0,
                clean1);
        base_dealloc(expr_str0);
        base_dealloc(clean0);
        base_dealloc(expr_str1);
        base_dealloc(clean1);
    }
    else {
        // max or min: emit each arg as a variable, then compare
        for (int i = 0; i < nargs; i++) {
            char *expr_str = emit_node_to_string(ctx, args[i]);
            char *clean    = strip_line_directives(expr_str);
            fprintf(body_f,
                    "    long long _v%d = (long long)(%s);\n",
                    i,
                    clean);
            base_dealloc(expr_str);
            base_dealloc(clean);
        }
        fprintf(body_f, "    long long _result = _v0;\n");
        const char *cmp = (mode == CE_MAX) ? ">" : "<";
        for (int i = 1; i < nargs; i++) {
            fprintf(body_f,
                    "    if (_v%d %s _result) _result = _v%d;\n",
                    i,
                    cmp,
                    i);
        }
        fprintf(body_f, "    printf(\"%%lld\\n\", _result);\n");
    }

    fprintf(body_f, "    return 0;\n}\n");
    fclose(body_f);

    // Build headers preamble (shared by both attempts)
    char  *headers = nullptr;
    size_t headers_size;
    FILE  *hdr_f = open_memstream(&headers, &headers_size);

    fprintf(hdr_f, "#include <stdio.h>\n");
    fprintf(hdr_f, "#include <stdint.h>\n");
    fprintf(hdr_f, "#include <stddef.h>\n");
    fprintf(hdr_f, "#include <limits.h>\n");
    fprintf(hdr_f, "#include <stdalign.h>\n");
    fprintf(hdr_f, "#include <string.h>\n");
    fprintf(hdr_f, "#include <pthread.h>\n");

    // NCC_CONSTEXPR_HEADERS env var
    const char *extra = getenv("NCC_CONSTEXPR_HEADERS");
    if (extra) {
        char *copy = base_strdup(extra);
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
                    if ((first == '<' && last == '>') || (first == '"' && last == '"')) {
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
                    ncc_error("constexpr: invalid NCC_CONSTEXPR_HEADERS "
                              "token: '%s' (must be <...> or \"...\")\n",
                              tok);
                    exit(1);
                }
            }
            tok = strtok(nullptr, ",");
        }
        base_dealloc(copy);
    }

    // Command-line --ncc-constexpr-include headers
    if (cctx && cctx->argv) {
        for (int ci = 0; ci < cctx->argv->num_constexpr_headers; ci++) {
            fprintf(hdr_f, "#include %s\n", cctx->argv->constexpr_headers[ci]);
        }
    }

    fclose(hdr_f);

    // Try without file-scope declarations first (avoids incomplete type errors
    // when the constexpr expression only uses primitive/builtin types).
    char  *program = nullptr;
    size_t prog_size;
    FILE  *prog_f = open_memstream(&program, &prog_size);
    fprintf(prog_f, "%s%s", headers, body);
    fclose(prog_f);

    char *compile_err = nullptr;
    char *output      = compile_and_run(compiler, program, &compile_err);

    if (!output && decls && *decls) {
        // Retry with file-scope declarations (needed when constexpr
        // references user-defined types like typedefs or structs).
        base_dealloc(program);
        base_dealloc(compile_err);
        compile_err = nullptr;

        prog_f = open_memstream(&program, &prog_size);
        fprintf(prog_f, "%s%s\n%s", headers, decls, body);
        fclose(prog_f);

        output = compile_and_run(compiler, program, &compile_err);
    }

    base_dealloc(headers);
    base_dealloc(body);
    base_dealloc(program);
    base_dealloc(decls);

    if (!output) {
        if (compile_err) {
            ncc_error("%s:%d: constexpr: compilation failed:\n%s",
                      file,
                      line,
                      compile_err);
            base_dealloc(compile_err);
        }
        else {
            ncc_error("%s:%d: constexpr: compilation/execution failed\n",
                      file,
                      line);
        }
        exit(1);
    }
    base_dealloc(compile_err);

    // Parse the integer result
    char     *endptr;
    long long value = strtoll(output, &endptr, 10);
    if (endptr == output) {
        ncc_error("%s:%d: constexpr: failed to parse result '%s'\n",
                  file,
                  line,
                  output);
        base_dealloc(output);
        exit(1);
    }
    base_dealloc(output);

    // Format as literal with LL suffix
    char result_str[NCC_INTSTR_BUF];
    snprintf(result_str, sizeof(result_str), "%lldLL", value);

    tnode_t *replacement = build_numeric_literal(result_str, line);
    base_dealloc(args);
    replace_node(node, replacement, "constexpr");
    return replacement;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void
register_constexpr_xform(xform_registry_t *reg)
{
    xform_register_post(reg,
                        NT_postfix_expression,
                        xform_constexpr,
                        "constexpr");
}
