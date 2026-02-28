#include "logic/logic_dsl.h"
#include "n00b.h"
#include "core/alloc.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Token types
// ============================================================================

typedef enum {
    TOK_EOF,
    TOK_IDENT,     // lowercase identifier (constant)
    TOK_VAR,       // uppercase identifier (logic variable)
    TOK_INT,       // integer literal
    TOK_UNDERSCORE,// _
    TOK_LPAREN,    // (
    TOK_RPAREN,    // )
    TOK_COMMA,     // ,
    TOK_DOT,       // .
    TOK_DOTDOT,    // ..
    TOK_IMPLIES,   // :-
    TOK_QUERY,     // ?-
    TOK_LBRACE,    // {
    TOK_RBRACE,    // }
    TOK_PLUS,      // +
    TOK_MINUS,     // -
    TOK_STAR,      // *
    TOK_SLASH,     // /
    TOK_PERCENT,   // %
    TOK_EQ,        // ==
    TOK_NE,        // !=
    TOK_LT,        // <
    TOK_LE,        // <=
    TOK_GT,        // >
    TOK_GE,        // >=
    // Keywords
    TOK_KW_VAR,
    TOK_KW_IN,
    TOK_KW_NOT,
    TOK_KW_SOLVE,
    TOK_KW_ALL,
    TOK_KW_IS,
    TOK_KW_ALLDIFF,
} dsl_tok_kind_t;

typedef struct {
    dsl_tok_kind_t kind;
    int32_t        line;
    int32_t        col;
    const char    *start;
    int32_t        len;
    int64_t        int_val;  // For TOK_INT.
} dsl_token_t;

// ============================================================================
// Lexer
// ============================================================================

typedef struct {
    const char    *src;
    size_t         src_len;
    size_t         pos;
    int32_t        line;
    int32_t        col;
    dsl_token_t   *tokens;
    int32_t        tok_count;
    int32_t        tok_cap;
    n00b_string_t *error;
    int32_t        error_line;
    int32_t        error_col;
} dsl_lexer_t;

static void
lex_push(dsl_lexer_t *lex, dsl_token_t tok)
{
    if (lex->tok_count >= lex->tok_cap) {
        int32_t      new_cap = lex->tok_cap ? lex->tok_cap * 2 : 64;
        dsl_token_t *new_toks = n00b_alloc_array(dsl_token_t, new_cap);
        if (lex->tok_count > 0) {
            memcpy(new_toks, lex->tokens,
                   lex->tok_count * sizeof(dsl_token_t));
        }
        n00b_free(lex->tokens);
        lex->tokens  = new_toks;
        lex->tok_cap = new_cap;
    }
    lex->tokens[lex->tok_count++] = tok;
}

static char
lex_peek(dsl_lexer_t *lex)
{
    if (lex->pos >= lex->src_len) {
        return '\0';
    }
    return lex->src[lex->pos];
}

static char
lex_advance(dsl_lexer_t *lex)
{
    char c = lex->src[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    }
    else {
        lex->col++;
    }
    return c;
}

static void
lex_skip_whitespace_and_comments(dsl_lexer_t *lex)
{
    while (lex->pos < lex->src_len) {
        char c = lex_peek(lex);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lex_advance(lex);
            continue;
        }
        if (c == '%') {
            // Line comment.
            while (lex->pos < lex->src_len && lex_peek(lex) != '\n') {
                lex_advance(lex);
            }
            continue;
        }
        break;
    }
}

static bool
lex_tokenize(dsl_lexer_t *lex)
{
    while (lex->pos < lex->src_len) {
        lex_skip_whitespace_and_comments(lex);
        if (lex->pos >= lex->src_len) {
            break;
        }

        int32_t     start_line = lex->line;
        int32_t     start_col  = lex->col;
        const char *start      = &lex->src[lex->pos];
        char        c          = lex_peek(lex);

        // Integer literal (or negative).
        if (isdigit((unsigned char)c)
            || (c == '-' && lex->pos + 1 < lex->src_len
                && isdigit((unsigned char)lex->src[lex->pos + 1]))) {
            bool negative = false;
            if (c == '-') {
                negative = true;
                lex_advance(lex);
            }

            int64_t val = 0;
            while (lex->pos < lex->src_len
                   && isdigit((unsigned char)lex_peek(lex))) {
                val = val * 10 + (lex_peek(lex) - '0');
                lex_advance(lex);
            }

            if (negative) {
                val = -val;
            }

            dsl_token_t tok = {
                .kind    = TOK_INT,
                .line    = start_line,
                .col     = start_col,
                .start   = start,
                .len     = (int32_t)(&lex->src[lex->pos] - start),
                .int_val = val,
            };
            lex_push(lex, tok);
            continue;
        }

        // Identifiers and keywords.
        if (isalpha((unsigned char)c) || c == '_') {
            while (lex->pos < lex->src_len) {
                char ch = lex_peek(lex);
                if (isalnum((unsigned char)ch) || ch == '_') {
                    lex_advance(lex);
                }
                else {
                    break;
                }
            }

            int32_t len = (int32_t)(&lex->src[lex->pos] - start);

            // Determine kind.
            dsl_tok_kind_t kind;
            if (len == 1 && start[0] == '_') {
                kind = TOK_UNDERSCORE;
            }
            else if (isupper((unsigned char)start[0]) || start[0] == '_') {
                kind = TOK_VAR;
            }
            else {
                kind = TOK_IDENT;
            }

            // Check keywords.
            if (kind == TOK_IDENT) {
                if (len == 3 && memcmp(start, "var", 3) == 0) {
                    kind = TOK_KW_VAR;
                }
                else if (len == 2 && memcmp(start, "in", 2) == 0) {
                    kind = TOK_KW_IN;
                }
                else if (len == 3 && memcmp(start, "not", 3) == 0) {
                    kind = TOK_KW_NOT;
                }
                else if (len == 5 && memcmp(start, "solve", 5) == 0) {
                    kind = TOK_KW_SOLVE;
                }
                else if (len == 3 && memcmp(start, "all", 3) == 0) {
                    kind = TOK_KW_ALL;
                }
                else if (len == 2 && memcmp(start, "is", 2) == 0) {
                    kind = TOK_KW_IS;
                }
                else if (len == 7 && memcmp(start, "alldiff", 7) == 0) {
                    kind = TOK_KW_ALLDIFF;
                }
            }

            dsl_token_t tok = {
                .kind  = kind,
                .line  = start_line,
                .col   = start_col,
                .start = start,
                .len   = len,
            };
            lex_push(lex, tok);
            continue;
        }

        // Multi-char tokens.
        lex_advance(lex);

        switch (c) {
        case '(':
            lex_push(lex, (dsl_token_t){TOK_LPAREN, start_line, start_col,
                                          start, 1, 0});
            break;
        case ')':
            lex_push(lex, (dsl_token_t){TOK_RPAREN, start_line, start_col,
                                          start, 1, 0});
            break;
        case ',':
            lex_push(lex, (dsl_token_t){TOK_COMMA, start_line, start_col,
                                          start, 1, 0});
            break;
        case '{':
            lex_push(lex, (dsl_token_t){TOK_LBRACE, start_line, start_col,
                                          start, 1, 0});
            break;
        case '}':
            lex_push(lex, (dsl_token_t){TOK_RBRACE, start_line, start_col,
                                          start, 1, 0});
            break;
        case '+':
            lex_push(lex, (dsl_token_t){TOK_PLUS, start_line, start_col,
                                          start, 1, 0});
            break;
        case '*':
            lex_push(lex, (dsl_token_t){TOK_STAR, start_line, start_col,
                                          start, 1, 0});
            break;
        case '/':
            lex_push(lex, (dsl_token_t){TOK_SLASH, start_line, start_col,
                                          start, 1, 0});
            break;
        case '.':
            if (lex_peek(lex) == '.') {
                lex_advance(lex);
                lex_push(lex, (dsl_token_t){TOK_DOTDOT, start_line, start_col,
                                              start, 2, 0});
            }
            else {
                lex_push(lex, (dsl_token_t){TOK_DOT, start_line, start_col,
                                              start, 1, 0});
            }
            break;
        case ':':
            if (lex_peek(lex) == '-') {
                lex_advance(lex);
                lex_push(lex, (dsl_token_t){TOK_IMPLIES, start_line,
                                              start_col, start, 2, 0});
            }
            else {
                lex->error      = r"expected '-' after ':'";
                lex->error_line = start_line;
                lex->error_col  = start_col;
                return false;
            }
            break;
        case '?':
            if (lex_peek(lex) == '-') {
                lex_advance(lex);
                lex_push(lex, (dsl_token_t){TOK_QUERY, start_line, start_col,
                                              start, 2, 0});
            }
            else {
                lex->error      = r"expected '-' after '?'";
                lex->error_line = start_line;
                lex->error_col  = start_col;
                return false;
            }
            break;
        case '=':
            if (lex_peek(lex) == '=') {
                lex_advance(lex);
                lex_push(lex, (dsl_token_t){TOK_EQ, start_line, start_col,
                                              start, 2, 0});
            }
            else {
                lex->error      = r"expected '=' after '='";
                lex->error_line = start_line;
                lex->error_col  = start_col;
                return false;
            }
            break;
        case '!':
            if (lex_peek(lex) == '=') {
                lex_advance(lex);
                lex_push(lex, (dsl_token_t){TOK_NE, start_line, start_col,
                                              start, 2, 0});
            }
            else {
                lex->error      = r"expected '=' after '!'";
                lex->error_line = start_line;
                lex->error_col  = start_col;
                return false;
            }
            break;
        case '<':
            if (lex_peek(lex) == '=') {
                lex_advance(lex);
                lex_push(lex, (dsl_token_t){TOK_LE, start_line, start_col,
                                              start, 2, 0});
            }
            else {
                lex_push(lex, (dsl_token_t){TOK_LT, start_line, start_col,
                                              start, 1, 0});
            }
            break;
        case '>':
            if (lex_peek(lex) == '=') {
                lex_advance(lex);
                lex_push(lex, (dsl_token_t){TOK_GE, start_line, start_col,
                                              start, 2, 0});
            }
            else {
                lex_push(lex, (dsl_token_t){TOK_GT, start_line, start_col,
                                              start, 1, 0});
            }
            break;
        case '-':
            // Standalone minus (not negative number — that's handled above).
            lex_push(lex, (dsl_token_t){TOK_MINUS, start_line, start_col,
                                          start, 1, 0});
            break;
        default:
            lex->error      = r"unexpected character";
            lex->error_line = start_line;
            lex->error_col  = start_col;
            return false;
        }
    }

    // Add EOF token.
    lex_push(lex, (dsl_token_t){TOK_EOF, lex->line, lex->col, nullptr, 0, 0});
    return true;
}

