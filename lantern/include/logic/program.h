#pragma once

#include "logic/datalog.h"
#include "logic/clpfd_label.h"

typedef struct core_logic_program core_logic_program_t;
typedef bool (*core_logic_solution_fn)(core_logic_program_t *program,
                                       void *context);

core_logic_program_t *core_logic_program_new(void);
void core_logic_program_free(core_logic_program_t *program);

core_datalog_engine_t *core_logic_datalog(core_logic_program_t *program);

/* Domain ownership transfers to the program, including when an existing
 * named variable makes the new domain redundant. */
core_csp_var_id_t core_logic_csp_var(core_logic_program_t *program,
                                     core_string_t name,
                                     core_csp_domain_t domain);
core_option_t(core_csp_var_id_t)
core_logic_csp_find(core_logic_program_t *program,
                    core_datalog_symbol_t symbol);

int32_t core_logic_vars_from_relation(core_logic_program_t *program,
                                      core_datalog_relation_id_t relation,
                                      int32_t column,
                                      const core_csp_domain_t *domain);
bool core_logic_constrain_pairs(core_logic_program_t *program,
                                core_datalog_relation_id_t relation,
                                core_csp_con_kind_t constraint);

bool core_logic_csp_eq(core_logic_program_t *program, core_csp_var_id_t left,
                       core_csp_var_id_t right);
bool core_logic_csp_ne(core_logic_program_t *program, core_csp_var_id_t left,
                       core_csp_var_id_t right);
bool core_logic_csp_lt(core_logic_program_t *program, core_csp_var_id_t left,
                       core_csp_var_id_t right);
bool core_logic_csp_le(core_logic_program_t *program, core_csp_var_id_t left,
                       core_csp_var_id_t right);
bool core_logic_csp_eq_const(core_logic_program_t *program,
                             core_csp_var_id_t variable, int64_t value);
bool core_logic_csp_in(core_logic_program_t *program,
                       core_csp_var_id_t variable,
                       core_csp_domain_t domain);
bool core_logic_csp_alldiff(core_logic_program_t *program,
                            const core_csp_var_id_t *variables, int32_t count);
bool core_logic_csp_linear(core_logic_program_t *program,
                           const core_csp_var_id_t *variables,
                           const int64_t *coefficients, int32_t count,
                           int64_t rhs);
void core_logic_csp_push(core_logic_program_t *program);
void core_logic_csp_pop(core_logic_program_t *program);

bool core_logic_run_datalog(core_logic_program_t *program);
bool core_logic_run_csp(core_logic_program_t *program);
bool core_logic_run(core_logic_program_t *program);
bool core_logic_solve(core_logic_program_t *program);
int64_t core_logic_solve_all(core_logic_program_t *program,
                             core_logic_solution_fn callback,
                             void *context);

core_csp_domain_result_t
core_logic_csp_domain(core_logic_program_t *program, core_csp_var_id_t var);
core_csp_bool_result_t
core_logic_csp_is_ground(core_logic_program_t *program,
                         core_csp_var_id_t var);
core_csp_int_result_t
core_logic_csp_value(core_logic_program_t *program, core_csp_var_id_t var);
