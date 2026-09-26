#include "logic/program.h"
#include "harness.h"

typedef struct {
  core_logic_program_t *program;
  core_datalog_symbol_t left;
  core_datalog_symbol_t right;
  int64_t count;
  bool values_are_distinct;
} solution_check_t;

static bool check_solution(core_logic_program_t *program, void *opaque) {
  solution_check_t *check = opaque;
  core_option_t(core_csp_var_id_t) left =
      core_logic_csp_find(program, check->left);
  core_option_t(core_csp_var_id_t) right =
      core_logic_csp_find(program, check->right);
  if (!left.has_value || !right.has_value)
    return false;
  core_csp_int_result_t left_value = core_logic_csp_value(program, left.value);
  core_csp_int_result_t right_value =
      core_logic_csp_value(program, right.value);
  check->values_are_distinct =
      check->values_are_distinct && left_value.is_ok && right_value.is_ok &&
      left_value.result.value != right_value.result.value;
  check->count++;
  return true;
}

static void test_datalog_to_csp_bridge_and_solution_restore(void) {
  core_logic_program_t *program = core_logic_program_new();
  core_datalog_engine_t *datalog = core_logic_datalog(program);
  core_datalog_relation_id_t edge =
      core_datalog_relation(datalog, CORE_STRING_STATIC("edge"), 2);
  core_datalog_symbol_t left =
      core_datalog_constant(datalog, CORE_STRING_STATIC("left"));
  core_datalog_symbol_t right =
      core_datalog_constant(datalog, CORE_STRING_STATIC("right"));
  core_datalog_symbol_t tuple[] = {left, right};
  core_datalog_add_fact(datalog, edge, 2, tuple);

  check("Datalog runs before the bridge", core_logic_run_datalog(program));
  core_csp_domain_t colors = core_csp_dom_range(1, 3);
  check("left relation column creates one CSP variable",
        core_logic_vars_from_relation(program, edge, 0, &colors) == 1);
  check("right relation column creates one CSP variable",
        core_logic_vars_from_relation(program, edge, 1, &colors) == 1);
  check("edge relation posts inequality",
        core_logic_constrain_pairs(program, edge, CORE_CSP_CON_NE));

  solution_check_t solutions = {
      .program = program,
      .left = left,
      .right = right,
      .values_are_distinct = true,
  };
  int64_t count = core_logic_solve_all(program, check_solution, &solutions);
  check("bridge enumerates the six unequal color pairs",
        count == 6 && solutions.count == 6 && solutions.values_are_distinct);
  core_option_t(core_csp_var_id_t) csp_left =
      core_logic_csp_find(program, left);
  check("enumeration restores the propagated store",
        csp_left.has_value && !core_result_get_ok(core_logic_csp_is_ground(
                                  program, csp_left.value)));
  core_logic_program_free(program);
}

static void test_csp_passthrough(void) {
  core_logic_program_t *program = core_logic_program_new();
  core_csp_var_id_t x = core_logic_csp_var(program, CORE_STRING_STATIC("x"),
                                           core_csp_dom_range(1, 3));
  core_csp_var_id_t y = core_logic_csp_var(program, CORE_STRING_STATIC("y"),
                                           core_csp_dom_range(1, 3));

  check("CSP variable creation succeeds", x >= 0 && y >= 0);
  check("inequality and constant constraints post",
        core_logic_csp_ne(program, x, y) &&
            core_logic_csp_eq_const(program, x, 2));
  check("CSP propagation succeeds", core_logic_run_csp(program));

  core_csp_int_result_t x_value = core_logic_csp_value(program, x);
  core_csp_domain_result_t y_domain = core_logic_csp_domain(program, y);
  check("fixed variable exposes its value",
        x_value.is_ok && x_value.result.value == 2);
  check("inequality removes the fixed value from the other domain",
        y_domain.is_ok && !core_csp_dom_contains(y_domain.result.value, 2));

  core_logic_program_free(program);
}

static void test_csp_push_pop_search(void) {
  core_logic_program_t *program = core_logic_program_new();
  core_csp_var_id_t x = core_logic_csp_var(program, CORE_STRING_STATIC("x"),
                                           core_csp_dom_range(1, 2));
  core_csp_var_id_t y = core_logic_csp_var(program, CORE_STRING_STATIC("y"),
                                           core_csp_dom_range(1, 2));
  check("search variables created", x >= 0 && y >= 0);
  check("base inequality posts", core_logic_csp_ne(program, x, y));

  core_logic_csp_push(program);
  check("first branch propagates",
        core_logic_csp_eq_const(program, x, 1) && core_logic_run_csp(program));
  core_csp_int_result_t first_y = core_logic_csp_value(program, y);
  check("first branch forces y to 2",
        first_y.is_ok && first_y.result.value == 2);
  core_logic_csp_pop(program);

  core_logic_csp_push(program);
  check("second branch propagates",
        core_logic_csp_eq_const(program, x, 2) && core_logic_run_csp(program));
  core_csp_int_result_t second_y = core_logic_csp_value(program, y);
  check("second branch forces y to 1",
        second_y.is_ok && second_y.result.value == 1);
  core_logic_csp_pop(program);

  core_csp_domain_result_t restored_x = core_logic_csp_domain(program, x);
  core_csp_domain_result_t restored_y = core_logic_csp_domain(program, y);
  check("popping both branches restores the original domains",
        restored_x.is_ok && restored_y.is_ok &&
            core_csp_dom_min(restored_x.result.value) == 1 &&
            core_csp_dom_max(restored_x.result.value) == 2 &&
            core_csp_dom_min(restored_y.result.value) == 1 &&
            core_csp_dom_max(restored_y.result.value) == 2);

  core_logic_program_free(program);
}

int main(void) {
  test_datalog_to_csp_bridge_and_solution_restore();
  test_csp_passthrough();
  test_csp_push_pop_search();
  return fails ? 1 : 0;
}