// ============================================================================
// AST types
// ============================================================================

typedef enum {
    AST_FACT,        // atom_head
    AST_RULE,        // atom_head + body (list of goals)
    AST_VAR_DECL,    // name + domain
    AST_CVAR_DECL,   // atom_head + domain + body
    AST_CONSTRAINT,  // lhs_atom + cmp + rhs_atom [+ body]
    AST_QUERY,       // goal_list
    AST_SOLVE,       // solve (all?)
    AST_ATOM,        // functor + args
    AST_TERM_VAR,    // logic variable
    AST_TERM_CONST,  // constant
    AST_TERM_INT,    // integer
    AST_TERM_ANON,   // _ (anonymous)
    AST_GOAL_NOT,    // negated atom
    AST_GOAL_CMP,    // expr cmp expr
    AST_GOAL_IS,     // Var is expr
    AST_DOMAIN_RANGE,// lo..hi
    AST_DOMAIN_SET,  // {v1, v2, ...}
    AST_ALLDIFF,     // alldiff(x, y, z)
    AST_LINEAR,      // 2*X + 3*Y == 12
} dsl_ast_kind_t;

typedef struct dsl_ast dsl_ast_t;

struct dsl_ast {
    dsl_ast_kind_t   kind;
    int32_t          line;
    int32_t          col;

    // Atom/term data.
    n00b_string_t   *name;
    int64_t          int_val;
    dsl_tok_kind_t   cmp_op;   // For constraints/comparisons.

    // Children.
    dsl_ast_t      **children;
    int32_t          child_count;
    int32_t          child_cap;

    // Domain data.
    int64_t          dom_lo;
    int64_t          dom_hi;
    int64_t         *dom_vals;
    int32_t          dom_val_count;

    // Solve all flag.
    bool             solve_all;

    // Linear constraint data (AST_LINEAR).
    int64_t         *linear_coeffs;
    int32_t          linear_count;
    int64_t          linear_rhs;
};

// ============================================================================
// AST helpers
// ============================================================================

static dsl_ast_t *
ast_new(dsl_ast_kind_t kind, int32_t line, int32_t col)
{
    dsl_ast_t *node = n00b_alloc(dsl_ast_t);
    node->kind = kind;
    node->line = line;
    node->col  = col;
    return node;
}

static void
ast_add_child(dsl_ast_t *parent, dsl_ast_t *child)
{
    if (parent->child_count >= parent->child_cap) {
        int32_t     new_cap = parent->child_cap ? parent->child_cap * 2 : 4;
        dsl_ast_t **new_ch  = n00b_alloc_array(dsl_ast_t *, new_cap);
        if (parent->child_count > 0) {
            memcpy(new_ch, parent->children,
                   parent->child_count * sizeof(dsl_ast_t *));
        }
        n00b_free(parent->children);
        parent->children = new_ch;
        parent->child_cap = new_cap;
    }
    parent->children[parent->child_count++] = child;
}

static void
ast_free(dsl_ast_t *node)
{
    if (!node) {
        return;
    }
    for (int32_t i = 0; i < node->child_count; i++) {
        ast_free(node->children[i]);
    }
    n00b_free(node->children);
    n00b_free(node->dom_vals);
    n00b_free(node->linear_coeffs);
    n00b_free(node);
}

// ============================================================================
// Parser
// ============================================================================

typedef struct {
    dsl_token_t   *tokens;
    int32_t        tok_count;
    int32_t        pos;
    int32_t        anon_counter;
    n00b_string_t *error;
    int32_t        error_line;
    int32_t        error_col;
    dsl_ast_t    **stmts;
    int32_t        stmt_count;
    int32_t        stmt_cap;
} dsl_parser_t;

static dsl_token_t *
parser_peek(dsl_parser_t *p)
{
    return &p->tokens[p->pos];
}

static dsl_token_t *
parser_advance(dsl_parser_t *p)
{
    return &p->tokens[p->pos++];
}

static bool
parser_at(dsl_parser_t *p, dsl_tok_kind_t kind)
{
    return p->tokens[p->pos].kind == kind;
}

static bool
parser_match(dsl_parser_t *p, dsl_tok_kind_t kind)
{
    if (p->tokens[p->pos].kind == kind) {
        p->pos++;
        return true;
    }
    return false;
}

static bool
parser_expect(dsl_parser_t *p, dsl_tok_kind_t kind, n00b_string_t *what)
{
    if (p->tokens[p->pos].kind == kind) {
        p->pos++;
        return true;
    }
    p->error      = what;
    p->error_line = p->tokens[p->pos].line;
    p->error_col  = p->tokens[p->pos].col;
    return false;
}

static void
parser_add_stmt(dsl_parser_t *p, dsl_ast_t *stmt)
{
    if (p->stmt_count >= p->stmt_cap) {
        int32_t     new_cap = p->stmt_cap ? p->stmt_cap * 2 : 16;
        dsl_ast_t **new_s   = n00b_alloc_array(dsl_ast_t *, new_cap);
        if (p->stmt_count > 0) {
            memcpy(new_s, p->stmts,
                   p->stmt_count * sizeof(dsl_ast_t *));
        }
        n00b_free(p->stmts);
        p->stmts    = new_s;
        p->stmt_cap = new_cap;
    }
    p->stmts[p->stmt_count++] = stmt;
}

static n00b_string_t *
tok_to_str(dsl_token_t *tok)
{
    return n00b_string_from_raw(tok->start, tok->len);
}

static bool is_cmp_token(dsl_tok_kind_t k)
{
    return k == TOK_EQ || k == TOK_NE || k == TOK_LT
           || k == TOK_LE || k == TOK_GT || k == TOK_GE;
}

// Forward declarations.
static dsl_ast_t *parse_atom(dsl_parser_t *p);
static dsl_ast_t *parse_term(dsl_parser_t *p);
static dsl_ast_t *parse_goal(dsl_parser_t *p);
static dsl_ast_t *parse_domain(dsl_parser_t *p);
static bool parse_linear_stmt(dsl_parser_t *p);

static dsl_ast_t *
parse_term(dsl_parser_t *p)
{
    dsl_token_t *tok = parser_peek(p);

    switch (tok->kind) {
    case TOK_UNDERSCORE: {
        parser_advance(p);
        dsl_ast_t *node = ast_new(AST_TERM_ANON, tok->line, tok->col);
        char tmp[32];
        int len = snprintf(tmp, sizeof(tmp), "_anon_%d", p->anon_counter++);
        node->name = n00b_string_from_raw(tmp, len);
        return node;
    }
    case TOK_VAR: {
        parser_advance(p);
        dsl_ast_t *node = ast_new(AST_TERM_VAR, tok->line, tok->col);
        node->name = tok_to_str(tok);
        return node;
    }
    case TOK_IDENT: {
        // Could be an atom (if followed by '(') or a constant.
        if (p->pos + 1 < p->tok_count
            && p->tokens[p->pos + 1].kind == TOK_LPAREN) {
            return parse_atom(p);
        }
        parser_advance(p);
        dsl_ast_t *node = ast_new(AST_TERM_CONST, tok->line, tok->col);
        node->name = tok_to_str(tok);
        return node;
    }
    case TOK_INT: {
        parser_advance(p);
        dsl_ast_t *node = ast_new(AST_TERM_INT, tok->line, tok->col);
        node->int_val = tok->int_val;
        char tmp[32];
        int len = snprintf(tmp, sizeof(tmp), "%lld", (long long)tok->int_val);
        node->name = n00b_string_from_raw(tmp, len);
        return node;
    }
    default:
        p->error      = r"expected term";
        p->error_line = tok->line;
        p->error_col  = tok->col;
        return nullptr;
    }
}

