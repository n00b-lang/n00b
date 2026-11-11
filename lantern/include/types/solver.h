#ifndef TYPES_SOLVER_H
#define TYPES_SOLVER_H

#include "ir/ir.h"
#include "lib/buffer.h"
#include "logic/datalog.h"
#include "th/typehash.h"

typedef struct ty_solver ty_solver_t;

typedef struct {
    const ir_instr_t *at;
    uint16_t          rule;
} ty_why_t;

typedef struct {
    const ir_value_t *value;
    const void       *symbol;
} ty_origin_t;

typedef enum : uint8_t {
    TY_PRED_IMPLEMENTS,
    TY_PRED_OPERATOR,
    TY_PRED_DISPATCH,
    TY_PRED_FIELD,
    TY_PRED_ELEMENT,
    TY_PRED_FLOW,
    TY_PRED_REFERENT,
    TY_PRED_ONE_OF,
    TY_PRED_CONDITION,
} ty_pred_kind_t;

typedef struct {
    ty_pred_kind_t kind;
    core_string_t  name;
    th_type       *subject;
    th_type       *other;
    th_type       *result;
    bool           positional;
    bool           bound;
    uint32_t       index;
} ty_pred_t;

typedef enum : uint8_t {
    TY_FAIL_MISMATCH,
    TY_FAIL_OCCURS,
    TY_FAIL_NIL,
    TY_FAIL_PREDICATE,
    TY_FAIL_REFINE,
} ty_fail_kind_t;

typedef struct {
    ty_fail_kind_t kind;
    ty_why_t       why;
    ty_why_t       cause;
    th_type       *expected;
    th_type       *actual;
    ty_pred_t      pred;
} ty_failure_t;

typedef th_type *(*ty_method_fn)(void *context, th_type *receiver,
                                 core_string_t method);
typedef th_type *(*ty_call_method_fn)(void *context, th_type *receiver,
                                      core_string_t method, th_type *call);
typedef size_t (*ty_requirements_fn)(void *context, th_type *receiver,
                                     core_string_t method, th_type *call,
                                     th_type *signature,
                                     th_trait_requirement *requirements,
                                     size_t max_requirements);
typedef bool (*ty_coerce_fn)(void *context, th_type *to, th_type *from);
typedef core_string_t (*ty_param_name_fn)(void *context, th_type *receiver,
                                          core_string_t method, size_t index);
typedef bool (*ty_constant_index_fn)(void *context, const ir_instr_t *at,
                                     uint64_t *index);
typedef bool (*ty_omittable_fn)(void *context, th_type *record,
                                core_string_t member);

ty_solver_t *ty_solver_new(th_ctx *types, core_datalog_engine_t *logic);
void         ty_solver_set_methods(ty_solver_t *s, ty_call_method_fn methods,
                                   void *context);
void         ty_solver_set_requirements(ty_solver_t *s,
                                        ty_requirements_fn requirements,
                                        void *context);
void         ty_solver_set_fields(ty_solver_t *s, ty_method_fn fields,
                                  void *context);
void         ty_solver_set_coercion(ty_solver_t *s, ty_coerce_fn coerces,
                                    void *context);
void         ty_solver_set_param_names(ty_solver_t *s, ty_param_name_fn names,
                                       void *context);
void         ty_solver_set_constant_index(ty_solver_t *s,
                                          ty_constant_index_fn index,
                                          void *context);
void         ty_solver_set_omittable(ty_solver_t *s, ty_omittable_fn omittable,
                                     void *context);
void         ty_solver_free(ty_solver_t *s);
th_ctx      *ty_solver_types(ty_solver_t *s);

bool ty_declare_trait(ty_solver_t *s, core_string_t trait,
                      const core_string_t *methods, size_t n);
bool ty_declare_method(ty_solver_t *s, th_type *type, core_string_t method);
bool ty_declare_impl(ty_solver_t *s, th_type *type, core_string_t trait);
bool ty_declare_trait_method(ty_solver_t *s, core_string_t trait,
                             core_string_t method, th_type *contract);
bool ty_declare_default(ty_solver_t *s, core_string_t trait, th_type *type);
size_t   ty_open_count(ty_solver_t *s, core_string_t trait);
th_type *ty_open_at(const ty_solver_t *s, size_t i);
bool     ty_settle_open(ty_solver_t *s, core_string_t trait, th_type *var,
                        th_type *type);
bool ty_declare_operator(ty_solver_t *s, core_string_t op, th_type *left,
                         th_type *right, th_type *result);

th_type *ty_fresh(ty_solver_t *s, ty_origin_t origin);
bool     ty_equate(ty_solver_t *s, th_type *a, th_type *b, ty_why_t why);
bool     ty_unify(ty_solver_t *s, th_type *slot, th_type *value, ty_why_t why);
th_type *ty_join(ty_solver_t *s, th_type *a, th_type *b, ty_why_t why);
bool     ty_require(ty_solver_t *s, ty_pred_t pred, ty_why_t why);
th_type *ty_refine(ty_solver_t *s, th_type *t, th_type *to, ty_why_t why);

bool     ty_solve(ty_solver_t *s);
th_type *ty_generalize(ty_solver_t *s, th_type *t);
th_type *ty_instantiate(ty_solver_t *s, th_type *scheme, ty_why_t why);
th_type *ty_instantiate_declared(ty_solver_t *s, th_type *t, ty_why_t why);
th_type *ty_dispatch_instance(ty_solver_t *s, const ir_instr_t *at);
size_t ty_dispatch_count(const ty_solver_t *s);
const ir_instr_t *ty_dispatch_at(const ty_solver_t *s, size_t i);
size_t ty_conversion_count(const ty_solver_t *s);
const ir_instr_t *ty_conversion_at(const ty_solver_t *s, size_t i);
void     ty_declare_rigid(ty_solver_t *s, th_type *t);
bool     ty_declare_bound(ty_solver_t *s, th_type *var, ty_pred_t bound,
                          ty_why_t why);
th_type *ty_resolve(ty_solver_t *s, th_type *t);
void     ty_explain(ty_solver_t *s, th_type *t, core_buffer_t *out);

size_t              ty_failure_count(const ty_solver_t *s);
const ty_failure_t *ty_failure_at(const ty_solver_t *s, size_t i);
size_t              ty_pending_count(const ty_solver_t *s);

#endif
