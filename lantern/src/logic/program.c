#include "logic/program.h"

#include <errno.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/dict.h"

struct core_logic_program {
    core_datalog_engine_t *datalog;
    core_csp_store_t *store;
    core_dict_t symbol_to_var;
    bool datalog_ready;
};

core_logic_program_t *core_logic_program_new(void)
{
    core_logic_program_t *program = core_alloc(core_logic_program_t);
    memset(program, 0, sizeof(*program));
    program->datalog = core_datalog_engine_new();
    core_dict_init(&program->symbol_to_var, core_hash_word, nullptr);
    return program;
}

void core_logic_program_free(core_logic_program_t *program)
{
    if (!program) return;
    core_datalog_engine_free(program->datalog);
    core_csp_store_free(program->store);
    core_dict_free(&program->symbol_to_var);
    core_free(program);
}

core_datalog_engine_t *core_logic_datalog(core_logic_program_t *program)
{
    return program ? program->datalog : nullptr;
}

static bool symbol_var_get(core_logic_program_t *program,
                           core_datalog_symbol_t symbol,
                           core_csp_var_id_t *var)
{
    bool found = false;
    void *value = core_dict_get(&program->symbol_to_var,
                                (void *)(uintptr_t)symbol, &found);
    if (found && var) *var = (core_csp_var_id_t)(uintptr_t)value;
    return found;
}

static void symbol_var_put(core_logic_program_t *program,
                           core_datalog_symbol_t symbol,
                           core_csp_var_id_t var)
{
    core_dict_put(&program->symbol_to_var, (void *)(uintptr_t)symbol,
                  (uintptr_t)var);
}

static bool ensure_store(core_logic_program_t *program)
{
    if (!program) return false;
    if (!program->store) program->store = core_csp_store_new();
    return program->store != nullptr;
}

core_csp_var_id_t core_logic_csp_var(core_logic_program_t *program,
                                     core_string_t name,
                                     core_csp_domain_t domain)
{
    if (!program || !name.data || !ensure_store(program)) {
        core_csp_dom_free(&domain);
        return -1;
    }
    core_datalog_symbol_t symbol = core_datalog_constant(program->datalog,
                                                         name);
    core_csp_var_id_t existing;
    if (symbol_var_get(program, symbol, &existing)) {
        core_csp_dom_free(&domain);
        return existing;
    }
    core_csp_var_id_t var = core_csp_new_var(program->store, name, domain);
    symbol_var_put(program, symbol, var);
    return var;
}

core_option_t(core_csp_var_id_t)
core_logic_csp_find(core_logic_program_t *program,
                    core_datalog_symbol_t symbol)
{
    core_csp_var_id_t var;
    if (program && symbol_var_get(program, symbol, &var))
        return core_option_set(core_csp_var_id_t, var);
    return core_option_none(core_csp_var_id_t);
}

typedef struct {
    core_logic_program_t *program;
    int32_t column;
    const core_csp_domain_t *domain;
    int32_t created;
    bool failed;
} bridge_vars_context_t;

static bool bridge_var_from_tuple(const core_datalog_symbol_t *tuple,
                                  int32_t arity, void *opaque)
{
    bridge_vars_context_t *context = opaque;
    if (context->column < 0 || context->column >= arity) {
        context->failed = true;
        return false;
    }
    core_datalog_symbol_t symbol = tuple[context->column];
    if (core_datalog_is_variable(symbol)) {
        context->failed = true;
        return false;
    }
    if (symbol_var_get(context->program, symbol, nullptr)) return true;
    core_string_t name = core_datalog_symbol_name(context->program->datalog,
                                                   symbol);
    if (!name.data) {
        context->failed = true;
        return false;
    }
    core_csp_domain_t copy = core_csp_dom_clone(context->domain);
    core_csp_var_id_t var = core_csp_new_var(
        context->program->store, name, copy);
    symbol_var_put(context->program, symbol, var);
    context->created++;
    return true;
}

int32_t core_logic_vars_from_relation(core_logic_program_t *program,
                                      core_datalog_relation_id_t relation,
                                      int32_t column,
                                      const core_csp_domain_t *domain)
{
    if (!program || !domain || !program->datalog_ready
        || !ensure_store(program)) return -1;
    core_option_t(int32_t) arity
        = core_datalog_relation_arity(program->datalog, relation);
    if (!arity.has_value || column < 0 || column >= arity.value) return -1;
    bridge_vars_context_t context = {
        .program = program, .column = column, .domain = domain};
    core_datalog_query(program->datalog, relation, bridge_var_from_tuple,
                       &context);
    return context.failed ? -1 : context.created;
}

typedef struct {
    core_logic_program_t *program;
    core_csp_con_kind_t constraint;
    bool failed;
} bridge_pairs_context_t;