static dsl_ast_t *
parse_atom(dsl_parser_t *p)
{
    dsl_token_t *name_tok = parser_advance(p);
    if (name_tok->kind != TOK_IDENT) {
        p->error      = r"expected atom name";
        p->error_line = name_tok->line;
        p->error_col  = name_tok->col;
        return nullptr;
    }

    dsl_ast_t *atom = ast_new(AST_ATOM, name_tok->line, name_tok->col);
    atom->name = tok_to_str(name_tok);

    if (!parser_expect(p, TOK_LPAREN, r"expected '('")) {
        ast_free(atom);
        return nullptr;
    }

    // Parse argument list.
    if (!parser_at(p, TOK_RPAREN)) {
        dsl_ast_t *arg = parse_term(p);
        if (!arg) {
            ast_free(atom);
            return nullptr;
        }
        ast_add_child(atom, arg);

        while (parser_match(p, TOK_COMMA)) {
            arg = parse_term(p);
            if (!arg) {
                ast_free(atom);
                return nullptr;
            }
            ast_add_child(atom, arg);
        }
    }

    if (!parser_expect(p, TOK_RPAREN, r"expected ')'")) {
        ast_free(atom);
        return nullptr;
    }

    return atom;
}

static dsl_ast_t *
parse_goal(dsl_parser_t *p)
{
    // Negation.
    if (parser_at(p, TOK_KW_NOT)) {
        dsl_token_t *not_tok = parser_advance(p);
        dsl_ast_t   *inner   = parse_atom(p);
        if (!inner) {
            return nullptr;
        }
        dsl_ast_t *node = ast_new(AST_GOAL_NOT, not_tok->line, not_tok->col);
        ast_add_child(node, inner);
        return node;
    }

    // Is-expression: Var is expr.
    // We detect this as: VAR followed by 'is'.
    if (parser_at(p, TOK_VAR) && p->pos + 1 < p->tok_count
        && p->tokens[p->pos + 1].kind == TOK_KW_IS) {
        dsl_token_t *var_tok = parser_advance(p);
        parser_advance(p); // consume 'is'
        dsl_ast_t *var_node = ast_new(AST_TERM_VAR, var_tok->line,
                                       var_tok->col);
        var_node->name = tok_to_str(var_tok);

        // Parse the RHS expression as a term for now.
        // A full expression parser is deferred — we support simple
        // arithmetic like "PY - PX" as: term op term.
        dsl_ast_t *rhs = parse_term(p);
        if (!rhs) {
            ast_free(var_node);
            return nullptr;
        }

        dsl_ast_t *is_node = ast_new(AST_GOAL_IS, var_tok->line,
                                      var_tok->col);
        ast_add_child(is_node, var_node);
        ast_add_child(is_node, rhs);

        // Check for binary arithmetic operator.
        dsl_tok_kind_t op_kind = parser_peek(p)->kind;
        if (op_kind == TOK_PLUS || op_kind == TOK_MINUS
            || op_kind == TOK_STAR || op_kind == TOK_SLASH
            || op_kind == TOK_PERCENT) {
            is_node->cmp_op = op_kind;
            parser_advance(p);
            dsl_ast_t *rhs2 = parse_term(p);
            if (!rhs2) {
                ast_free(is_node);
                return nullptr;
            }
            ast_add_child(is_node, rhs2);
        }

        return is_node;
    }

    // Atom or comparison. Parse the first term/atom.
    dsl_ast_t *lhs = nullptr;

    if ((parser_at(p, TOK_IDENT) || parser_at(p, TOK_VAR)
         || parser_at(p, TOK_INT) || parser_at(p, TOK_UNDERSCORE))) {
        // If it's an atom (ident followed by '('), parse as atom.
        if (parser_at(p, TOK_IDENT)
            && p->pos + 1 < p->tok_count
            && p->tokens[p->pos + 1].kind == TOK_LPAREN) {
            lhs = parse_atom(p);
        }
        else {
            lhs = parse_term(p);
        }
    }
    else {
        p->error      = r"expected goal";
        p->error_line = parser_peek(p)->line;
        p->error_col  = parser_peek(p)->col;
        return nullptr;
    }

    if (!lhs) {
        return nullptr;
    }

    // Check for comparison operator.
    if (is_cmp_token(parser_peek(p)->kind)) {
        dsl_token_t *op = parser_advance(p);
        dsl_ast_t   *rhs;

        if (parser_at(p, TOK_IDENT)
            && p->pos + 1 < p->tok_count
            && p->tokens[p->pos + 1].kind == TOK_LPAREN) {
            rhs = parse_atom(p);
        }
        else {
            rhs = parse_term(p);
        }

        if (!rhs) {
            ast_free(lhs);
            return nullptr;
        }

        dsl_ast_t *cmp = ast_new(AST_GOAL_CMP, op->line, op->col);
        cmp->cmp_op = op->kind;
        ast_add_child(cmp, lhs);
        ast_add_child(cmp, rhs);
        return cmp;
    }

    // Plain atom as goal.
    return lhs;
}

static dsl_ast_t *
parse_domain(dsl_parser_t *p)
{
    dsl_token_t *tok = parser_peek(p);

    if (tok->kind == TOK_LBRACE) {
        // Set domain: {v1, v2, ...}
        parser_advance(p);
        dsl_ast_t *dom = ast_new(AST_DOMAIN_SET, tok->line, tok->col);

        // Collect values.
        int64_t *vals     = nullptr;
        int32_t  val_count = 0;
        int32_t  val_cap   = 0;

        if (!parser_at(p, TOK_RBRACE)) {
            if (!parser_at(p, TOK_INT)) {
                p->error      = r"expected integer in set";
                p->error_line = parser_peek(p)->line;
                p->error_col  = parser_peek(p)->col;
                ast_free(dom);
                return nullptr;
            }
            dsl_token_t *v = parser_advance(p);

            if (val_count >= val_cap) {
                int32_t  new_cap = val_cap ? val_cap * 2 : 8;
                int64_t *nv      = n00b_alloc_array(int64_t, new_cap);
                if (val_count > 0) {
                    memcpy(nv, vals, val_count * sizeof(int64_t));
                }
                n00b_free(vals);
                vals    = nv;
                val_cap = new_cap;
            }
            vals[val_count++] = v->int_val;

            while (parser_match(p, TOK_COMMA)) {
                if (!parser_at(p, TOK_INT)) {
                    p->error      = r"expected integer in set";
                    p->error_line = parser_peek(p)->line;
                    p->error_col  = parser_peek(p)->col;
                    n00b_free(vals);
                    ast_free(dom);
                    return nullptr;
                }
                v = parser_advance(p);
                if (val_count >= val_cap) {
                    int32_t  new_cap = val_cap * 2;
                    int64_t *nv      = n00b_alloc_array(int64_t, new_cap);
                    memcpy(nv, vals, val_count * sizeof(int64_t));
                    n00b_free(vals);
                    vals    = nv;
                    val_cap = new_cap;
                }
                vals[val_count++] = v->int_val;
            }
        }

        if (!parser_expect(p, TOK_RBRACE, r"expected '}'")) {
            n00b_free(vals);
            ast_free(dom);
            return nullptr;
        }

        dom->dom_vals      = vals;
        dom->dom_val_count = val_count;
        return dom;
    }

    // Range domain: INT..INT
    if (tok->kind == TOK_INT) {
        dsl_token_t *lo = parser_advance(p);
        if (!parser_expect(p, TOK_DOTDOT, r"expected '..'")) {
            return nullptr;
        }
        if (!parser_at(p, TOK_INT)) {
            p->error      = r"expected integer after '..'";
            p->error_line = parser_peek(p)->line;
            p->error_col  = parser_peek(p)->col;
            return nullptr;
        }
        dsl_token_t *hi  = parser_advance(p);
        dsl_ast_t   *dom = ast_new(AST_DOMAIN_RANGE, lo->line, lo->col);
        dom->dom_lo      = lo->int_val;
        dom->dom_hi      = hi->int_val;
        return dom;
    }

    p->error      = r"expected domain (range or set)";
    p->error_line = tok->line;
    p->error_col  = tok->col;
    return nullptr;
}

