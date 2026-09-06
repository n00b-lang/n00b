#pragma once

#include "lib/string.h"
#include "parse/grammar.h"
#include "parse/tree_visit.h"
#include "parse/symtab.h"

typedef enum : uint8_t {
    NCC_INFER_CHILD,
    NCC_INFER_SELF,
    NCC_INFER_RETURN,
    NCC_INFER_TVAR,
    NCC_INFER_NAMED,
    NCC_INFER_NAMED_APP,
    NCC_INFER_CHILD_APP,
    NCC_INFER_SPREAD,
    NCC_INFER_LOOKUP,
    NCC_INFER_PROJECT,
    NCC_INFER_UNIFY,
    NCC_INFER_SUM,
} ncc_infer_kind_t;

typedef enum : uint8_t {
    NCC_INFER_RETURN_OF,
    NCC_INFER_ELEMENT_OF,
    NCC_INFER_UNWRAP_RESULT,
    NCC_INFER_METHOD_RETURN,
} ncc_infer_proj_t;

typedef struct ncc_infer_expr_t ncc_infer_expr_t;

struct ncc_infer_expr_t {
    ncc_infer_kind_t   kind;
    ncc_infer_proj_t   proj;
    int32_t            index;
    ncc_string_t       name;
    ncc_infer_expr_t  *lhs;
    ncc_infer_expr_t  *rhs;
    ncc_infer_expr_t **args;
    size_t             n_args;
};

ncc_infer_expr_t *ncc_infer_parse(ncc_string_t expr);
ncc_infer_expr_t *ncc_infer_clone(const ncc_infer_expr_t *e);
void              ncc_infer_free(ncc_infer_expr_t *e);
char             *ncc_infer_print(const ncc_infer_expr_t *e);
const char       *ncc_infer_kind_name(ncc_infer_kind_t k);
const char       *ncc_infer_proj_name(ncc_infer_proj_t p);

typedef struct ncc_infer_lower_t ncc_infer_lower_t;

ncc_infer_lower_t *ncc_infer_lower_new(void);
void               ncc_infer_lower_free(ncc_infer_lower_t *l);
bool               ncc_infer_lower_failed(const ncc_infer_lower_t *l);
void               ncc_infer_install(ncc_tree_walk_t   *w,
                                     ncc_infer_lower_t *l);
ncc_infer_expr_t  *ncc_infer_reduced(void *result);

th_type *ncc_infer_eval(const ncc_infer_expr_t *e,
                        ncc_visit_t            *v,
                        th_ctx                 *tc,
                        ncc_symtab_t           *st,
                        bool                   *ok);
