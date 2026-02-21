# n00b Logic Subsystem

## Overview

The n00b logic subsystem combines two reasoning engines into a unified
pipeline:

1. **Datalog** -- a bottom-up relational query language.  You
   define facts and rules; the engine computes all derivable tuples
   via semi-naive fixpoint evaluation.
2. **CSP (Constraint Satisfaction)** -- a finite-domain constraint
   solver.  You declare variables with integer domains and post
   constraints between them; the solver narrows domains via AC-3
   propagation and finds assignments via backtracking search.

A **bridge** layer connects the two: Datalog computes relational
structure (e.g., which nodes are adjacent in a graph), and the bridge
creates CSP variables from those relations so you can solve assignments
over them (e.g., graph coloring).

There is also a **DSL** compiler that accepts a Prolog-like text format
and compiles it into the same pipeline.

### Headers

￼
---

## Quick start: graph coloring

Color the nodes of a triangle with 3 colors so that no two adjacent
nodes share a color:

```c
#include "logic/logic_program.h"

auto prog = n00b_logic_new();

// Define the graph as Datalog facts.
n00b_logic_fact(prog, "edge", "a", "b");
n00b_logic_fact(prog, "edge", "b", "c");
n00b_logic_fact(prog, "edge", "a", "c");

// Bridge: create CSP variables {1,2,3} for each node,
//         post != for each edge.
n00b_logic_bridge(prog, "edge",
                   .domain     = n00b_csp_dom_range(1, 3),
                   .constraint = N00B_CSP_CON_NE);

// Solve (Datalog + propagation + labeling).
n00b_logic_solve(prog);

// Read results.
int64_t ca = n00b_result_get(n00b_logic_get_int(prog, "a"));
int64_t cb = n00b_result_get(n00b_logic_get_int(prog, "b"));
int64_t cc = n00b_result_get(n00b_logic_get_int(prog, "c"));

n00b_logic_free(prog);
```

---

## Concepts

### Datalog

Datalog is a declarative query language based on first-order logic.
A Datalog program consists of:

- **Facts** &mdash; ground tuples: `edge(a, b).`
- **Rules** &mdash; logical implications: `path(X, Y) :- edge(X, Y).`
- **Relations** &mdash; named tuple stores with a fixed arity.
- **Constants** &mdash; lowercase atoms (`a`, `b`, `node1`) or integers.
- **Variables** &mdash; uppercase names (`X`, `Y`, `Node`) that range
  over all constants.

The engine evaluates rules bottom-up using **semi-naive evaluation**:
each iteration only considers newly derived facts, avoiding redundant
work.  It handles **stratified negation** (rules with `not` in the
body) by splitting the program into strata and evaluating them in
dependency order.

### CSP (Constraint Satisfaction Problem)

The CSP solver works with:

- **Variables** &mdash; named unknowns, each with an integer domain.
- **Domains** &mdash; finite sets of integer values.  Represented as
  intervals (`n00b_csp_dom_range(1, 10)`), bitsets (up to 64 values),
  or sorted sparse arrays.
- **Constraints** &mdash; relations between variables:

  | Constraint | Meaning |
  |------------|---------|
  | `N00B_CSP_CON_EQ` | X = Y |
  | `N00B_CSP_CON_NE` | X != Y |
  | `N00B_CSP_CON_LT` | X < Y |
  | `N00B_CSP_CON_LE` | X <= Y |
  | `N00B_CSP_CON_EQ_CONST` | X = c |
  | `N00B_CSP_CON_IN` | X in D |
  | `N00B_CSP_CON_ALLDIFF` | all different |
  | `N00B_CSP_CON_LINEAR` | a1*X1 + ... = c |

**Propagation** (AC-3) narrows domains by removing values that cannot
satisfy any constraint.  **Labeling** performs backtracking search
(MRV heuristic) to find complete assignments.

### The bridge

The bridge connects Datalog output to CSP input:

1. Run Datalog to compute relations.
2. For each column of a relation, create one CSP variable per distinct
   symbol, with a specified domain.
3. For each tuple in a binary relation, post a constraint between the
   corresponding CSP variables.

The ergonomic `n00b_logic_bridge()` function does all three steps in
one call.