static bool
parse_body(dsl_parser_t *p, dsl_ast_t *parent)
{
    dsl_ast_t *goal = parse_goal(p);
    if (!goal) {
        return false;
    }
    ast_add_child(parent, goal);

    while (parser_match(p, TOK_COMMA)) {
        goal = parse_goal(p);
        if (!goal) {
            return false;
        }
        ast_add_child(parent, goal);
    }
    return true;
}

// Parse: [INT*] (VAR|IDENT) {(+|-) [INT*] (VAR|IDENT)}* == INT .
// Returns true on success (statement added), false on failure (caller backtracks).
static bool
parse_linear_stmt(dsl_parser_t *p)
{
    int32_t start_pos = p->pos;

    // Collect terms: arrays for coefficients and variable term AST nodes.
    int64_t    coeffs[32];
    dsl_ast_t *term_nodes[32];
    int32_t    term_count = 0;

    // Parse first term.
    int64_t sign = 1;
    for (;;) {
        if (term_count >= 32) {
            p->pos = start_pos;
            return false;
        }

        int64_t coeff = 1;
        dsl_token_t *t = parser_peek(p);

        // Optional coefficient: INT *
        if (t->kind == TOK_INT
            && p->pos + 1 < p->tok_count
            && p->tokens[p->pos + 1].kind == TOK_STAR) {
            coeff = t->int_val;
            parser_advance(p); // INT
            parser_advance(p); // *
            t = parser_peek(p);
        }

        // Must be VAR or IDENT (not followed by '(' — that's an atom).
        if (t->kind != TOK_VAR && t->kind != TOK_IDENT) {
            p->pos = start_pos;
            return false;
        }
        if (p->pos + 1 < p->tok_count
            && p->tokens[p->pos + 1].kind == TOK_LPAREN) {
            p->pos = start_pos;
            return false;
        }

        dsl_ast_t *var_node = parse_term(p);
        if (!var_node) {
            p->pos = start_pos;
            return false;
        }

        coeffs[term_count]     = sign * coeff;
        term_nodes[term_count] = var_node;
        term_count++;

        // Check for + or -.
        dsl_tok_kind_t nk = parser_peek(p)->kind;
        if (nk == TOK_PLUS) {
            parser_advance(p);
            sign = 1;
        }
        else if (nk == TOK_MINUS) {
            parser_advance(p);
            sign = -1;
        }
        else {
            break;
        }
    }

    // Expect ==.
    if (!parser_at(p, TOK_EQ)) {
        // Not a linear statement — backtrack.
        for (int32_t i = 0; i < term_count; i++) {
            ast_free(term_nodes[i]);
        }
        p->pos = start_pos;
        return false;
    }
    parser_advance(p); // consume ==

    // Expect INT rhs.
    if (!parser_at(p, TOK_INT)) {
        for (int32_t i = 0; i < term_count; i++) {
            ast_free(term_nodes[i]);
        }
        p->pos = start_pos;
        return false;
    }
    int64_t rhs = parser_advance(p)->int_val;

    // Expect '.'.
    if (!parser_at(p, TOK_DOT)) {
        for (int32_t i = 0; i < term_count; i++) {
            ast_free(term_nodes[i]);
        }
        p->pos = start_pos;
        return false;
    }
    parser_advance(p);

    // Build AST_LINEAR node.
    dsl_ast_t *node = ast_new(AST_LINEAR, p->tokens[start_pos].line,
                               p->tokens[start_pos].col);
    node->linear_rhs   = rhs;
    node->linear_count = term_count;
    node->linear_coeffs = n00b_alloc_array(int64_t, term_count);
    for (int32_t i = 0; i < term_count; i++) {
        node->linear_coeffs[i] = coeffs[i];
        ast_add_child(node, term_nodes[i]);
    }

    parser_add_stmt(p, node);
    return true;
}