static bool bridge_pair_from_tuple(const core_datalog_symbol_t *tuple,
                                   int32_t arity, void *opaque)
{
    bridge_pairs_context_t *context = opaque;
    if (arity != 2) {
        context->failed = true;
        return false;
    }
    core_csp_var_id_t left, right;
    if (!symbol_var_get(context->program, tuple[0], &left)
        || !symbol_var_get(context->program, tuple[1], &right)) {
        context->failed = true;
        return false;
    }
    bool ok;
    switch (context->constraint) {
    case CORE_CSP_CON_EQ:
        ok = core_csp_post_eq(context->program->store, left, right);
        break;
    case CORE_CSP_CON_NE:
        ok = core_csp_post_ne(context->program->store, left, right);
        break;
    case CORE_CSP_CON_LT:
        ok = core_csp_post_lt(context->program->store, left, right);
        break;
    case CORE_CSP_CON_LE:
        ok = core_csp_post_le(context->program->store, left, right);
        break;
    default:
        ok = false;
        break;
    }
    if (!ok) context->failed = true;
    return ok;
}

bool core_logic_constrain_pairs(core_logic_program_t *program,
                                core_datalog_relation_id_t relation,
                                core_csp_con_kind_t constraint)
{
    if (!program || !program->datalog_ready || !program->store) return false;
    core_option_t(int32_t) arity
        = core_datalog_relation_arity(program->datalog, relation);
    if (!arity.has_value || arity.value != 2) return false;
    bridge_pairs_context_t context = {
        .program = program, .constraint = constraint};
    core_datalog_query(program->datalog, relation, bridge_pair_from_tuple,
                       &context);
    return !context.failed;
}

#define LOGIC_POST(name, call)                                                   \
    bool name(core_logic_program_t *program, core_csp_var_id_t left,            \
              core_csp_var_id_t right)                                           \
    {                                                                            \
        return ensure_store(program) && call;                                   \
    }

LOGIC_POST(core_logic_csp_eq,
           core_csp_post_eq(program->store, left, right))
LOGIC_POST(core_logic_csp_ne,
           core_csp_post_ne(program->store, left, right))
LOGIC_POST(core_logic_csp_lt,
           core_csp_post_lt(program->store, left, right))
LOGIC_POST(core_logic_csp_le,
           core_csp_post_le(program->store, left, right))

bool core_logic_csp_eq_const(core_logic_program_t *program,
                             core_csp_var_id_t variable, int64_t value)
{
    return ensure_store(program)
        && core_csp_post_eq_const(program->store, variable, value);
}

bool core_logic_csp_in(core_logic_program_t *program,
                       core_csp_var_id_t variable, core_csp_domain_t domain)
{
    if (!ensure_store(program)) {
        core_csp_dom_free(&domain);
        return false;
    }
    return core_csp_post_in(program->store, variable, domain);
}

bool core_logic_csp_alldiff(core_logic_program_t *program,
                            const core_csp_var_id_t *variables, int32_t count)
{
    return ensure_store(program)
        && core_csp_post_alldiff(program->store, variables, count);
}

bool core_logic_csp_linear(core_logic_program_t *program,
                           const core_csp_var_id_t *variables,
                           const int64_t *coefficients, int32_t count,
                           int64_t rhs)
{
    return ensure_store(program)
        && core_csp_post_linear(program->store, variables, coefficients,
                                count, rhs);
}

void core_logic_csp_push(core_logic_program_t *program)
{
    if (ensure_store(program)) core_csp_push_state(program->store);
}

void core_logic_csp_pop(core_logic_program_t *program)
{
    if (program && program->store) core_csp_pop_state(program->store);
}

bool core_logic_run_datalog(core_logic_program_t *program)
{
    if (!program || !core_datalog_run(program->datalog)) return false;
    program->datalog_ready = true;
    return true;
}

bool core_logic_run_csp(core_logic_program_t *program)
{
    return program && (!program->store || core_csp_propagate(program->store));
}

bool core_logic_run(core_logic_program_t *program)
{
    return core_logic_run_datalog(program) && core_logic_run_csp(program);
}

bool core_logic_solve(core_logic_program_t *program)
{
    return core_logic_run(program)
        && (!program->store || core_csp_label(program->store));
}

typedef struct {
    core_logic_program_t *program;
    core_logic_solution_fn callback;
    void *context;
} solve_all_context_t;

static bool solve_all_adapter(core_csp_store_t *store, void *opaque)
{
    (void)store;
    solve_all_context_t *context = opaque;
    return !context->callback
        || context->callback(context->program, context->context);
}

int64_t core_logic_solve_all(core_logic_program_t *program,
                             core_logic_solution_fn callback, void *context)
{
    if (!core_logic_run(program)) return 0;
    if (!program->store) return 1;
    solve_all_context_t adapter = {program, callback, context};
    return core_csp_label_all(program->store, solve_all_adapter, &adapter);
}

core_csp_domain_result_t
core_logic_csp_domain(core_logic_program_t *program, core_csp_var_id_t var)
{
    return program && program->store
        ? core_csp_var_domain(program->store, var)
        : core_result_err(core_csp_domain_result_t, EINVAL);
}

core_csp_bool_result_t
core_logic_csp_is_ground(core_logic_program_t *program,
                         core_csp_var_id_t var)
{
    return program && program->store
        ? core_csp_var_is_ground(program->store, var)
        : core_result_err(core_csp_bool_result_t, EINVAL);
}

core_csp_int_result_t
core_logic_csp_value(core_logic_program_t *program, core_csp_var_id_t var)
{
    return program && program->store
        ? core_csp_var_value(program->store, var)
        : core_result_err(core_csp_int_result_t, EINVAL);
}