---

## The C API

### Lifecycle

```c
// Heap-allocated (recommended).
n00b_logic_t *prog = n00b_logic_new();
// ... use prog ...
n00b_logic_free(prog);   // frees the pointer too.

// Stack-allocated.
n00b_logic_t prog;
n00b_logic_init(&prog);
// ... use &prog ...
n00b_logic_free(&prog);  // does NOT free the struct.
```

### Adding facts

The ergonomic `n00b_logic_fact()` uses ncc's variadic `+` extension
to infer arity and auto-intern symbols:

```c
n00b_logic_fact(prog, "edge", "a", "b");         // arity 2
n00b_logic_fact(prog, "triple", "x", "y", "z");  // arity 3
n00b_logic_fact(prog, "node", "a");               // arity 1
```

For the low-level API, intern symbols and add facts manually:

```c
n00b_dl_rel_id_t edge = n00b_logic_relation(prog, "edge", 2);
n00b_dl_sym_t a = n00b_logic_const(prog, "a");
n00b_dl_sym_t b = n00b_logic_const(prog, "b");
n00b_logic_add_fact(prog, edge, 2, (n00b_dl_sym_t[]){a, b});
```

### Adding rules

Rules use the low-level builder API:

```c
n00b_dl_rel_id_t edge = n00b_logic_relation(prog, "edge", 2);
n00b_dl_rel_id_t path = n00b_logic_relation(prog, "path", 2);
n00b_dl_sym_t X = n00b_logic_var(prog, "X");
n00b_dl_sym_t Y = n00b_logic_var(prog, "Y");
n00b_dl_sym_t Z = n00b_logic_var(prog, "Z");

// path(X,Y) :- edge(X,Y).
n00b_dl_rule_builder_t rb;
n00b_dl_rule_builder_init(&rb);
n00b_dl_rule_builder_head(&rb, path, 2, (n00b_dl_sym_t[]){X, Y});
n00b_dl_rule_builder_add(&rb, edge, 2, (n00b_dl_sym_t[]){X, Y}, false);
n00b_logic_add_rule(prog, n00b_dl_rule_builder_finish(&rb));

// path(X,Y) :- path(X,Z), edge(Z,Y).
n00b_dl_rule_builder_init(&rb);
n00b_dl_rule_builder_head(&rb, path, 2, (n00b_dl_sym_t[]){X, Y});
n00b_dl_rule_builder_add(&rb, path, 2, (n00b_dl_sym_t[]){X, Z}, false);
n00b_dl_rule_builder_add(&rb, edge, 2, (n00b_dl_sym_t[]){Z, Y}, false);
n00b_logic_add_rule(prog, n00b_dl_rule_builder_finish(&rb));
```

### Bridging (Datalog -> CSP)

The ergonomic `n00b_logic_bridge()` uses `_kargs` for optional
parameters:

```c
// Bridge all columns, post NE constraint per tuple.
n00b_logic_bridge(prog, "edge",
                   .domain     = n00b_csp_dom_range(1, 3),
                   .constraint = N00B_CSP_CON_NE);
```

This auto-runs Datalog if needed, creates CSP variables for every
distinct symbol in every column of the named relation, and posts the
constraint for every tuple pair.

For fine-grained control, use the low-level functions:

```c
n00b_logic_run_datalog(prog);
n00b_logic_vars_from_rel(prog, edge_id, 0, n00b_csp_dom_range(1, 3));
n00b_logic_vars_from_rel(prog, edge_id, 1, n00b_csp_dom_range(1, 3));
n00b_logic_constrain_pairs(prog, edge_id, N00B_CSP_CON_NE);
```

### Standalone CSP variables

Create named CSP variables without Datalog:

```c
// Shorthand: range domain.
n00b_csp_var_id_t x = n00b_logic_int_var(prog, "x", 1, 10);
n00b_csp_var_id_t y = n00b_logic_int_var(prog, "y", 1, 10);

// Full control: arbitrary domain.
n00b_csp_var_id_t z = n00b_logic_csp_var(prog, "z",
                                           n00b_csp_dom_range(1, 100));
```

### Constraints

Post constraints between named variables:

```c
n00b_logic_constrain(prog, "x", "y", N00B_CSP_CON_NE);
n00b_logic_constrain(prog, "x", "z", N00B_CSP_CON_LT);
```

Or use variable IDs directly:

```c
n00b_logic_csp_ne(prog, x, y);
n00b_logic_csp_lt(prog, x, z);
n00b_logic_csp_eq_const(prog, x, 42);
```

### Solving

```c
// Find the first solution.
bool ok = n00b_logic_solve(prog);

// Enumerate all solutions.
int64_t count = n00b_logic_solve_all(prog, my_callback, my_ctx);
```

`n00b_logic_solve()` runs the full pipeline: Datalog evaluation,
CSP propagation, and backtracking labeling.  On success, all CSP
variables are ground (single-valued).

`n00b_logic_solve_all()` calls a callback for each solution.  The
store is restored to its pre-solve state when done.

### Reading results

The ergonomic `n00b_logic_get_int()` looks up a variable by name and
returns its value:

```c
n00b_result_t(int64_t) r = n00b_logic_get_int(prog, "x");
if (n00b_result_is_ok(r)) {
    printf("x = %ld\n", n00b_result_get(r));
}
```

Error codes:
- `ENOENT` &mdash; variable not found.
- `EINVAL` &mdash; variable exists but is not ground.

For the low-level API, look up by symbol and variable ID:

```c
n00b_option_t(n00b_csp_var_id_t) opt = n00b_logic_csp_find(prog, sym);
if (n00b_option_is_set(opt)) {
    n00b_result_t(int64_t) val = n00b_logic_csp_value(prog,
                                                        n00b_option_get(opt));
}
```

---

## The DSL

The text DSL (`logic/logic_dsl.h`) accepts a Prolog-like syntax and
compiles it into the same `n00b_logic_t` pipeline.

### Example

```prolog
% Graph coloring
edge(a, b).
edge(b, c).
edge(a, c).

color(Node) in 1..3 :- edge(Node, _).
color(Node) in 1..3 :- edge(_, Node).
color(X) != color(Y) :- edge(X, Y).

solve.
```

### Usage

```c
const char *src = "edge(a,b). edge(b,c). ...";
n00b_dsl_result_t r = n00b_dsl_compile_and_run(src, 0);
if (r.error) {
    fprintf(stderr, "Error at %d:%d: %s\n",
            r.error_line, r.error_col, r.error);
} else {
    // r.prog is ready; r.solved indicates if solve succeeded.
    n00b_result_t(int64_t) v = n00b_logic_get_int(r.prog, "a");
    // ...
}
n00b_dsl_result_free(&r);
```

### Syntax reference

￼
---

## Advanced usage

### When to use the low-level API

The ergonomic functions (`n00b_logic_fact`, `n00b_logic_bridge`, etc.)
cover the common case.  Drop down to the low-level API when you need:

- **Integer symbols.** `n00b_logic_int(prog, 42)` interns an integer
  as a Datalog symbol.  The ergonomic `n00b_logic_fact()` only handles
  string constants.
- **Per-column domains.** `n00b_logic_bridge()` applies the same
  domain to all columns.  Use `n00b_logic_vars_from_rel()` to give
  different domains to different columns.
- **Custom constraints.** `n00b_logic_constrain_pairs()` posts the
  same constraint type for every tuple.  For mixed constraints, iterate
  tuples with `n00b_logic_query()` and post constraints individually.
- **Choice-point management.** `n00b_logic_csp_push()` and
  `n00b_logic_csp_pop()` let you implement custom search strategies
  instead of using the built-in labeling.
- **Rule construction.** Rules always use the builder API
  (`n00b_dl_rule_builder_t`).

### Memory management

- `n00b_logic_new()` allocates with `calloc` and `n00b_logic_free()`
  calls `free()`.  Stack-allocated programs (`n00b_logic_init`) do not
  free the struct.
- Domains passed to `n00b_logic_vars_from_rel()` are consumed (freed
  internally).  Domains passed to `n00b_logic_bridge()` via `_kargs`
  are cloned per column and the original is freed automatically.
- The constraint store is heap-allocated and freed by
  `n00b_logic_free()`.

---

## Reference

￼