static bool
parse_statement(dsl_parser_t *p)
{
    dsl_token_t *tok = parser_peek(p);

    if (tok->kind == TOK_EOF) {
        return false;
    }

    // Query: ?- goal_list .
    if (tok->kind == TOK_QUERY) {
        parser_advance(p);
        dsl_ast_t *query = ast_new(AST_QUERY, tok->line, tok->col);
        if (!parse_body(p, query)) {
            ast_free(query);
            return false;
        }
        if (!parser_expect(p, TOK_DOT, r"expected '.' after query")) {
            ast_free(query);
            return false;
        }
        parser_add_stmt(p, query);
        return true;
    }

    // Solve: solve [all] .
    if (tok->kind == TOK_KW_SOLVE) {
        parser_advance(p);
        dsl_ast_t *solve = ast_new(AST_SOLVE, tok->line, tok->col);
        if (parser_match(p, TOK_KW_ALL)) {
            solve->solve_all = true;
        }
        if (!parser_expect(p, TOK_DOT, r"expected '.' after solve")) {
            ast_free(solve);
            return false;
        }
        parser_add_stmt(p, solve);
        return true;
    }

    // Var decl: var IDENT in domain .
    if (tok->kind == TOK_KW_VAR) {
        parser_advance(p);
        dsl_token_t *name = parser_peek(p);
        if (name->kind != TOK_IDENT && name->kind != TOK_VAR) {
            p->error      = r"expected variable name after 'var'";
            p->error_line = name->line;
            p->error_col  = name->col;
            return false;
        }
        parser_advance(p);

        if (!parser_expect(p, TOK_KW_IN, r"expected 'in'")) {
            return false;
        }

        dsl_ast_t *dom = parse_domain(p);
        if (!dom) {
            return false;
        }

        if (!parser_expect(p, TOK_DOT, r"expected '.'")) {
            ast_free(dom);
            return false;
        }

        dsl_ast_t *decl = ast_new(AST_VAR_DECL, tok->line, tok->col);
        decl->name = tok_to_str(name);
        ast_add_child(decl, dom);
        parser_add_stmt(p, decl);
        return true;
    }

    // alldiff(x, y, z).
    if (tok->kind == TOK_KW_ALLDIFF) {
        parser_advance(p);
        if (!parser_expect(p, TOK_LPAREN, r"expected '(' after alldiff")) {
            return false;
        }

        dsl_ast_t *node = ast_new(AST_ALLDIFF, tok->line, tok->col);

        if (!parser_at(p, TOK_RPAREN)) {
            dsl_ast_t *arg = parse_term(p);
            if (!arg) {
                ast_free(node);
                return false;
            }
            ast_add_child(node, arg);

            while (parser_match(p, TOK_COMMA)) {
                arg = parse_term(p);
                if (!arg) {
                    ast_free(node);
                    return false;
                }
                ast_add_child(node, arg);
            }
        }

        if (!parser_expect(p, TOK_RPAREN, r"expected ')'")) {
            ast_free(node);
            return false;
        }
        if (!parser_expect(p, TOK_DOT, r"expected '.' after alldiff")) {
            ast_free(node);
            return false;
        }

        parser_add_stmt(p, node);
        return true;
    }

    // Fact, rule, constraint, or conditional var decl.
    // All start with an atom: ident(args...)
    // Also handle standalone constraints like: meeting_a < meeting_b.
    if (tok->kind == TOK_IDENT
        && p->pos + 1 < p->tok_count
        && p->tokens[p->pos + 1].kind == TOK_LPAREN) {
        // Parse atom head.
        dsl_ast_t *head = parse_atom(p);
        if (!head) {
            return false;
        }

        dsl_tok_kind_t next = parser_peek(p)->kind;

        // Fact: atom .
        if (next == TOK_DOT) {
            parser_advance(p);
            dsl_ast_t *fact = ast_new(AST_FACT, head->line, head->col);
            ast_add_child(fact, head);
            parser_add_stmt(p, fact);
            return true;
        }

        // Rule: atom :- body .
        if (next == TOK_IMPLIES) {
            parser_advance(p);
            dsl_ast_t *rule = ast_new(AST_RULE, head->line, head->col);
            ast_add_child(rule, head);
            if (!parse_body(p, rule)) {
                ast_free(rule);
                return false;
            }
            if (!parser_expect(p, TOK_DOT, r"expected '.' after rule")) {
                ast_free(rule);
                return false;
            }
            parser_add_stmt(p, rule);
            return true;
        }

        // Conditional var decl: atom in domain :- body .
        if (next == TOK_KW_IN) {
            parser_advance(p);
            dsl_ast_t *dom = parse_domain(p);
            if (!dom) {
                ast_free(head);
                return false;
            }

            dsl_ast_t *decl = ast_new(AST_CVAR_DECL, head->line, head->col);
            ast_add_child(decl, head); // child 0: atom
            ast_add_child(decl, dom);  // child 1: domain

            if (parser_match(p, TOK_IMPLIES)) {
                if (!parse_body(p, decl)) {
                    ast_free(decl);
                    return false;
                }
            }

            if (!parser_expect(p, TOK_DOT, r"expected '.'")) {
                ast_free(decl);
                return false;
            }

            parser_add_stmt(p, decl);
            return true;
        }

        // Constraint: atom CMP atom [!= atom]* [. | :- body .]
        // Chained != desugars to alldiff.
        if (is_cmp_token(next)) {
            dsl_token_t *op = parser_advance(p);

            dsl_ast_t *rhs;
            if (parser_at(p, TOK_IDENT)
                && p->pos + 1 < p->tok_count
                && p->tokens[p->pos + 1].kind == TOK_LPAREN) {
                rhs = parse_atom(p);
            }
            else {
                rhs = parse_term(p);
            }

            if (!rhs) {
                ast_free(head);
                return false;
            }

            // Check for chained !=: a != b != c => alldiff(a, b, c)
            if (op->kind == TOK_NE && parser_at(p, TOK_NE)) {
                dsl_ast_t *ad = ast_new(AST_ALLDIFF, head->line, head->col);
                ast_add_child(ad, head);
                ast_add_child(ad, rhs);

                while (parser_at(p, TOK_NE)) {
                    parser_advance(p);
                    dsl_ast_t *next_op;
                    if (parser_at(p, TOK_IDENT)
                        && p->pos + 1 < p->tok_count
                        && p->tokens[p->pos + 1].kind == TOK_LPAREN) {
                        next_op = parse_atom(p);
                    }
                    else {
                        next_op = parse_term(p);
                    }
                    if (!next_op) {
                        ast_free(ad);
                        return false;
                    }
                    ast_add_child(ad, next_op);
                }

                if (parser_match(p, TOK_IMPLIES)) {
                    // Body is stored as additional children; executor
                    // will handle it same as constraint bodies.
                    // For alldiff, we store body atoms starting at
                    // child_count (after the variable children).
                    // Mark the boundary.
                    ad->linear_count = ad->child_count; // reuse field as var count
                    if (!parse_body(p, ad)) {
                        ast_free(ad);
                        return false;
                    }
                }

                if (!parser_expect(p, TOK_DOT, r"expected '.'")) {
                    ast_free(ad);
                    return false;
                }

                parser_add_stmt(p, ad);
                return true;
            }

            dsl_ast_t *con = ast_new(AST_CONSTRAINT, head->line, head->col);
            con->cmp_op = op->kind;
            ast_add_child(con, head);
            ast_add_child(con, rhs);

            if (parser_match(p, TOK_IMPLIES)) {
                if (!parse_body(p, con)) {
                    ast_free(con);
                    return false;
                }
            }

            if (!parser_expect(p, TOK_DOT, r"expected '.'")) {
                ast_free(con);
                return false;
            }

            parser_add_stmt(p, con);
            return true;
        }

        p->error      = r"expected '.', ':-', 'in', or comparison after atom";
        p->error_line = parser_peek(p)->line;
        p->error_col  = parser_peek(p)->col;
        ast_free(head);
        return false;
    }

    // Linear constraint: [INT*] (VAR|IDENT) {(+|-) [INT*] (VAR|IDENT)}* == INT .
    // Try to parse with backtracking; fall through on failure.
    if (tok->kind == TOK_INT || tok->kind == TOK_VAR || tok->kind == TOK_IDENT) {
        int32_t       save_pos      = p->pos;
        n00b_string_t *save_err      = p->error;
        int32_t       save_err_line = p->error_line;
        int32_t       save_err_col  = p->error_col;

        if (parse_linear_stmt(p)) {
            return true;
        }

        // Backtrack on failure.
        p->pos = save_pos;
        p->error = save_err;
        p->error_line = save_err_line;
        p->error_col = save_err_col;
    }

    // Standalone identifiers (not atoms): e.g., "meeting_a < meeting_b."
    // Also handles chained != desugaring to alldiff.
    if (tok->kind == TOK_IDENT || tok->kind == TOK_VAR || tok->kind == TOK_INT) {
        dsl_ast_t *lhs = parse_term(p);
        if (!lhs) {
            return false;
        }

        if (is_cmp_token(parser_peek(p)->kind)) {
            dsl_token_t *op = parser_advance(p);
            dsl_ast_t   *rhs;
            if (parser_at(p, TOK_IDENT)
                && p->pos + 1 < p->tok_count
                && p->tokens[p->pos + 1].kind == TOK_LPAREN) {
                rhs = parse_atom(p);
            }
            else {
                rhs = parse_term(p);
            }

            if (!rhs) {
                ast_free(lhs);
                return false;
            }

            // Chained != desugars to alldiff: x != y != z.
            if (op->kind == TOK_NE && parser_at(p, TOK_NE)) {
                dsl_ast_t *ad = ast_new(AST_ALLDIFF, lhs->line, lhs->col);
                ast_add_child(ad, lhs);
                ast_add_child(ad, rhs);

                while (parser_at(p, TOK_NE)) {
                    parser_advance(p);
                    dsl_ast_t *next_op = parse_term(p);
                    if (!next_op) {
                        ast_free(ad);
                        return false;
                    }
                    ast_add_child(ad, next_op);
                }

                if (!parser_expect(p, TOK_DOT, r"expected '.'")) {
                    ast_free(ad);
                    return false;
                }

                parser_add_stmt(p, ad);
                return true;
            }

            dsl_ast_t *con = ast_new(AST_CONSTRAINT, lhs->line, lhs->col);
            con->cmp_op = op->kind;
            ast_add_child(con, lhs);
            ast_add_child(con, rhs);

            if (parser_match(p, TOK_IMPLIES)) {
                if (!parse_body(p, con)) {
                    ast_free(con);
                    return false;
                }
            }

            if (!parser_expect(p, TOK_DOT, r"expected '.'")) {
                ast_free(con);
                return false;
            }

            parser_add_stmt(p, con);
            return true;
        }

        p->error      = r"expected comparison operator";
        p->error_line = parser_peek(p)->line;
        p->error_col  = parser_peek(p)->col;
        ast_free(lhs);
        return false;
    }

    p->error      = r"unexpected token";
    p->error_line = tok->line;
    p->error_col  = tok->col;
    return false;
}

static bool
parse_program(dsl_parser_t *p)
{
    while (!parser_at(p, TOK_EOF)) {
        if (!parse_statement(p)) {
            if (p->error) {
                return false;
            }
            break;
        }
    }
    return true;
}

// ============================================================================
// Compiler: instruction types
// ============================================================================

typedef enum {
    INST_ADD_FACT,
    INST_ADD_RULE,
    INST_CSP_VAR,
    INST_VARS_FROM_REL,
    INST_CONSTRAIN_PAIRS,
    INST_CSP_STANDALONE_CONSTR,
    INST_SOLVE,
    INST_SOLVE_ALL,
    INST_QUERY,
    INST_CSP_ALLDIFF,
    INST_CSP_LINEAR,
} dsl_inst_kind_t;

// Rule body atom.
typedef struct {
    n00b_string_t *functor;
    int32_t        arity;
    n00b_string_t *args[16];
    bool           negated;
} dsl_body_atom_t;

typedef struct {
    dsl_inst_kind_t kind;

    // INST_ADD_FACT.
    n00b_string_t  *functor;
    int32_t         arity;
    n00b_string_t  *args[16];
    bool            args_is_int[16];
    int64_t         args_int[16];

    // INST_ADD_RULE.
    n00b_string_t  *head_functor;
    int32_t         head_arity;
    n00b_string_t  *head_args[16];
    dsl_body_atom_t body_atoms[32];
    int32_t         body_count;

    // INST_CSP_VAR.
    n00b_string_t  *var_name;
    int64_t         dom_lo;
    int64_t         dom_hi;
    int64_t        *dom_vals;
    int32_t         dom_val_count;
    bool            dom_is_range;

    // INST_VARS_FROM_REL.
    n00b_string_t  *rel_name;
    int32_t         col;

    // INST_CONSTRAIN_PAIRS / INST_CSP_STANDALONE_CONSTR.
    dsl_tok_kind_t  cmp_op;
    n00b_string_t  *lhs_name;
    n00b_string_t  *rhs_name;
    bool            lhs_is_atom;
    bool            rhs_is_atom;

    // INST_QUERY goal list.
    dsl_body_atom_t query_goals[32];
    int32_t         query_goal_count;

    // INST_CSP_ALLDIFF.
    n00b_string_t **alldiff_vars;
    int32_t         alldiff_count;

    // INST_CSP_LINEAR.
    n00b_string_t **linear_vars;
    int64_t        *linear_coeffs;
    int32_t         linear_count;
    int64_t         linear_rhs;
} dsl_inst_t;

// ============================================================================
// Compiler
// ============================================================================

