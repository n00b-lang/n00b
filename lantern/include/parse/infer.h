#pragma once

#include "lib/string.h"
#include "parse/grammar.h"
#include "parse/tree_visit.h"
#include "parse/symtab.h"

typedef enum : uint8_t {
    PARSE_INFER_CHILD,
    PARSE_INFER_SELF,
    PARSE_INFER_RETURN,
    PARSE_INFER_TVAR,
    PARSE_INFER_NAMED,
    PARSE_INFER_NAMED_APP,
    PARSE_INFER_CHILD_APP,
    PARSE_INFER_SPREAD,
    PARSE_INFER_LOOKUP,
    PARSE_INFER_PROJECT,
    PARSE_INFER_COMPARE,
    PARSE_INFER_METHOD_RESULT,
    PARSE_INFER_UNIFY,
    PARSE_INFER_CONVERT,
    PARSE_INFER_SUM,
    PARSE_INFER_CHILDREN,
} parse_infer_kind_t;

typedef enum : uint8_t {
    PARSE_INFER_RETURN_OF,
    PARSE_INFER_ELEMENT_OF,
    PARSE_INFER_SLICE_OF,
    PARSE_INFER_UNWRAP_RESULT,
    PARSE_INFER_METHOD_RETURN,
    PARSE_INFER_FIELD_RETURN,
    PARSE_INFER_FIELD_RETURN_AUTO_DEREF,
} parse_infer_proj_t;

typedef struct parse_infer_expr_t parse_infer_expr_t;
typedef th_type *(*parse_infer_type_node_resolver_fn)(
    th_ctx *types, parse_grammar_t *grammar, parse_tree_t *node,
    void *context);

struct parse_infer_expr_t {
    parse_infer_kind_t   kind;
    parse_infer_proj_t   proj;
    int32_t            index;
    core_string_t       name;
    core_string_t       other_name;
    parse_infer_expr_t  *lhs;
    parse_infer_expr_t  *rhs;
    parse_infer_expr_t **args;
    size_t             n_args;
};

parse_infer_expr_t *parse_infer_parse(core_string_t expr);
parse_infer_expr_t *parse_infer_clone(const parse_infer_expr_t *e);
void              parse_infer_free(parse_infer_expr_t *e);
core_string_t     parse_infer_print(const parse_infer_expr_t *e);
core_string_t     parse_infer_kind_name(parse_infer_kind_t k);
core_string_t     parse_infer_proj_name(parse_infer_proj_t p);

typedef struct parse_infer_lower_t parse_infer_lower_t;

parse_infer_lower_t *parse_infer_lower_new(void);
void               parse_infer_lower_free(parse_infer_lower_t *l);
bool               parse_infer_lower_failed(const parse_infer_lower_t *l);
void               parse_infer_install(parse_tree_walk_t   *w,
                                     parse_infer_lower_t *l);
parse_infer_expr_t  *parse_infer_reduced(void *result);

th_type *parse_infer_eval(const parse_infer_expr_t *e,
                        parse_visit_t            *v,
                        th_ctx                 *tc,
                        parse_symtab_t           *st,
                        bool                   *ok);
th_type *parse_infer_eval_with_type_node_resolver(
    const parse_infer_expr_t *e, parse_visit_t *v, th_ctx *tc,
    parse_symtab_t *st, parse_infer_type_node_resolver_fn resolver,
    void *resolver_context, bool *ok);