typedef struct {
    dsl_inst_t    *insts;
    int32_t        inst_count;
    int32_t        inst_cap;
    n00b_string_t *error;
    int32_t        error_line;
    int32_t        error_col;
} dsl_compiler_t;

static void
compiler_add_inst(dsl_compiler_t *c, dsl_inst_t inst)
{
    if (c->inst_count >= c->inst_cap) {
        int32_t     new_cap = c->inst_cap ? c->inst_cap * 2 : 32;
        dsl_inst_t *new_i   = n00b_alloc_array(dsl_inst_t, new_cap);
        if (c->inst_count > 0) {
            memcpy(new_i, c->insts, c->inst_count * sizeof(dsl_inst_t));
        }
        n00b_free(c->insts);
        c->insts    = new_i;
        c->inst_cap = new_cap;
    }
    c->insts[c->inst_count++] = inst;
}

static void
compile_fact(dsl_compiler_t *c, dsl_ast_t *stmt)
{
    dsl_ast_t *atom = stmt->children[0];
    dsl_inst_t inst = {};

    inst.kind  = INST_ADD_FACT;
    inst.arity = atom->child_count;
    inst.functor = atom->name;

    for (int32_t i = 0; i < atom->child_count && i < 16; i++) {
        dsl_ast_t *arg = atom->children[i];
        inst.args[i] = arg->name;
        if (arg->kind == AST_TERM_INT) {
            inst.args_is_int[i] = true;
            inst.args_int[i]    = arg->int_val;
        }
    }

    compiler_add_inst(c, inst);
}

static void
compile_body_atom(dsl_body_atom_t *dst, dsl_ast_t *goal)
{
    if (goal->kind == AST_GOAL_NOT) {
        dst->negated = true;
        goal = goal->children[0];
    }

    dst->functor = goal->name;
    dst->arity = goal->child_count;
    for (int32_t i = 0; i < goal->child_count && i < 16; i++) {
        dst->args[i] = goal->children[i]->name;
    }
}

static void
compile_rule(dsl_compiler_t *c, dsl_ast_t *stmt)
{
    dsl_ast_t *head = stmt->children[0];
    dsl_inst_t inst = {};

    inst.kind       = INST_ADD_RULE;
    inst.head_arity = head->child_count;
    inst.head_functor = head->name;
    for (int32_t i = 0; i < head->child_count && i < 16; i++) {
        inst.head_args[i] = head->children[i]->name;
    }

    // Body goals (children 1+).
    inst.body_count = 0;
    for (int32_t i = 1; i < stmt->child_count && inst.body_count < 32; i++) {
        dsl_ast_t *goal = stmt->children[i];
        if (goal->kind == AST_ATOM || goal->kind == AST_GOAL_NOT) {
            compile_body_atom(&inst.body_atoms[inst.body_count++], goal);
        }
        // Comparison goals in rule bodies are not compiled to Datalog
        // rules — they become CSP constraints generated at Phase 4.
    }

    compiler_add_inst(c, inst);
}

static void
fill_domain(dsl_inst_t *inst, dsl_ast_t *dom)
{
    if (dom->kind == AST_DOMAIN_RANGE) {
        inst->dom_is_range = true;
        inst->dom_lo       = dom->dom_lo;
        inst->dom_hi       = dom->dom_hi;
    }
    else {
        inst->dom_is_range    = false;
        inst->dom_vals        = dom->dom_vals;
        inst->dom_val_count   = dom->dom_val_count;
        // Take ownership — prevent double-free.
        dom->dom_vals         = nullptr;
        dom->dom_val_count    = 0;
    }
}

static void
compile_var_decl(dsl_compiler_t *c, dsl_ast_t *stmt)
{
    dsl_ast_t *dom  = stmt->children[0];
    dsl_inst_t inst = {};

    inst.kind = INST_CSP_VAR;
    inst.var_name = stmt->name;
    fill_domain(&inst, dom);

    compiler_add_inst(c, inst);
}

static void
compile_cvar_decl(dsl_compiler_t *c, dsl_ast_t *stmt)
{
    dsl_ast_t *atom = stmt->children[0]; // e.g., color(Node)
    dsl_ast_t *dom  = stmt->children[1]; // domain

    // This becomes VARS_FROM_REL: extract symbols from body relations.
    // The body atoms (children 2+) determine which relation to use.
    // For simplicity, if there's a body, use the first body atom's relation
    // for variable extraction. If no body, create a standalone CSP var.

    if (stmt->child_count > 2) {
        // Has body — extract from first body relation.
        for (int32_t bi = 2; bi < stmt->child_count; bi++) {
            dsl_ast_t *body_goal = stmt->children[bi];
            if (body_goal->kind != AST_ATOM) {
                continue;
            }

            // For each column in the body atom, if the arg name matches
            // one of the head atom's args, emit VARS_FROM_REL for that column.
            for (int32_t col = 0; col < body_goal->child_count; col++) {
                for (int32_t ha = 0; ha < atom->child_count; ha++) {
                    if (n00b_unicode_str_eq(body_goal->children[col]->name,
                                           atom->children[ha]->name)) {
                        dsl_inst_t inst = {};
                        inst.kind = INST_VARS_FROM_REL;
                        inst.rel_name = body_goal->name;
                        inst.functor  = atom->name;
                        inst.col = col;
                        fill_domain(&inst, dom);
                        compiler_add_inst(c, inst);
                    }
                }
            }
        }
    }
    else {
        // No body — standalone.
        dsl_inst_t inst = {};
        inst.kind = INST_CSP_VAR;
        inst.var_name = atom->name;
        fill_domain(&inst, dom);
        compiler_add_inst(c, inst);
    }
}

static n00b_csp_con_kind_t
tok_to_csp_kind(dsl_tok_kind_t op)
{
    switch (op) {
    case TOK_EQ: return N00B_CSP_CON_EQ;
    case TOK_NE: return N00B_CSP_CON_NE;
    case TOK_LT: return N00B_CSP_CON_LT;
    case TOK_LE: return N00B_CSP_CON_LE;
    case TOK_GT: return N00B_CSP_CON_LT;  // Swap operands.
    case TOK_GE: return N00B_CSP_CON_LE;  // Swap operands.
    default:     return N00B_CSP_CON_NE;
    }
}

static void
compile_constraint(dsl_compiler_t *c, dsl_ast_t *stmt)
{
    dsl_ast_t *lhs = stmt->children[0];
    dsl_ast_t *rhs = stmt->children[1];

    if (stmt->child_count > 2) {
        // Conditional constraint with body.
        // Emit CONSTRAIN_PAIRS using the first body relation.
        for (int32_t bi = 2; bi < stmt->child_count; bi++) {
            dsl_ast_t *body_goal = stmt->children[bi];
            if (body_goal->kind != AST_ATOM) {
                continue;
            }

            // Check if lhs and rhs args match body columns.
            dsl_inst_t inst = {};
            inst.kind = INST_CONSTRAIN_PAIRS;
            inst.rel_name = body_goal->name;
            inst.cmp_op = stmt->cmp_op;

            // For atoms like color(X) != color(Y) :- edge(X, Y),
            // we need to find which columns of edge contain X and Y.
            // For now, just emit a constrain_pairs for the relation.
            inst.lhs_name = lhs->name;
            inst.rhs_name = rhs->name;
            inst.lhs_is_atom = (lhs->kind == AST_ATOM);
            inst.rhs_is_atom = (rhs->kind == AST_ATOM);

            compiler_add_inst(c, inst);
            return;  // Only use first body relation.
        }
    }
    else {
        // Standalone constraint (no body).
        dsl_inst_t inst = {};
        inst.kind = INST_CSP_STANDALONE_CONSTR;
        inst.cmp_op = stmt->cmp_op;

        inst.lhs_name    = lhs->name;
        inst.lhs_is_atom = (lhs->kind == AST_ATOM);
        inst.rhs_name    = rhs->name;
        inst.rhs_is_atom = (rhs->kind == AST_ATOM);

        compiler_add_inst(c, inst);
    }
}

static void
compile_alldiff(dsl_compiler_t *c, dsl_ast_t *stmt)
{
    // For chained != from atom path, linear_count holds the var child count.
    // For plain alldiff(...), all children are variable terms.
    int32_t var_limit = stmt->linear_count > 0
                            ? stmt->linear_count
                            : stmt->child_count;

    bool has_body = (stmt->linear_count > 0
                     && stmt->child_count > stmt->linear_count);
    bool has_atoms = (var_limit > 0
                      && stmt->children[0]->kind == AST_ATOM);

    if (has_atoms && has_body) {
        // Atom chained !=  with body:
        //   color(X) != color(Y) != color(Z) :- edge(X,Y), edge(Y,Z).
        // Decompose into pairwise CONSTRAIN_PAIRS, reusing the body
        // relation — identical to the existing pairwise constraint path.
        n00b_string_t *body_rel = nullptr;
        for (int32_t bi = var_limit; bi < stmt->child_count; bi++) {
            dsl_ast_t *goal = stmt->children[bi];
            if (goal->kind == AST_ATOM) {
                body_rel = goal->name;
                break;
            }
        }

        for (int32_t i = 0; i < var_limit; i++) {
            for (int32_t j = i + 1; j < var_limit; j++) {
                dsl_inst_t inst = {};
                inst.kind     = INST_CONSTRAIN_PAIRS;
                inst.rel_name = body_rel;
                inst.cmp_op   = TOK_NE;

                inst.lhs_name    = stmt->children[i]->name;
                inst.rhs_name    = stmt->children[j]->name;
                inst.lhs_is_atom = (stmt->children[i]->kind == AST_ATOM);
                inst.rhs_is_atom = (stmt->children[j]->kind == AST_ATOM);

                compiler_add_inst(c, inst);
            }
        }
        return;
    }

    // Simple case: alldiff(x, y, z) or standalone chained x != y != z.
    // Children are simple variable/constant terms — names map directly
    // to CSP variable names.
    dsl_inst_t inst = {};
    inst.kind = INST_CSP_ALLDIFF;
    inst.alldiff_count = var_limit;
    inst.alldiff_vars  = n00b_alloc_array(n00b_string_t *, var_limit);

    for (int32_t i = 0; i < var_limit; i++) {
        inst.alldiff_vars[i] = stmt->children[i]->name;
    }

    compiler_add_inst(c, inst);
}

static void
compile_linear(dsl_compiler_t *c, dsl_ast_t *stmt)
{
    dsl_inst_t inst   = {};
    inst.kind         = INST_CSP_LINEAR;
    inst.linear_count = stmt->linear_count;
    inst.linear_rhs   = stmt->linear_rhs;
    inst.linear_vars  = n00b_alloc_array(n00b_string_t *, stmt->linear_count);
    inst.linear_coeffs = n00b_alloc_array(int64_t, stmt->linear_count);

    for (int32_t i = 0; i < stmt->linear_count; i++) {
        inst.linear_vars[i]   = stmt->children[i]->name;
        inst.linear_coeffs[i] = stmt->linear_coeffs[i];
    }

    compiler_add_inst(c, inst);
}

static bool
compile_stmts(dsl_compiler_t *c, dsl_ast_t **stmts, int32_t count)
{
    for (int32_t i = 0; i < count; i++) {
        dsl_ast_t *stmt = stmts[i];

        switch (stmt->kind) {
        case AST_FACT:
            compile_fact(c, stmt);
            break;
        case AST_RULE:
            compile_rule(c, stmt);
            break;
        case AST_VAR_DECL:
            compile_var_decl(c, stmt);
            break;
        case AST_CVAR_DECL:
            compile_cvar_decl(c, stmt);
            break;
        case AST_CONSTRAINT:
            compile_constraint(c, stmt);
            break;
        case AST_ALLDIFF:
            compile_alldiff(c, stmt);
            break;
        case AST_LINEAR:
            compile_linear(c, stmt);
            break;
        case AST_QUERY:
            // Queries are deferred to execution phase.
            {
                dsl_inst_t inst = {};
                inst.kind = INST_QUERY;
                inst.query_goal_count = 0;
                for (int32_t g = 0; g < stmt->child_count
                         && inst.query_goal_count < 32; g++) {
                    dsl_ast_t *goal = stmt->children[g];
                    if (goal->kind == AST_ATOM) {
                        compile_body_atom(
                            &inst.query_goals[inst.query_goal_count++], goal);
                    }
                }
                compiler_add_inst(c, inst);
            }
            break;
        case AST_SOLVE:
            {
                dsl_inst_t inst = {};
                inst.kind = stmt->solve_all ? INST_SOLVE_ALL : INST_SOLVE;
                compiler_add_inst(c, inst);
            }
            break;
        default:
            break;
        }
    }
    return true;
}

// ============================================================================
// Executor
// ============================================================================

static n00b_csp_domain_t
make_domain(dsl_inst_t *inst)
{
    if (inst->dom_is_range) {
        return n00b_csp_dom_range(inst->dom_lo, inst->dom_hi);
    }
    return n00b_csp_dom_from_values(inst->dom_vals, inst->dom_val_count);
}

// Helper: create or find a CSP variable by name.
static n00b_csp_var_id_t
ensure_csp_var(n00b_logic_t *prog, n00b_string_t *name, n00b_csp_domain_t dom)
{
    if (prog->store) {
        n00b_option_t(n00b_csp_var_id_t) opt = n00b_csp_find_var(prog->store,
                                                                     name);
        if (n00b_option_is_set(opt)) {
            return n00b_option_get(opt);
        }
    }
    return n00b_logic_csp_var(prog, name, dom);
}

static n00b_csp_var_id_t
find_csp_var(n00b_logic_t *prog, n00b_string_t *name)
{
    if (prog->store) {
        n00b_option_t(n00b_csp_var_id_t) opt = n00b_csp_find_var(prog->store,
                                                                     name);
        if (n00b_option_is_set(opt)) {
            return n00b_option_get(opt);
        }
    }
    return -1;
}

// Adapter to bridge n00b_logic_solution_cb to n00b_csp_solution_cb.
typedef struct {
    n00b_logic_solution_cb  cb;
    n00b_logic_t           *prog;
    void                   *ctx;
} dsl_solve_all_ctx_t;

static bool
dsl_solve_all_adapter(n00b_csp_store_t *s, void *raw_ctx)
{
    (void)s;
    dsl_solve_all_ctx_t *sa = (dsl_solve_all_ctx_t *)raw_ctx;
    if (sa->cb) {
        return sa->cb(sa->prog, sa->ctx);
    }
    return true;
}

static bool
is_var_name(n00b_string_t *name)
{
    return name && name->u8_bytes > 0 && isupper((unsigned char)name->data[0]);
}

static n00b_dl_sym_t
intern_sym(n00b_logic_t *prog, n00b_string_t *name, bool is_int, int64_t int_val)
{
    if (is_int) {
        return n00b_logic_int(prog, int_val);
    }
    if (is_var_name(name)
        || (name->u8_bytes > 1 && name->data[0] == '_')) {
        return n00b_logic_var(prog, name);
    }
    return n00b_logic_const(prog, name);
}

static bool
execute_program(n00b_logic_t *prog, dsl_inst_t *insts, int32_t count,
                n00b_logic_solution_cb cb, void *ctx,
                n00b_dsl_result_t *result, bool run_solve)
{
    // Phase 1: Add facts and rules.
    for (int32_t i = 0; i < count; i++) {
        dsl_inst_t *inst = &insts[i];

        if (inst->kind == INST_ADD_FACT) {
            n00b_dl_rel_id_t rel = n00b_logic_relation(prog, inst->functor,
                                                         inst->arity);
            n00b_dl_sym_t syms[16];
            for (int32_t a = 0; a < inst->arity; a++) {
                syms[a] = intern_sym(prog, inst->args[a],
                                     inst->args_is_int[a],
                                     inst->args_int[a]);
            }
            n00b_logic_add_fact(prog, rel, inst->arity, syms);
        }

        if (inst->kind == INST_ADD_RULE) {
            n00b_dl_rel_id_t rel = n00b_logic_relation(prog,
                                                         inst->head_functor,
                                                         inst->head_arity);
            n00b_dl_sym_t head_syms[16];
            for (int32_t a = 0; a < inst->head_arity; a++) {
                head_syms[a] = intern_sym(prog, inst->head_args[a],
                                          false, 0);
            }

            n00b_dl_rule_builder_t rb;
            n00b_dl_rule_builder_init(&rb);
            n00b_dl_rule_builder_head(&rb, rel, inst->head_arity, head_syms);

            for (int32_t b = 0; b < inst->body_count; b++) {
                dsl_body_atom_t *ba  = &inst->body_atoms[b];
                n00b_dl_rel_id_t brel = n00b_logic_relation(prog, ba->functor,
                                                              ba->arity);
                n00b_dl_sym_t body_syms[16];
                for (int32_t a = 0; a < ba->arity; a++) {
                    body_syms[a] = intern_sym(prog, ba->args[a], false, 0);
                }
                n00b_dl_rule_builder_add(&rb, brel, ba->arity, body_syms,
                                           ba->negated);
            }

            n00b_logic_add_rule(prog, n00b_dl_rule_builder_finish(&rb));
        }
    }

    // Phase 2: Run Datalog.
    if (!n00b_logic_run_datalog(prog)) {
        result->error = r"Datalog stratification failed";
        return false;
    }

    // Phase 3: Create CSP variables.
    for (int32_t i = 0; i < count; i++) {
        dsl_inst_t *inst = &insts[i];

        if (inst->kind == INST_CSP_VAR) {
            n00b_csp_domain_t dom = make_domain(inst);
            ensure_csp_var(prog, inst->var_name, dom);
        }

        if (inst->kind == INST_VARS_FROM_REL) {
            n00b_dl_rel_id_t rel = n00b_logic_relation(prog, inst->rel_name,
                                                         0);
            // Arity 0 means "look up existing relation".
            // Actually, n00b_logic_relation does lookup-or-create. Since
            // the relation was already created in Phase 1, it'll return
            // the existing one regardless of arity.
            // But we need correct arity. Let's just use a large value
            // and let the function figure it out.
            // Actually, the function uses name-based lookup and arity
            // is only used for creation. Since it already exists, arity
            // is ignored. But we should pass a reasonable value.
            // Let's pass inst->col+1 as minimum arity hint.
            n00b_csp_domain_t dom = make_domain(inst);
            n00b_logic_vars_from_rel(prog, rel, inst->col, dom);
        }
    }

    // Phase 4: Post constraints.
    for (int32_t i = 0; i < count; i++) {
        dsl_inst_t *inst = &insts[i];

        if (inst->kind == INST_CONSTRAIN_PAIRS) {
            n00b_dl_rel_id_t rel = n00b_logic_relation(prog, inst->rel_name,
                                                         0);
            n00b_csp_con_kind_t ck = tok_to_csp_kind(inst->cmp_op);
            if (!n00b_logic_constrain_pairs(prog, rel, ck)) {
                result->error = r"constraint posting failed (unsatisfiable)";
                return false;
            }
        }

        if (inst->kind == INST_CSP_STANDALONE_CONSTR) {
            n00b_csp_var_id_t lhs = find_csp_var(prog, inst->lhs_name);
            n00b_csp_var_id_t rhs = find_csp_var(prog, inst->rhs_name);

            if (lhs < 0 || rhs < 0) {
                result->error = r"constraint references undefined variable";
                return false;
            }

            bool ok = true;
            n00b_csp_con_kind_t ck = tok_to_csp_kind(inst->cmp_op);

            // Handle GT/GE by swapping operands.
            if (inst->cmp_op == TOK_GT || inst->cmp_op == TOK_GE) {
                n00b_csp_var_id_t tmp = lhs;
                lhs = rhs;
                rhs = tmp;
            }

            switch (ck) {
            case N00B_CSP_CON_EQ:
                ok = n00b_logic_csp_eq(prog, lhs, rhs);
                break;
            case N00B_CSP_CON_NE:
                ok = n00b_logic_csp_ne(prog, lhs, rhs);
                break;
            case N00B_CSP_CON_LT:
                ok = n00b_logic_csp_lt(prog, lhs, rhs);
                break;
            case N00B_CSP_CON_LE:
                ok = n00b_logic_csp_le(prog, lhs, rhs);
                break;
            default:
                break;
            }

            if (!ok) {
                result->error = r"standalone constraint failed (unsatisfiable)";
                return false;
            }
        }

        if (inst->kind == INST_CSP_ALLDIFF) {
            if (!prog->store) {
                result->error = r"alldiff requires CSP variables";
                return false;
            }
            n00b_csp_var_id_t *vars = n00b_alloc_array(n00b_csp_var_id_t,
                                                         inst->alldiff_count);
            for (int32_t j = 0; j < inst->alldiff_count; j++) {
                vars[j] = find_csp_var(prog, inst->alldiff_vars[j]);
                if (vars[j] < 0) {
                    n00b_free(vars);
                    result->error = r"alldiff references undefined variable";
                    return false;
                }
            }
            if (!n00b_csp_post_alldiff(prog->store, vars, inst->alldiff_count)) {
                n00b_free(vars);
                result->error = r"alldiff constraint failed (unsatisfiable)";
                return false;
            }
            n00b_free(vars);
        }

        if (inst->kind == INST_CSP_LINEAR) {
            if (!prog->store) {
                result->error = r"linear constraint requires CSP variables";
                return false;
            }
            n00b_csp_var_id_t *vars = n00b_alloc_array(n00b_csp_var_id_t,
                                                         inst->linear_count);
            for (int32_t j = 0; j < inst->linear_count; j++) {
                vars[j] = find_csp_var(prog, inst->linear_vars[j]);
                if (vars[j] < 0) {
                    n00b_free(vars);
                    result->error = r"linear constraint references undefined variable";
                    return false;
                }
            }
            if (!n00b_csp_post_linear(prog->store, vars, inst->linear_coeffs,
                                       inst->linear_count, inst->linear_rhs)) {
                n00b_free(vars);
                result->error = r"linear constraint failed (unsatisfiable)";
                return false;
            }
            n00b_free(vars);
        }
    }

    // Phase 5: Run CSP propagation.
    if (!n00b_logic_run_csp(prog)) {
        result->error = r"CSP propagation failed (unsatisfiable)";
        return false;
    }

    if (!run_solve) {
        return true;
    }

    // Phase 6: Execute solve/query commands.
    for (int32_t i = 0; i < count; i++) {
        dsl_inst_t *inst = &insts[i];

        if (inst->kind == INST_SOLVE) {
            if (prog->store) {
                result->solved = n00b_csp_label(prog->store);
            }
            else {
                result->solved = true;
            }
        }

        if (inst->kind == INST_SOLVE_ALL) {
            if (prog->store) {
                if (cb) {
                    dsl_solve_all_ctx_t sa_ctx = {
                        .cb   = cb,
                        .prog = prog,
                        .ctx  = ctx,
                    };
                    result->solution_count = n00b_csp_label_all(
                        prog->store, dsl_solve_all_adapter, &sa_ctx);
                }
                else {
                    result->solution_count = n00b_csp_label_all(
                        prog->store, nullptr, nullptr);
                }
            }
            else {
                result->solution_count = 1;
                result->solved = true;
            }
        }
    }

    return true;
}

// ============================================================================
// Public API
// ============================================================================

static n00b_dsl_result_t
dsl_process(n00b_string_t *src, n00b_logic_solution_cb cb, void *ctx,
            bool run_solve)
{
    n00b_dsl_result_t result = {};

    if (!src || !src->data || src->u8_bytes == 0) {
        // Empty program.
        result.prog = n00b_alloc(n00b_logic_t);
        n00b_logic_init(result.prog);
        return result;
    }

    // Lex.
    dsl_lexer_t lex = {
        .src     = src->data,
        .src_len = src->u8_bytes,
        .pos     = 0,
        .line    = 1,
        .col     = 1,
    };

    if (!lex_tokenize(&lex)) {
        result.error      = lex.error;
        result.error_line = lex.error_line;
        result.error_col  = lex.error_col;
        n00b_free(lex.tokens);
        return result;
    }

    // Parse.
    dsl_parser_t parser = {
        .tokens    = lex.tokens,
        .tok_count = lex.tok_count,
        .pos       = 0,
    };

    if (!parse_program(&parser)) {
        result.error      = parser.error;
        result.error_line = parser.error_line;
        result.error_col  = parser.error_col;
        // Clean up.
        for (int32_t i = 0; i < parser.stmt_count; i++) {
            ast_free(parser.stmts[i]);
        }
        n00b_free(parser.stmts);
        n00b_free(lex.tokens);
        return result;
    }

    // Compile.
    dsl_compiler_t compiler = {};
    compile_stmts(&compiler, parser.stmts, parser.stmt_count);

    // Execute.
    result.prog = n00b_alloc(n00b_logic_t);
    n00b_logic_init(result.prog);

    execute_program(result.prog, compiler.insts, compiler.inst_count,
                    cb, ctx, &result, run_solve);

    // Clean up.
    for (int32_t i = 0; i < compiler.inst_count; i++) {
        n00b_free(compiler.insts[i].dom_vals);
        n00b_free(compiler.insts[i].alldiff_vars);
        n00b_free(compiler.insts[i].linear_vars);
        n00b_free(compiler.insts[i].linear_coeffs);
    }
    n00b_free(compiler.insts);
    for (int32_t i = 0; i < parser.stmt_count; i++) {
        ast_free(parser.stmts[i]);
    }
    n00b_free(parser.stmts);
    n00b_free(lex.tokens);

    return result;
}

n00b_dsl_result_t
n00b_dsl_compile(n00b_string_t *src)
{
    return dsl_process(src, nullptr, nullptr, false);
}

n00b_dsl_result_t
n00b_dsl_run(n00b_string_t *src, n00b_logic_solution_cb cb, void *ctx)
{
    return dsl_process(src, cb, ctx, true);
}

void
n00b_dsl_result_free(n00b_dsl_result_t *r)
{
    if (!r) {
        return;
    }
    if (r->prog) {
        n00b_logic_free(r->prog);
        n00b_free(r->prog);
        r->prog = nullptr;
    }
}
