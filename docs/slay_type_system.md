# Slay Type System — Design Document

Status: draft, for review.

## Goals

Slay's type system serves **grammar authors** building language tooling.
It must be:

1. **General-purpose.** Works for any language a slay grammar can parse —
   not just n00b or C. A grammar author defines types, inference rules,
   and constraints; slay provides the engine.

2. **Easy to use.** A grammar author who isn't a type theorist should be
   able to express "this expression's type is the element type of
   whatever container it indexes" without writing C code.

3. **Expressive enough for real languages.** Supports parametric
   polymorphism, sum types (tagged unions), pattern matching with
   exhaustiveness checking, ad-hoc polymorphism narrowing, option/result
   as first-class, and references.

4. **Gives good diagnostics.** Type errors should explain *why* two types
   don't unify, what constraints were violated, and (for pattern
   matching) exactly which variants aren't covered.

5. **Separable.** The type engine is a standalone library. It doesn't
   depend on slay's parser or grammar system. Slay's `@infer`
   annotation walk is a *client* of this library, not part of it.

## Non-goals (for now)

- Unifying with ncc's C type system or the runtime `type_info_t`
  registry. That's a later integration step.
- Dependent types, linear types, effect systems.
- Higher-kinded types (but the representation shouldn't preclude them).

---

## Part 1: Type Representation

### 1.1 Type kinds

Every type is a node in a union-find graph. Nodes carry a *kind*
discriminator and a kind-specific payload:

| Kind | C type | Payload | Examples |
|------|--------|---------|----------|
| **Var** | `n00b_tc_var_t` | id, given_name (option), display_name, constraint list | `` `T ``, `` `K `` |
| **Primitive** | `n00b_tc_prim_t` | interned name | `int`, `f64`, `bool`, `string`, `nil` |
| **Param** | `n00b_tc_param_t` | constructor name, param list | `` list[`T] ``, `dict[string, int]`, `ref[int]` |
| **Fn** | `n00b_tc_fn_t` | positional list, vargs type, kargs record type, return type | `(int, string, *int, **{.timeout: int}) -> bool` |
| **Sum** | `n00b_tc_sum_t` | variant list | `int \| string \| nil` |
| **Record** | `n00b_tc_record_t` | name, type params, field names+types, ordered flag | `Point{x: int, y: int}`, `` Pair[`A, `B]{fst: `A, snd: `B} `` |
| **Tuple** | `n00b_tc_tuple_t` | element list, min_len, open flag | `(int, string)`, `(int, string, ...)` |

Seven kinds total. Note what's **not** a separate kind:

- **Ref** is `Param("ref", [T])` — just a parameterized type like
  `list` or `maybe`. No special kind needed.
- **Maybe/option** is `Param("maybe", [T])`. The type `int | nil` is
  a sum, not a maybe — they're semantically equivalent but
  structurally different. The engine should normalize between them
  (see below).
- **Result** is `Param("result", [T, E])`.

Slop had `TC_KIND_REF` as its own kind. We drop it. If `ref` needs
special unification behavior (e.g., invariant rather than covariant),
that's handled via variance annotations on the parameterized type
(see Open Questions), not a separate kind.

### Maybe / option normalization

`maybe[int]` and `int | nil` should be interchangeable. The engine
normalizes: when constructing `maybe[T]`, it's stored as
`Param("maybe", [T])`. When unifying `maybe[T]` with a sum that
contains `nil`, the engine treats them as compatible — `maybe[int]`
unifies with `int | nil` by unifying `int` with `int` and
recognizing that `nil` is the "nothing" case.

This is a special-case unification rule, not a structural identity.
Grammar authors can rely on either form.

### 1.2 C representation

The core type node is a struct with two fields: a union-find
`forward` pointer for type inference, and an `n00b_variant_t`
holding the kind-specific payload. The variant gives us O(1)
`typehash`-based kind discrimination.

```c
// Forward declare — kind payloads reference n00b_tc_type_t *.
typedef struct n00b_tc_type_s n00b_tc_type_t;

// --- Kind payloads (one struct per kind) ---

typedef struct {
    uint32_t                             id;           // Unique within a context.
    n00b_option_t(n00b_string_t)         given_name;   // User's name (if any).
    n00b_string_t                        display_name; // Shown in errors ("T", "t_42").
    n00b_list_t(n00b_tc_constraint_t)   *constraints;
} n00b_tc_var_t;

typedef struct {
    n00b_string_t  name;   // Interned: "int", "f64", "bool", "nil", ...
} n00b_tc_prim_t;

typedef struct {
    n00b_string_t                    name;    // "list", "dict", "ref", "maybe", ...
    n00b_list_t(n00b_tc_type_t *)   *params;
} n00b_tc_param_t;

typedef struct {
    n00b_list_t(n00b_tc_type_t *)   *positional;
    n00b_tc_type_t                  *vargs_type;   // NULL if not variadic.
    n00b_tc_type_t                  *kargs_type;   // NULL, or Record (ordered=false).
    n00b_tc_type_t                  *return_type;
} n00b_tc_fn_t;

typedef struct {
    n00b_list_t(n00b_tc_type_t *)   *variants;     // Sorted, flat.
} n00b_tc_sum_t;

// Field descriptor — used to build records via n00b_tc_record().
typedef struct {
    n00b_string_t    name;
    n00b_tc_type_t  *type;
    bool             has_default;   // True = caller may omit (keyword args).
} n00b_tc_field_t;

typedef struct {
    n00b_string_t                    name;
    n00b_list_t(n00b_tc_type_t *)   *type_params;  // May be NULL.
    n00b_list_t(n00b_string_t)      *field_names;
    n00b_list_t(n00b_tc_type_t *)   *field_types;
    n00b_list_t(bool)               *field_has_default; // Per-field; NULL = none have defaults.
    bool                             open;         // Open row (duck typing).
    bool                             ordered;      // true = struct layout; false = keyword bag.
} n00b_tc_record_t;

typedef struct {
    n00b_list_t(n00b_tc_type_t *)   *elements;
    uint16_t                         min_len;      // Minimum tuple length.
    bool                             open;         // Open row (at least N elements).
} n00b_tc_tuple_t;

// --- The type node: union-find link + variant payload ---

typedef n00b_variant_decl(
    n00b_tc_var_t,
    n00b_tc_prim_t,
    n00b_tc_param_t,
    n00b_tc_fn_t,
    n00b_tc_sum_t,
    n00b_tc_record_t,
    n00b_tc_tuple_t
) n00b_tc_kind_t;

struct n00b_tc_type_s {
    n00b_tc_type_t  *forward;   // Union-find link (NULL = root).
    n00b_tc_kind_t   kind;      // Which kind + kind-specific data.
};
```

Usage:

```c
// Check what kind a type is:
n00b_variant_is_type(t->kind, n00b_tc_var_t)
n00b_variant_is_type(t->kind, n00b_tc_fn_t)

// Extract the kind payload:
auto var  = n00b_variant_get(t->kind, n00b_tc_var_t);
auto fn   = n00b_variant_get(t->kind, n00b_tc_fn_t);
auto rec  = n00b_variant_get(t->kind, n00b_tc_record_t);
```

**Why `n00b_variant_t`?** The `typehash`-based selector gives O(1)
kind checks with no enum boilerplate. Adding a new kind is just
adding a struct to the variant decl.

**Why `n00b_list_t` everywhere?** Push/pop, indexed access,
`n00b_list_foreach` iteration, and optional thread safety. Not
worth trading for raw arrays + manual length tracking in a
type-checking library.

### 1.3 Why not hardcoded container kinds?

Old n00b had separate `list`, `dict`, `set`, `tuple` kinds. Slop
unified these under `Param` — a `list[int]` is just
`Param("list", [int])`. This is the right call:

- Grammar authors can define their own parameterized types without
  touching the type engine (`matrix['T]`, `stream['T]`, `channel['T]`).
- Unification of parameterized types is one code path, not N.
- The constraint system works uniformly on all parameterized types.

### 1.4 Sum types

Sum types represent "one of these types." Written with `|`:

```
int | string | nil
Circle{r: float} | Rect{w: float, h: float} | Triangle{b: float, h: float}
list[`T] | dict[`K, `V] | set[`T]
```

Parentheses are optional for grouping: `(int | string)` is the same
as `int | string`.

Sums are **not** parameterized types. `list[int]` has a named
constructor ("list") with fixed arity (1 param). A sum is an open
set of alternatives with no constructor name. They're structurally
different and unify differently:

- **Param + Param:** constructor names must match, arities must
  match, params unify pairwise.
- **Sum + Sum:** variant counts must match, sorted variants unify
  pairwise.
- **Sum + non-Sum:** injection — the non-sum must unify with one
  of the variants.

Sums are central to:

- **Maybe/option types:** `int | nil` means "int or nothing." The
  parameterized `maybe[int]` is sugar for this.
- **Tagged unions / variants:** Named record variants compose with
  sums naturally: `Circle{r: float} | Rect{w: float, h: float}`.
- **Ad-hoc polymorphism narrowing:** When inference knows a value is
  `list[`T] | dict[`K, `V] | set[`T]` and you index it with a string
  key, the set drops out.
- **Exhaustiveness checking:** The type engine can enumerate variants,
  subtract matched ones, and report what's missing.

Sum types are **flat** (no nesting: `a | (b | c)` normalizes to
`a | b | c`) and **sorted** for canonical comparison during
unification.

### 1.5 Records

Records are structural types with named fields. They serve as:

- Struct/class bodies (fields discovered by the annotation walk's
  `@field` processing).
- Enum variant payloads (each variant of a sum can be a record).
- Function keyword argument bundles (see §1.8).

Records have an **ordered** flag that controls unification semantics:

- **Ordered** (`ordered = true`): field position matters. Structs,
  C-like layouts, positional destructuring. Two ordered records
  unify only if fields match by both name and position.
- **Unordered** (`ordered = false`): only field names matter, not
  their order. Keyword argument bags, config objects, named
  parameter bundles. Two unordered records unify by matching fields
  by name regardless of the order they were declared in.
- **Ordered + unordered** is a unification error — you can't unify
  a struct (where layout matters) with a keyword bag (where it
  doesn't).

Records can be **parameterized** — the record carries type parameters
that appear in its field types:

```
Pair['A, 'B]{fst: 'A, snd: 'B}
Node['T]{value: 'T, children: list[Node['T]]}
```

When you instantiate `Pair[int, string]`, the fields become
`fst: int, snd: string`. Parameterized records unify by matching
names, unifying type parameters, then checking that field types
are consistent with the parameter bindings.

Records may also carry **per-field `has_default`** metadata (see
`field_has_default` in §1.2). This is used for keyword argument
records — a field with a default may be omitted by the caller.
The type system doesn't store default *values*, only whether one
exists. Values are a codegen concern.

The C representation is `n00b_tc_record_t` (see §1.2). Records unify
by name and field-by-field. If both records have type parameters,
those are unified first, and field types are checked for consistency
with the resulting bindings.

### 1.6 Tuple types (product types)

Tuples are ordered, positional product types — like records but with
indexed elements instead of named fields:

```
(int, string)                  # fixed 2-tuple
(int, string, bool)            # fixed 3-tuple
(int, string, ...)             # open tuple: at least 2 elements
(`A, `B)                       # polymorphic tuple
```

**Open tuples** support row polymorphism on positional elements.
An open tuple `(int, string, ...)` means "at least two elements,
the first is int, the second is string, there may be more." This
is essential for expressing constraints like "this is a tuple with
at least 4 elements":

```
($x is `T and `T has min_len 4)
```

Or in the rule syntax:

```
multi_return:
  $result is (`A, `B, `C, `D, ...)
```

The C representation:

```c
typedef struct {
    n00b_list_t(n00b_tc_type_t *) *elements;  // Known element types.
    uint16_t                       min_len;   // Minimum tuple length.
    bool                           open;      // True = open (may have more elements).
} n00b_tc_tuple_t;
```

**Unification rules:**
- **Closed + Closed:** lengths must match, elements unify pairwise.
- **Open + Closed:** closed length must be >= open's min_len. Known
  elements unify pairwise; extra elements in the closed tuple are
  unconstrained.
- **Open + Open:** the resulting open tuple has the longer known
  prefix and the max of both min_lens.
- **Tuple + non-Tuple:** never unifies (tuples are not lists).

Tuples are distinct from Records (no field names, positional access)
and from Param types like `list[T]` (fixed or minimum arity, not
homogeneous).

### 1.7 Row polymorphism: open vs closed

Records and tuples share the same design: **definitions are closed,
constraints are open.**

**Closed** (definition site): "this type has *exactly* these
members."
- A record definition `Point{x: int, y: int}` has exactly two
  fields.
- A function's kargs record `{.timeout: int, .verbose: bool}` has
  exactly those two keywords (callers may omit ones with defaults,
  but can't add unknown ones).
- A tuple `(int, string)` has exactly two elements.

**Open** (constraint/call site): "this type has *at least* these
members."
- A duck-typing constraint `` `T has .x: int and `T has .y: int ``
  is open — `` `T `` might have more fields.
- A call site's kargs record is open — it only mentions the
  keywords actually passed, which is a subset of the callee's
  accepted keywords (the rest have defaults).
- A tuple constraint `(int, string, ...)` requires at least two
  elements.

This is **row polymorphism** — the "row" (of fields or elements)
may have an unspecified tail.

Each type that supports rows has an `open` flag (see §1.2):
- `n00b_tc_record_t.open` — open record/kargs row
- `n00b_tc_tuple_t.open` — open tuple row

**Unification rules for open/closed:**
- **Closed + Closed:** members must match exactly.
- **Open + Closed:** every member in the open row must appear in the
  closed type. Extra members in the closed type are fine — but for
  unordered records (kargs), extra closed-side fields that the open
  side omits must have `has_default = true`, or it's a missing
  required keyword error.
- **Open + Open:** take the union of known members; result is still
  open.

This gives us Pythonic duck typing that's still statically safe:
a function accepting `` `T:HasX,HasY `` will match any record with
at least `.x` and `.y` fields, regardless of what else it has.

### 1.8 Function types

Function types carry up to four components:

```
(positional..., *vargs_type, **kargs_record) -> return_type
```

**Positional parameters** are ordered and matched by position during
unification. Two function types unify only if their positional
parameter counts match and each pair unifies.

**Variadic parameter** (`*vargs_type`) is an optional type. If present,
the function accepts additional positional arguments beyond the fixed
ones, all of which must unify with the vargs type. Internally it's
just a `n00b_tc_type_t *` (possibly a Var).

**Keyword arguments** (`**kargs_record`) are an unordered record —
a `n00b_tc_type_t *` that must be a Record with `ordered = false`.
The record's fields are the accepted keywords; each field's
`has_default` flag indicates whether the caller may omit it.

This means keywords aren't a special concept in the type system —
they're just a record that happens to be unordered. The `**` syntax
in the user-facing display hints at this, but internally it's the
same Record kind used for structs, just with `ordered = false`.

**Unification of function kargs:** When unifying two function types,
their `kargs_type` fields unify as records. Since both are unordered
records, fields are matched by name. The standard record unification
rules apply (§1.7):

```
unify_fn_kargs(ctx, callee_fn, caller_fn):
    ca = callee_fn.kargs_type
    cr = caller_fn.kargs_type
    if ca == NULL and cr == NULL: ok.
    if ca != NULL and cr == NULL:
        // Caller passed no keywords. Every callee keyword
        // must have a default, or it's an error.
        for field in ca.fields:
            if !field.has_default: return err(MISSING_KEYWORD)
        ok.
    if ca == NULL and cr != NULL:
        // Callee accepts no keywords but caller passed some.
        return err(UNKNOWN_KEYWORD)
    else:
        unify(ctx, ca, cr)  // Record unification handles the rest.
```

At a call site, the inference engine constructs an **open** kargs
record from the keywords actually passed. This open record unifies
against the callee's **closed** kargs record. The open-vs-closed
rules (§1.7) ensure:
- Every passed keyword must exist in the callee's record (or it's
  an error).
- Every callee keyword without a default must appear in the caller's
  record (or it's an error: missing required keyword).
- Callee keywords with defaults that the caller omits are fine —
  the caller's open record simply doesn't mention them.

The C representation is `n00b_tc_fn_t` (see §1.2). The `kargs_type`
field is NULL for functions that take no keywords, or points to a
Record type node.

**Display format examples:**

```
(int, string) -> bool                       // positional only
(int, string, *int) -> void                 // positional + vargs
(int, **{.timeout: int, .verbose: bool}) -> void  // positional + kargs
(*string, **{.sep: string}) -> string       // vargs + kargs
```

**Why this matters for inference:** When the inference engine sees a
call site like `f(1, "hello", .timeout = 5)`, it constructs a
function type `(int, string, **{.timeout: int}) -> 'R` where the
kargs record is open (the caller might not be passing all keywords).
This unifies against `f`'s declared type, matching the `.timeout`
field by name against the callee's closed kargs record.

### 1.9 Type variable syntax

Type variables use the **backtick prefix** from n00b 1: `` `T ``,
`` `K ``, `` `V ``, `` `Container ``. This is unambiguous — no
primitive or constructor name starts with a backtick.

### 1.10 Type variables and the union-find

Type variables are the core of inference. A fresh variable `` `T ``
can unify with any type, subject to its constraints. Unification
sets a `forward` pointer (union-find link) from the variable to the
resolved type. Path compression keeps lookups fast.

Variables are created:
- Explicitly by the grammar author (`` `T `` in an annotation or
  type expression).
- Implicitly by the inference engine (one per annotated parse tree
  node).

---

## Part 2: Constraints on Type Variables

This is where n00b 2 goes beyond slop. Slop had three constraint
kinds; we need a richer, composable system.

### 2.1 Constraint kinds

| Constraint | Meaning | Example |
|------------|---------|---------|
| **Unifies** | Must unify with a specific type | `'T unifies int` |
| **OneOf** | Must unify with one of a set | `'T one_of {list, dict, set}` |
| **Implements** | Must implement a named interface | `'T implements Indexable` |
| **HasField** | Must be a record with a named field of a given type | `'T has_field(x, int)` |
| **HasParam** | Parameterized type whose Nth param unifies with a type | `'T has_param(0, string)` |
| **Promotes** | Must be promotable to a target type | `'T promotes_to f64` |
| **Not** | Must NOT unify with a type | `'T not nil` |

The first three are slop's originals. **HasField**, **HasParam**, and
**Not** are new.

#### C representation

```c
typedef enum {
    N00B_TC_CON_UNIFIES,
    N00B_TC_CON_ONE_OF,
    N00B_TC_CON_IMPLEMENTS,
    N00B_TC_CON_HAS_FIELD,
    N00B_TC_CON_HAS_PARAM,
    N00B_TC_CON_PROMOTES,
    N00B_TC_CON_NOT,
} n00b_tc_con_kind_t;

typedef struct {
    n00b_tc_con_kind_t  kind;
    union {
        struct {                                    // UNIFIES
            n00b_tc_type_t *target;
        } unifies;
        struct {                                    // ONE_OF
            n00b_list_t(n00b_tc_type_t *) *types;
        } one_of;
        struct {                                    // IMPLEMENTS
            n00b_string_t iface_name;
        } implements;
        struct {                                    // HAS_FIELD
            n00b_string_t   field_name;
            n00b_tc_type_t *field_type;             // Unifies with the field's type.
        } has_field;
        struct {                                    // HAS_PARAM
            int32_t         index;
            n00b_tc_type_t *param_type;             // Unifies with param[index].
        } has_param;
        struct {                                    // PROMOTES
            n00b_tc_type_t *target;                 // Must promote to this type.
        } promotes;
        struct {                                    // NOT
            n00b_tc_type_t *excluded;               // Must NOT unify with this type.
        } not_;
    };
} n00b_tc_constraint_t;
```

An enum + union is the right choice here (not a variant) because
constraint kinds are a closed set — grammar authors don't define
new constraint kinds, only new interfaces and types.

### 2.2 HasParam — the key to ad-hoc polymorphism

The motivating example: `x["blah"] = 100`. What do we know about `x`?

- It supports indexing (some operator `[]`).
- The key type unifies with `string`.
- The value type unifies with `int`.

With `HasParam`, the grammar author writes constraints on the
container's *structural shape* without knowing the concrete type:

```
'Container has_param(0, string)   // key type is string
'Container has_param(1, int)      // value type is int
```

If `'Container` starts as `list['T] | dict['K, 'V] | set['T]`:
- `has_param(0, string)` eliminates `list` and `set` (their param 0
  is the element type, not a key type — unless the grammar defines
  indexing differently).
- What remains: `dict[string, int]`.

This is more general than hardcoded type functions like `key_of()` or
`element_of()`. Those are convenience sugar that desugar to
`HasParam`:

```
element_of('C)  →  let 'T where 'C has_param(0, 'T)  →  'T
key_of('C)      →  let 'K where 'C has_param(0, 'K)  →  'K
value_of('C)    →  let 'V where 'C has_param(1, 'V)  →  'V
```

### 2.3 HasField — structural record access

For record field access (`point.x`), grammar authors need:

```
'R has_field("x", 'T)
```

This constrains `'R` to be a record type with a field named `"x"`
whose type unifies with `'T`.

### 2.4 Not — negative constraints

`'T not nil` means "this type variable cannot resolve to nil." This
is useful for:

- Post-null-check narrowing: after `if x != nil`, x's type in the
  true branch drops `nil` from any sum.
- Exhaustiveness: after matching variants A, B, C of a sum, the
  remaining type must be `not A, not B, not C` — if that's empty,
  the match is exhaustive.

### 2.5 Constraint checking

Constraints are checked at **binding time** — when a variable's
`forward` pointer is set during unification. If the concrete type
violates any constraint, unification fails with a diagnostic that
names the constraint.

For **OneOf** and **Not**, binding triggers set membership / exclusion
checks. For **HasParam** and **HasField**, binding triggers structural
inspection of the resolved type.

### 2.6 Interfaces

Interfaces are named capability sets registered on the type context.

#### C representation

```c
/**
 * @brief A named parameter of an interface.
 *
 * E.g., Indexable has params "key" and "value".
 */
typedef struct {
    n00b_string_t    name;   // "key", "value", "element", ...
    n00b_tc_type_t  *type;   // Type variable for this param.
} n00b_tc_iface_param_t;

/**
 * @brief An interface definition.
 */
typedef struct {
    n00b_string_t                          name;    // "Indexable", "Numeric", ...
    n00b_list_t(n00b_tc_iface_param_t)    *params;  // Named type parameters.
} n00b_tc_iface_t;

/**
 * @brief An implementation binding: "dict implements Indexable with [...]".
 */
typedef struct {
    n00b_string_t                    type_name;   // "dict", "list", ...
    n00b_string_t                    iface_name;  // "Indexable", ...
    n00b_list_t(n00b_tc_type_t *)   *bindings;    // Concrete types for each iface param.
} n00b_tc_impl_t;
```

#### Registration API

```c
auto k = n00b_tc_var(ctx, r"K");
auto v = n00b_tc_var(ctx, r"V");

n00b_tc_register_interface(ctx, r"Indexable",
    n00b_tc_iface_param(r"key", k),
    n00b_tc_iface_param(r"value", v));

n00b_tc_impl(ctx, r"dict", r"Indexable", key_param, val_param);
```

When a constraint says `'T implements Indexable`, and `'T` resolves
to `dict[string, int]`, the engine:

1. Looks up `dict`'s `Indexable` implementation.
2. Instantiates the interface params with the concrete type's params.
3. Unifies `K → string`, `V → int`.

This gives you the ad-hoc polymorphism narrowing you want.
`Indexable` can be implemented by `dict`, `list` (with integer keys),
custom types — and the engine narrows based on what the key/value
types unify with.

---

## Part 3: Pattern Matching and Exhaustiveness

### 3.1 The problem with typeof

n00b 1's `typeof` statement is cascading if-else with type narrowing
in each branch. It requires a catch-all `else`, which means:

- No exhaustiveness checking (the else is a rug to sweep cases under).
- No compiler help when you add a variant and forget a case.
- Dead branches detected, but missing branches not.

### 3.2 What we need

A `match` construct (or whatever the language calls it) that:

1. Takes a value whose type is a sum (or a type variable constrained
   to a sum).
2. Has one arm per variant.
3. The compiler checks that **every variant is covered exactly once**
   (or that a wildcard arm exists).
4. In each arm, the matched variable is **narrowed** to that variant's
   type.
5. If a variant is a record, its fields are destructured and bound.

### 3.3 How the type system supports this

The type system doesn't implement match syntax — that's the grammar's
job. But it provides the **primitives** that make exhaustiveness
checking possible:

#### Sum decomposition

Given a type `'T = A | B | C`, the type engine can:

- Enumerate the variants: `n00b_tc_sum_variants(t)` → `[A, B, C]`.
- Subtract a variant: `n00b_tc_sum_subtract(t, A)` → `B | C`.
- Check emptiness: `n00b_tc_sum_is_empty(t)` → true when all variants
  are covered.

#### Narrowing

After matching variant `A`, the variable's type in that branch is
`A` (not the full sum). This is a new type variable constrained to
unify with `A`.

#### The exhaustiveness algorithm

```
remaining = copy of the full sum type
for each match arm:
    arm_type = the type pattern in this arm
    if arm_type is a wildcard:
        remaining = empty
        break
    if arm_type is not in remaining:
        error: "duplicate or impossible pattern"
    remaining = remaining - arm_type

if remaining is not empty:
    error: "non-exhaustive match, missing: <remaining variants>"
```

This runs during the type inference pass. The grammar author wires
it up with two annotations and a type rule:

#### Grammar annotations

```bnf
<match_stmt> @rule @scope(match) ::= %"match" <scrutinee> %":" <match_body>
<match_arm>  @rule               ::= %"case" <pattern> %":" <body>
```

The `@scope(match)` creates a scope so the scrutinee's type
variable is shared across all arms.

#### Type rule

```
match_stmt:
  $scrutinee is `S and $result is void

match_arm:
  $pattern is `S and $body is `T
```

The inference engine handles exhaustiveness automatically:
after evaluating all `match_arm` rules within a `match` scope,
it collects all the concrete types that `` `S `` was unified
with across arms and runs the exhaustiveness algorithm (§3.3).

#### The `@exhaust` annotation

To trigger exhaustiveness checking, the grammar author adds
`@exhaust(scrutinee)` to the match statement:

```bnf
<match_stmt> @rule @exhaust(scrutinee) @scope(match) ::=
    %"match" <scrutinee> %":" <match_body>
```

`@exhaust(scrutinee)` tells the inference pass: "after processing
all children, check that the scrutinee's sum type is fully covered
by the arm patterns." Without `@exhaust`, the match works for type
narrowing but doesn't require completeness — useful for languages
that allow partial matches with a default.

The exhaustiveness checker:
1. Gets the scrutinee's resolved type (must be a Sum).
2. Collects each arm's pattern type.
3. Runs the subtraction algorithm from §3.3.
4. Emits `NON_EXHAUSTIVE` or `UNREACHABLE_PATTERN` errors.

#### Nested patterns

For sum variants that carry payloads (records), exhaustiveness extends
recursively:

```
match shape:
    case Circle{r}:  ...    // binds r: float
    case Rect{w, h}: ...    // binds w: float, h: float
```

The type system provides `n00b_tc_record_fields(t)` to enumerate
field names and types for binding. Nested sum patterns (variant
inside variant) recurse the exhaustiveness check.

### 3.4 Diagnostics

When a match is non-exhaustive, the error should say exactly what's
missing:

```
error: non-exhaustive match on type `Shape`
  missing variants:
    - Triangle{base: float, height: float}
  add a case for `Triangle`, or add a wildcard `_` arm
```

When a pattern is unreachable:

```
warning: unreachable pattern
  case `Circle` is already covered by a previous arm
```

When a scrutinee isn't a sum type:

```
error: match requires a sum type, got `int`
  consider using `switch` for value matching instead
```

---

## Part 4: Type Rules

### 4.1 Design philosophy: rules separate from grammar

Slop's `@infer("$0 unify $1, $self : int")` is a constraint
mini-language jammed into a BNF annotation string. It's compact but
unreadable — a grammar author has to decode positional references and
a custom DSL. And cramming type logic inline with the grammar makes
both harder to read.

The better approach: **mark** which productions have type rules (a
lightweight annotation), then **write the rules separately** in a
block that reads like English. The grammar stays clean. The rules
stay readable.

### 4.2 The @rule annotation

`@rule` marks a production as having an associated type rule. It
goes on the BNF production line — its only job is to say "look up
the type rule for this production."

```bnf
<int_literal>  @rule ::= %INT
<add_expr>     @rule ::= <lhs> %"+" <rhs>
<index_expr>   @rule ::= <container> %"[" <key> %"]"
<call_expr>    @rule ::= <callee> %"(" <args> %")"
```

If a nonterminal has multiple alternatives and only some need rules,
use `@rule(name)` to give each alternative a distinct rule name:

```bnf
<binary_expr> @rule(add)    ::= <lhs> %"+" <rhs>
<binary_expr> @rule(compare) ::= <lhs> <cmp_op> <rhs>
<binary_expr>               ::= <lhs> %"," <rhs>   # no type rule
```

That's it. The grammar stays free of type logic.

### 4.3 Type rule syntax

Type rules live in a separate block — either after the grammar in
the same file, or in their own file. Each rule names a production
(or rule name), then lists one or more `when/then` clauses:

```
int_literal:
  $result is int

float_literal:
  $result is float

string_literal:
  $result is string
```

The simplest rules are unconditional: the result just **is** a type.
`$result` refers to the node itself — the production's synthesized
type.

#### Referencing children

`$child_name` refers to a named child from the BNF production.
Type variables use backtick: `` `T ``.

```
add_expr:
  $lhs is `T and $rhs is `T and $result is `T

neg_expr:
  $result is `T and $operand is `T

assign_expr:
  $target is `T and $value is `T and $result is `T
```

When the same `` `T `` appears in multiple positions, those types
must **unify** — they're the same type variable.

#### Constraints on type variables

The `:` after a type variable introduces a constraint list:

```
add_expr:
  $lhs is `T:Numeric and $rhs is `T and $result is `T
```

`` `T:Numeric `` means "`` `T `` must implement the `Numeric`
interface." Multiple constraints are comma-separated:

```
  $x is `T:Numeric,Comparable
```

This reads: "`` `T `` implements both `Numeric` and `Comparable`."

### 4.4 Conditional rules (when/else)

When a production's type depends on context, use `when/else` clauses.
Each `when` is an if-branch; cases are tried top-to-bottom; the first
match wins:

```
binary_op:
  when $lhs is string and $rhs is string:
    $result is string
  when $lhs is `T:Numeric and $rhs is `T:
    $result is `T
```

This reads almost like English: "when the left-hand side is a string
and the right-hand side is a string, the result is a string. When
the left-hand side is some numeric type T and the right-hand side
is the same type, the result is that type."

The colon after the `when` condition opens the constraint block for
that case.

#### Value guards

Sometimes the condition depends on a child's *token value*, not its
type — e.g., which operator was matched:

```
binary_expr:
  when $op == "+" and $lhs is string and $rhs is string:
    $result is string
  when $op == "+" and $lhs is `T:Numeric and $rhs is `T:
    $result is `T
  when $op == "==" and $lhs is `T and $rhs is `T:
    $result is bool
  when $op == "<" and $lhs is `T:Comparable and $rhs is `T:
    $result is bool
```

`$op == "+"` checks the token text of the `op` child. Value guards
can be combined with type conditions using `and`.

#### Fallback / else

An `else` clause acts as a default when no `when` matches:

```
to_string:
  when $expr is string:
    $result is string
  else:
    $result is string and $expr is `T:Stringable
```

If no `when` clause matches and there's no `else`, it's a type error.

### 4.5 Parameterized type destructuring

Backtick variables can appear inside parameterized types, binding
to the type's parameters:

```
list_index:
  $container is list[`T] and $index is int and $result is `T

dict_index:
  $container is dict[`K, `V] and $key is `K and $result is `V
```

This is pattern matching on the type structure: "the container is a
dict of `` `K `` to `` `V ``" simultaneously constrains the container
to be a dict and binds `` `K `` and `` `V `` to its type parameters.

For generic indexing via interfaces:

```
index_expr:
  $container is `C:Indexable[`K, `V] and $key is `K and $result is `V
```

`` `C:Indexable[`K, `V] `` means "`` `C `` implements `Indexable`
with key type `` `K `` and value type `` `V ``."

### 4.6 Function calls

Function types destructure naturally:

```
call_expr:
  $callee is (`P) -> `R and $args is `P and $result is `R
```

For calls with keyword arguments, the kargs are an unordered record
(see §1.8). The rule syntax uses `**{...}` to denote the kargs
record type:

```
call_with_kw:
  $callee is (`P, **{.timeout: int, .retries: int}) -> `R
    and $args is `P and $result is `R
```

The `**{...}` in the rule is sugar for "an unordered record with
these fields." At the call site, the inference engine constructs an
open kargs record from the keywords actually passed and unifies it
against the callee's closed kargs record (§1.8).

For generic kargs (the callee accepts unknown keywords):

```
generic_call:
  $callee is (`P, **`K) -> `R
    and $args is `P and $kw_args is `K and $result is `R
```

Here `` `K `` is a type variable that will unify with whatever
unordered record the callee expects.

### 4.7 Sum types and narrowing

Sum types use `|`:

```
numeric_literal:
  $result is int | float
```

The inference engine narrows based on context — if the literal is
used where an `int` is expected, it becomes `int`.

Negative constraints remove types from a sum:

```
nil_checked:
  $expr is `T | nil and $result is `T and `T not nil
```

"The expression is `` `T `` or nil; the result is `` `T ``; and
`` `T `` is not nil." The `not` keyword is a negative constraint.

### 4.8 Field access and record constraints

```
field_access:
  $obj is `R and `R has .x: `F and $result is `F
```

`` `R has .x: `F `` means "`` `R `` has a field named `x` of type
`` `F ``." This works for records, interfaces, or any type that
supports field access.

For dynamic field names (the field comes from the grammar):

```
dot_access:
  $obj is `R and `R has .$field: `F and $result is `F
```

`.$field` takes the field name from the `field` child's token text.

### 4.9 Scope-level type variables

**Default scope:** Type variables resolve in the **current static
scope** — the innermost `@scope` enclosing the production. You
don't need to qualify them. A bare `` `R `` in a rule for a
production inside a `func` scope refers to the same variable as
`` `R `` in any other rule in that scope.

Some type rules span multiple productions. Function return types are
the classic example: every `return` statement must produce the same
type.

In the grammar:

```bnf
<func_def>    @rule @scope(func) ::= %"func" <name> %"(" <params> %")" %"->" <return_type> <body>
<return_stmt> @rule              ::= %"return" <value>
```

In the rules:

```
func_def:
  $return_type is `R

return_stmt:
  $value is `R
```

Both rules are inside the `func` scope, so `` `R `` refers to the
same scope-level variable. Every return statement's value must
unify with the function's declared return type.

To explicitly reference a different (e.g., outer) scope, qualify it:

```
inner_return:
  $value is scope.outer_func.`R
```

But in practice, the default (current scope) is almost always what
you want.

### 4.10 Promotions in rules

Type rules can declare that a promotion is acceptable:

```
widening_add:
  $lhs is `L:Numeric and $rhs is `R:Numeric
    and `L promotes to `T and `R promotes to `T
    and $result is `T
```

"Both operands are numeric; both promote to some common type `` `T ``;
the result is `` `T ``." The promotion graph (Part 5) determines
which promotions are valid.

### 4.11 Summary: rule syntax

| Pattern | Example | Meaning |
|---------|---------|---------|
| `$child is T` | `$lhs is int` | Child has type T |
| `` $child is `T `` | `` $lhs is `T `` | Child has fresh type var |
| `` `T:I `` | `` `T:Numeric `` | Type var constrained to interface |
| `` `T:I,J `` | `` `T:Numeric,Comparable `` | Multiple interface constraints |
| `$child is C[`T]` | `$c is list[`T]` | Destructure parameterized type |
| `T \| U` | `int \| float` | Sum type |
| `` `T not X `` | `` `T not nil `` | Negative constraint |
| `` `R has .f: U `` | `` `R has .x: `F `` | Field constraint |
| `` `V `` (in scope) | `` `R `` | Type var in current scope (default) |
| `` scope.name.`V `` | `` scope.func.`R `` | Type var in a specific scope |
| `` `L promotes to `T `` | (see §4.10) | Promotion constraint |
| `$child == "val"` | `$op == "+"` | Value guard on token text |
| `when ...:` | (see §4.4) | Conditional case |
| `else:` | (see §4.4) | Default/fallback case |

### 4.12 Where do rules live?

Rules can appear **anywhere in the BNF file** — interleaved with
grammar productions, grouped at the end, or anything in between.
Some people like rules next to their productions; others prefer all
type rules together. Both work.

**Option 1: Rules next to their productions:**

```bnf
<int_literal> @rule ::= %INT

int_literal:
  $result is int

<add_expr> @rule ::= <lhs> %"+" <rhs>

add_expr:
  $lhs is `T:Numeric and $rhs is `T and $result is `T
```

**Option 2: All rules grouped together:**

```bnf
# --- Grammar ---
<int_literal> @rule ::= %INT
<add_expr>    @rule ::= <lhs> %"+" <rhs>

# --- Type rules ---
int_literal:
  $result is int

add_expr:
  $lhs is `T:Numeric and $rhs is `T and $result is `T
```

**Option 3: Separate file** — loaded via the C API:

```c
n00b_tc_load_rules(ctx, grammar, r"my_lang.rules");
```

**Option 4: C API only** — no rule text, build rules
programmatically (§4.13).

### 4.13 Formal grammar for type rules

The type rule syntax parsed by Phase 7's `type_rules.c`:

```bnf
# A rule file is a sequence of rule blocks.
<rule_file>     ::= <rule_block>*

# Each block: a name, colon, then either unconditional or conditional body.
<rule_block>    ::= <IDENT> ":" <rule_body>

<rule_body>     ::= <unconditional>
                  | <conditional>+

# Unconditional: just constraint expressions.
<unconditional> ::= <constraint_list>

# Conditional: when/else clauses.
<conditional>   ::= "when" <condition_list> ":" <constraint_list>
                  | "else" ":" <constraint_list>

# Conditions are joined by "and".
<condition_list> ::= <condition> ("and" <condition>)*

<condition>     ::= <type_assertion>
                  | <value_guard>

# $child is Type
<type_assertion> ::= "$" <IDENT> "is" <type_expr>

# $child == "value"
<value_guard>    ::= "$" <IDENT> "==" <STRING>

# Constraint expressions joined by "and".
<constraint_list> ::= <constraint_expr> ("and" <constraint_expr>)*

<constraint_expr> ::= <type_assertion>
                    | <negative_constraint>
                    | <field_constraint>
                    | <promotes_constraint>

# `T not Type
<negative_constraint> ::= <type_var> "not" <type_expr>

# `R has .field: Type    or    `R has .$child: Type
<field_constraint>    ::= <type_var> "has" "." <field_ref> ":" <type_expr>
<field_ref>           ::= <IDENT>
                        | "$" <IDENT>

# `L promotes to `T
<promotes_constraint> ::= <type_var> "promotes" "to" <type_var>

# --- Type expressions ---

<type_expr>     ::= <type_atom>
                  | <type_atom> "|" <type_expr>              # Sum

<type_atom>     ::= <IDENT>                                  # Primitive or constructor
                  | <type_var>                                # `T
                  | <type_var> ":" <constraint_names>         # `T:Numeric,Comparable
                  | <IDENT> "[" <type_list> "]"              # Parameterized: list[`T]
                  | <type_var> ":" <IDENT> "[" <type_list> "]"  # `C:Indexable[`K, `V]
                  | <fn_type>                                # Function type
                  | "(" <type_list> ")"                      # Tuple
                  | "(" <type_list> "," "..." ")"            # Open tuple
                  | <kargs_record>                           # Unordered record literal

# Function type — positional, optional *vargs, optional **kargs, -> return.
<fn_type>       ::= "(" <fn_params> ")" "->" <type_expr>

<fn_params>     ::= <type_list>                              # positional only
                  | <type_list> "," "*" <type_expr>           # positional + vargs
                  | <type_list> "," <kargs_ref>               # positional + kargs
                  | <type_list> "," "*" <type_expr> "," <kargs_ref>  # all three
                  | "*" <type_expr>                           # vargs only
                  | <kargs_ref>                               # kargs only
                  | "*" <type_expr> "," <kargs_ref>           # vargs + kargs

# **{...} inline kargs record, or **`T for a type variable.
<kargs_ref>     ::= "**" <kargs_record>
                  | "**" <type_var>

# Unordered record: {.name: type, .name: type, ...}
<kargs_record>  ::= "{" <kargs_fields> "}"

<kargs_fields>  ::= <kargs_field> ("," <kargs_field>)*

<kargs_field>   ::= "." <IDENT> ":" <type_expr>

<type_var>      ::= "`" <IDENT>
                  | "scope" "." <IDENT> "." "`" <IDENT>      # Qualified scope ref

<constraint_names> ::= <IDENT> ("," <IDENT>)*

<type_list>     ::= <type_expr> ("," <type_expr>)*

# Terminals
<IDENT>         ::= [a-zA-Z_][a-zA-Z0-9_]*
<STRING>        ::= '"' [^"]* '"'
```

This grammar is intentionally simple — it's a DSL for type rules,
not a general-purpose expression language. The parser (Phase 7)
translates this into `n00b_tc_rule_t` / `n00b_tc_rule_case_t`
structures that the inference engine evaluates.

### 4.14 Programmatic C API

The rule syntax desugars to C API calls. Grammar authors who build
grammars in C (or need dynamic rules) use this directly.

**Design principle:** composable, one-expression-per-rule where
possible. Type constructors take varargs (`n00b_tc_type_t *+`)
via ncc's typed vargs. Rule constructors take constraint varargs.

#### Type constructors

All type constructors return `n00b_tc_type_t *` and compose freely:

```c
n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

// Primitives are pre-registered on the context.
ctx->t_int       // int
ctx->t_string    // string
ctx->t_bool      // bool
ctx->t_nil       // nil
ctx->t_f64       // f64
// ... etc.

// Named type variables — given_name is set, shown in errors.
auto t = n00b_tc_var(ctx, r"T");
auto k = n00b_tc_var(ctx, r"K");
auto v = n00b_tc_var(ctx, r"V");

// Anonymous type variable — given_name is none, display_name is "t_42".
auto anon = n00b_tc_fresh_var(ctx);

// Parameterized types — varargs for params.
auto list_t = n00b_tc_param(ctx, r"list", t);           // list[`T]
auto dict_kv = n00b_tc_param(ctx, r"dict", k, v);       // dict[`K, `V]
auto ref_int = n00b_tc_param(ctx, r"ref", ctx->t_int);  // ref[int]
auto maybe_t = n00b_tc_param(ctx, r"maybe", t);         // maybe[`T]

// Sum types — varargs for variants.
auto int_or_str = n00b_tc_sum(ctx, ctx->t_int, ctx->t_string);
auto nullable   = n00b_tc_sum(ctx, t, ctx->t_nil);  // `T | nil

// Function types.
auto fn1 = n00b_tc_fn(ctx, ctx->t_int, ctx->t_string,
                       .returns = ctx->t_bool);
// (int, string) -> bool

auto fn2 = n00b_tc_fn(ctx, ctx->t_string,
                       .vargs    = ctx->t_int,
                       .returns  = ctx->t_bool);
// (string, *int) -> bool

// Keywords are just an unordered record.
auto kargs = n00b_tc_record(ctx, r"",
                              n00b_tc_field(r"timeout", ctx->t_int, .has_default = true),
                              n00b_tc_field(r"retries", ctx->t_int, .has_default = true),
                              .ordered = false);

auto fn3 = n00b_tc_fn(ctx, ctx->t_string,
                       .kargs   = kargs,
                       .returns = ctx->t_bool);
// (string, **{.timeout: int, .retries: int}) -> bool

// Record types — ordered by default (struct layout).
auto point = n00b_tc_record(ctx, r"Point",
                             n00b_tc_field(r"x", ctx->t_int),
                             n00b_tc_field(r"y", ctx->t_int));
// Point{x: int, y: int} — ordered

// Tuple types — varargs for elements.
auto pair = n00b_tc_tuple(ctx, ctx->t_int, ctx->t_string);
// (int, string) — closed, exactly 2 elements

auto open_pair = n00b_tc_tuple(ctx, ctx->t_int, ctx->t_string,
                                .open = true);
// (int, string, ...) — open, at least 2 elements

auto at_least_4 = n00b_tc_tuple(ctx, t, t, t, t,
                                 .open = true);
// (`T, `T, `T, `T, ...) — open, at least 4 of the same type
```

**API signatures:**

```c
// Field descriptor for n00b_tc_record (passed as typed vargs).
extern n00b_tc_field_t n00b_tc_field(n00b_string_t name, n00b_tc_type_t *type) _kargs {
    bool has_default = false;  // true = caller may omit (keyword args).
};

// Type constructors.
n00b_tc_type_t *n00b_tc_var(n00b_tc_ctx_t *ctx, n00b_string_t name);
n00b_tc_type_t *n00b_tc_fresh_var(n00b_tc_ctx_t *ctx);
n00b_tc_type_t *n00b_tc_param(n00b_tc_ctx_t *ctx, n00b_string_t name, n00b_tc_type_t *+);
n00b_tc_type_t *n00b_tc_sum(n00b_tc_ctx_t *ctx, n00b_tc_type_t *+);
extern n00b_tc_type_t *n00b_tc_record(n00b_tc_ctx_t *ctx, n00b_string_t name,
                                       n00b_tc_field_t +) _kargs {
    bool ordered = true;   // false = unordered keyword bag.
    bool open    = false;  // true = open row (duck typing / call site).
};
n00b_tc_type_t *n00b_tc_tuple(n00b_tc_ctx_t *ctx, n00b_tc_type_t *+) _kargs { bool open = false; };
extern n00b_tc_type_t *n00b_tc_fn(n00b_tc_ctx_t *ctx, n00b_tc_type_t *+) _kargs {
    n00b_tc_type_t  *returns;
    n00b_tc_type_t  *vargs  = NULL;
    n00b_tc_type_t  *kargs  = NULL;  // Must be a Record with ordered=false, or NULL.
};
```

`n00b_tc_var` takes `n00b_string_t *` — pass `r"T"` for a named
variable. The function wraps it in `n00b_option_set(given_name)` and
generates a `display_name`. `n00b_tc_fresh_var` creates an anonymous
Var with `given_name = none`.

The `n00b_tc_param`, `n00b_tc_sum`, `n00b_tc_fn`, and
`n00b_tc_tuple` functions take `n00b_tc_type_t *+` (ncc typed
vargs) for their positional type arguments. `n00b_tc_fn` uses
`_kargs` for `.returns`, `.vargs`, and `.kargs` (a Record type built
via `n00b_tc_record` with `.ordered = false`). `n00b_tc_record` uses
`_kargs` for `.ordered` and `.open`. `n00b_tc_field` builds a single
field descriptor with optional `.has_default`.

#### Constraints

```c
n00b_tc_implements(ctx, t, r"Numeric");       // `T:Numeric
n00b_tc_has_field(ctx, t, r"x", ctx->t_int);  // `T has .x: int
n00b_tc_not(ctx, t, ctx->t_nil);               // `T not nil
n00b_tc_promotes_to(ctx, t, ctx->t_f64);       // `T promotes to f64
n00b_tc_one_of(ctx, t, ctx->t_int, ctx->t_string); // `T is int or string
```

Constraints attach to the type variable and are checked during
unification.

#### Rules

Rules bind a production name to constraints on its children and
result. The composable API uses `n00b_tc_child()` and
`n00b_tc_result()` as vararg constraint terms:

```c
// Simple: int_literal → $result is int
n00b_tc_rule(ctx, r"int_literal",
    n00b_tc_result(ctx->t_int));

// Constrained: add_expr → $lhs is `T:Numeric, $rhs is `T, $result is `T
auto t = n00b_tc_var(ctx, r"T");
n00b_tc_implements(ctx, t, r"Numeric");

n00b_tc_rule(ctx, r"add_expr",
    n00b_tc_child(r"lhs", t),
    n00b_tc_child(r"rhs", t),
    n00b_tc_result(t));

// Destructuring: dict_index → $container is dict[`K, `V], $key is `K, $result is `V
auto k = n00b_tc_var(ctx, r"K");
auto v = n00b_tc_var(ctx, r"V");

n00b_tc_rule(ctx, r"dict_index",
    n00b_tc_child(r"container", n00b_tc_param(ctx, r"dict", k, v)),
    n00b_tc_child(r"key", k),
    n00b_tc_result(v));
```

#### Conditional rules

`n00b_tc_when` and `n00b_tc_else` compose inside `n00b_tc_rule`:

```c
// binary_op:
//   when $lhs is string and $rhs is string: $result is string
//   when $lhs is `T:Numeric and $rhs is `T: $result is `T
n00b_tc_rule(ctx, r"binary_op",
    n00b_tc_when(
        n00b_tc_child(r"lhs", ctx->t_string),
        n00b_tc_child(r"rhs", ctx->t_string),
        n00b_tc_result(ctx->t_string)),
    n00b_tc_when(
        n00b_tc_child(r"lhs", t),
        n00b_tc_child(r"rhs", t),
        n00b_tc_result(t)));

// With value guards:
n00b_tc_rule(ctx, r"binary_expr",
    n00b_tc_when(
        n00b_tc_guard(r"op", r"+"),
        n00b_tc_child(r"lhs", ctx->t_string),
        n00b_tc_child(r"rhs", ctx->t_string),
        n00b_tc_result(ctx->t_string)),
    n00b_tc_when(
        n00b_tc_guard(r"op", r"+"),
        n00b_tc_child(r"lhs", t),
        n00b_tc_child(r"rhs", t),
        n00b_tc_result(t)),
    n00b_tc_when(
        n00b_tc_guard(r"op", r"=="),
        n00b_tc_child(r"lhs", t),
        n00b_tc_child(r"rhs", t),
        n00b_tc_result(ctx->t_bool)));
```

Each `n00b_tc_when(...)` takes a varargs list of constraint terms
(children, guards, result). `n00b_tc_else(...)` for the fallback.

### 4.15 Type extractors

Built-in accessors for decomposing types programmatically:

```c
auto elem = n00b_tc_element_of(ctx, container_type);          // param[0]
auto key  = n00b_tc_key_of(ctx, dict_type);                   // param[0]
auto val  = n00b_tc_value_of(ctx, dict_type);                 // param[1]
auto ret  = n00b_tc_return_of(ctx, fn_type);                  // fn return
auto p    = n00b_tc_param_of(ctx, type, n);                   // param[n]
auto ft   = n00b_tc_field_of(ctx, record_type, r"x");        // record field
auto pt   = n00b_tc_positional_of(ctx, fn_type, n);           // fn positional[n]
auto kt   = n00b_tc_kargs_field_of(ctx, fn_type, r"timeout"); // fn kargs field type
```

Custom extractors:

```c
n00b_tc_register_extractor(ctx, r"my_extractor", my_callback);
```

### 4.16 Core type API (standalone, no grammar)

The type system works as a standalone library without any grammar.
This is the API for programmatic type checking:

```c
auto ctx = n00b_tc_ctx_new();

// Create types using composable constructors (see §4.13).
auto t      = n00b_tc_var(ctx, r"T");
auto list_t = n00b_tc_param(ctx, r"list", t);

// Unify.
auto r = n00b_tc_unify(ctx, list_t, n00b_tc_param(ctx, r"list", ctx->t_int));
// r.ok is true, r.type is list[int]

// Constrained variable.
auto c = n00b_tc_var(ctx, r"C");
n00b_tc_implements(ctx, c, r"Indexable");

auto r2 = n00b_tc_unify(ctx, c,
    n00b_tc_param(ctx, r"dict", ctx->t_string, ctx->t_int));
// Succeeds: dict[string, int] implements Indexable.

// Sum types and exhaustiveness.
auto sum = n00b_tc_sum(ctx, ctx->t_int, ctx->t_string, ctx->t_nil);

auto remaining = n00b_tc_sum_copy(ctx, sum);
remaining = n00b_tc_sum_subtract(ctx, remaining, ctx->t_int);
remaining = n00b_tc_sum_subtract(ctx, remaining, ctx->t_string);
// remaining is: nil
// n00b_tc_sum_is_exhausted(remaining) → false; missing nil

// Polymorphic types — type variables with constraints.
auto a = n00b_tc_var(ctx, r"A");
n00b_tc_implements(ctx, a, r"Comparable");

auto id_type = n00b_tc_fn(ctx, a, .returns = ctx->t_bool);
// (`A:Comparable) -> bool

// After inference, freeze: collect unbound Vars as type params.
// At each call site, instantiate: fresh `A with Comparable constraint.

n00b_tc_ctx_free(ctx);
```

### 4.17 The inference pass

The inference pass is a **post-annotation-walk** tree traversal:

```
Input:  parse tree + grammar (with @rule annotations) + type rules + symbol table
Output: node → type mapping + error list
```

1. **Bottom-up walk.** Process children before parent.
2. **Per-node:** Create a fresh type variable for the node (lazily).
3. **Look up the type rule** for the matched production.
4. **Evaluate the rule:**
   - If unconditional: resolve `$child` references, bind type
     variables, apply constraints, unify.
   - If conditional (`when/else`): try each case top-to-bottom.
     First match wins. Check value guards against parse tree,
     attempt type unification non-destructively, commit on match.
5. **Record coercions** (promotions, option wrapping) for later codegen.
6. **On error:** record diagnostic, continue (don't abort on first
   error). If no `when` case matches and there's no `else`,
   emit "no matching type rule" with the attempted cases listed.

The pass is separate from the annotation walk that builds the symbol
table and CF labels. It runs after both, consuming their output.

### 4.18 Type propagation into nodes and symbol table

The inference pass produces a type context mapping nodes to types.
But consumers (codegen, IDE tooling, error messages) need types
accessible directly on parse tree nodes and symbol table entries.

#### Symbol table: `n00b_sym_entry_t`

Currently `n00b_sym_entry_t` has `type_node` (a parse tree pointer
to the syntactic type annotation). After inference, we add a
**resolved type**:

```c
struct n00b_sym_entry_t {
    // ... existing fields ...
    n00b_parse_tree_t                *type_node;     // Syntactic type (from @type/@field).
    n00b_tc_type_t                   *resolved_type; // NEW: resolved type after inference.
    n00b_list_t(n00b_tc_type_t *)    *type_params;   // NEW: unbound Vars (type variables).
};
```

`resolved_type` is set by the inference pass. For variables, it's
the inferred type. For ADT tags, it's the record or sum type. For
function names, it's the function type. For fields, it's the field
type within its parent record.

`type_params` is set by the freeze step. It lists the unbound type
variables (Vars with constraints) that are the function's type
parameters. When the symbol is referenced at a call site, each
type variable is instantiated with a fresh copy. If `type_params`
is NULL or empty, the type is monomorphic (no instantiation needed).

The annotation walk fills in `type_node` (the raw parse subtree).
The inference pass resolves it to a `n00b_tc_type_t *` and stores
it in `resolved_type`. Consumers use whichever they need — the
syntactic form for source display, the resolved form for type
checking.

#### Parse tree nodes: `n00b_nt_node_t`

Currently `n00b_nt_node_t` has no type field. Add one:

```c
typedef struct n00b_nt_node_t {
    // ... existing fields ...
    n00b_tc_type_t *type;   // NEW: type assigned by inference pass.
} n00b_nt_node_t;
```

Every non-terminal node with a `@rule` annotation gets a type
assigned during the inference pass. Leaf nodes (tokens) get types
from their production's rule (e.g., `int_literal` → `int`).

This is the primary output surface: after running the inference
pass, you can walk any subtree and read `pn->type` to get the
resolved type.

#### The inference result

The inference pass populates types in place on parse tree nodes and
symbol table entries, then returns a result struct:

```c
typedef struct {
    n00b_tc_ctx_t                      *tc_ctx;    // Type context (owns all types).
    n00b_list_t(n00b_tc_error_t)       *errors;    // Type errors (may be empty).
    n00b_list_t(n00b_tc_coercion_t)    *coercions; // Implicit coercions found.
} n00b_tc_infer_result_t;
```

The context owns all type memory. Errors are collected — the pass
continues through errors rather than aborting on the first one.
Check `n00b_list_len(result.errors) == 0` for success.

#### Individual operation results

Operations like unify return `n00b_result_t(n00b_tc_type_t *)`.
On success, the resolved type. On failure, an error code. The
rich `n00b_tc_error_t` is accumulated in the context's error list
for later reporting:

```c
auto r = n00b_tc_unify(ctx, lhs, rhs);

if (n00b_result_is_ok(r)) {
    auto resolved = n00b_result_get(r);
    // use resolved type
} else {
    // error details already appended to ctx->errors
}
```

This keeps the familiar `n00b_result_t` / `!` operator workflow
while the structured `n00b_tc_error_t` (with expected/got/constraint
info) is available in the error list for diagnostics.

---

## Part 5: Promotion and Coercion

### 5.1 Promotion graph

Numeric widening rules form a directed graph:

```
i8 → i16 → i32 → i64
u8 → u16 → u32 → u64
u8 → i16, u16 → i32, u32 → i64    (unsigned fits in next-wider signed)
f32 → f64
i32 → f64                          (exact representation)
```

Promotion is **transitive**: `i8` promotes to `i64` via the chain.

Grammar authors can register additional promotions for their
language's type system.

### 5.2 Coercion tracking

When unification succeeds via promotion (not exact match), a coercion
record is emitted:

```c
typedef enum {
    N00B_TC_COERCE_PROMOTE,      // Numeric widening (i32 → i64, f32 → f64).
    N00B_TC_COERCE_OPTION_WRAP,  // T → maybe[T] (implicit option injection).
    N00B_TC_COERCE_CUSTOM,       // Grammar-author-registered coercion.
} n00b_tc_coerce_kind_t;

typedef struct {
    n00b_tc_coerce_kind_t kind;
    n00b_tc_type_t       *from;
    n00b_tc_type_t       *to;
    n00b_tc_span_t        span;   // Where the coercion occurs.
} n00b_tc_coercion_t;
```

Coercions are accumulated per-unification and available to the grammar
author's codegen pass. They enable warnings ("implicit conversion from
i64 to f64 may lose precision") and insertion of explicit conversion
code.

### 5.3 Option wrapping

Unifying `T` with `option['T]` succeeds by implicitly wrapping. This
is tracked as a `N00B_TC_COERCE_OPTION_WRAP` coercion. Grammar authors
can disable this behavior if their language doesn't want implicit
option wrapping.

---

## Part 6: Diagnostics

### 6.1 Source locations

Type errors need source locations. We reuse the token's existing
location info (`line`, `column`, `endcol`, `file` from
`n00b_token_info_t`) and define a span:

```c
typedef struct {
    n00b_option_t(n00b_string_t)  file;       // Source file (option: may be stdin).
    uint32_t                      start_line;
    uint32_t                      start_col;
    uint32_t                      end_line;
    uint32_t                      end_col;
} n00b_tc_span_t;
```

Type *nodes* themselves do **not** carry source locations — they're
abstract. The mapping from type to source is through the parse tree
node or symbol table entry that the type was inferred from. The
inference pass fills in `n00b_tc_span_t` on error structs by looking
up the parse tree node that triggered the error:

```c
auto tok = n00b_parse_node_token(node);
span = (n00b_tc_span_t){ tok->file, tok->line, tok->column, ... };
```

Type variables store `given_name` for display but not locations —
when an error involves a Var, the location comes from the rule or
node that created it.

### 6.2 Error structure

```c
typedef struct {
    n00b_tc_err_kind_t    kind;
    n00b_string_t         message;       // Human-readable
    n00b_tc_type_t       *expected;      // What was expected
    n00b_tc_type_t       *got;           // What was found
    n00b_string_t         constraint;    // Which constraint failed (if any)
    n00b_tc_span_t        span;          // Where in source
    n00b_tc_span_t        related_span;  // Second location (e.g., conflicting decl)
} n00b_tc_error_t;
```

Two spans allow errors like:
```
error: cannot unify `int` with `string`
  --> foo.n00b:3:5
  |  x = 42
  note: conflicting use at:
  --> foo.n00b:7:5
  |  y = x + "hello"
```

### 6.3 Error kinds

| Kind | Example message |
|------|-----------------|
| `UNIFY_FAIL` | "cannot unify `int` with `string`" |
| `CONSTRAINT_FAIL` | "type `set[int]` does not implement `Indexable`" |
| `OCCURS_CHECK` | "infinite type: `'T` occurs in `list['T]`" |
| `NON_EXHAUSTIVE` | "non-exhaustive match on `Shape`, missing: `Triangle`" |
| `UNREACHABLE_PATTERN` | "pattern `Circle` is unreachable (already covered)" |
| `DUPLICATE_VARIANT` | "variant `Red` appears twice in sum type" |
| `NO_SUCH_FIELD` | "type `Point` has no field `z`" |
| `PARAM_MISMATCH` | "`list` expects 1 type parameter, got 2" |
| `ARITY_MISMATCH` | "function expects 2 arguments, got 3" |
| `MISSING_KEYWORD` | "missing required keyword argument `.timeout`" |
| `UNKNOWN_KEYWORD` | "unknown keyword argument `.verbose` (function does not accept it)" |
| `NO_MATCHING_RULE` | "no type rule matched for production `binary_expr`" |

### 6.4 Design principle

Every error message should answer **three questions**:

1. **What happened?** ("cannot unify X with Y")
2. **Why?** ("because constraint Z requires ...")
3. **What can you do?** ("add a case for Triangle" / "check the type
   of the second argument")

---

## Part 7: Module Structure

```
include/typecheck/
    types.h          — n00b_tc_type_t (variant), kind structs, constraints,
                       coercions, errors, field descriptor
    context.h        — n00b_tc_ctx_t, initialization, promotion, interfaces
    construct.h      — composable type constructors (var, prim, param, fn,
                       sum, record, tuple) + rule constructors
    unify.h          — unification API
    freeze.h         — freeze/instantiate API (type param collection, deep copy)
    display.h        — type-to-string, error-to-string
    exhaust.h        — sum decomposition and exhaustiveness checking

src/typecheck/
    context.c        — context lifecycle, built-in types, promotion graph
    construct.c      — type + rule construction
    unify.c          — core unification + constraint checking
    freeze.c         — freeze/instantiate and deep copy
    display.c        — pretty-printing
    exhaust.c        — exhaustiveness algorithm

include/slay/
    type_rules.h     — rule syntax parser (text → rule objects)
    infer.h          — inference pass API

src/slay/
    type_rules.c     — rule syntax parser
    infer.c          — inference tree walk: evaluates rules, propagates
                       types into parse tree nodes + symbol table entries
```

The `typecheck/` module has **no dependency on slay**. The
`slay/type_rules` and `slay/infer` files are the bridge — they
consume `typecheck/` to build and evaluate rules against parse trees.

---

## Part 8: Type Environment and Evaluation

This part describes the runtime machinery: the type context that owns
everything, the unification algorithm, how rules are evaluated, and
how the inference pass walks the tree.

### 8.1 The type context (`n00b_tc_ctx_t`)

The type context is the single owner of all type-checking state for
one compilation unit. It embeds an `n00b_logic_t` program for
relational queries and constraint solving (see §8.9).

```c
typedef struct {
    // --- Type allocation ---
    n00b_list_t(n00b_tc_type_t)   *all_types;    // Arena: all allocated type nodes.
    uint32_t                       next_var_id;   // Counter for fresh Var ids.

    // --- Built-in primitives ---
    n00b_tc_type_t  *t_int;
    n00b_tc_type_t  *t_i8, *t_i16, *t_i32, *t_i64;
    n00b_tc_type_t  *t_u8, *t_u16, *t_u32, *t_u64;
    n00b_tc_type_t  *t_f32, *t_f64;
    n00b_tc_type_t  *t_bool;
    n00b_tc_type_t  *t_string;
    n00b_tc_type_t  *t_nil;
    n00b_tc_type_t  *t_void;

    // --- Logic engine (Datalog + CSP) ---
    n00b_logic_t                                     logic;

    // --- Interfaces ---
    n00b_dict_t(n00b_string_t, n00b_tc_iface_t *)   *interfaces;

    // --- Type rules (from @rule annotations or C API) ---
    n00b_dict_t(n00b_string_t, n00b_tc_rule_t *)     *rules;

    // --- (No separate frozen type dict — type params live on sym_entry_t) ---

    // --- Error accumulator ---
    n00b_list_t(n00b_tc_error_t)                     *errors;

    // --- Coercion log ---
    n00b_list_t(n00b_tc_coercion_t)                  *coercions;
} n00b_tc_ctx_t;
```

Note: the old `impls` and `promotions` dicts are gone — they're now
Datalog relations inside `ctx->logic` (see §8.9).

**Lifecycle:**

```c
auto ctx = n00b_tc_ctx_new();     // Allocates, registers built-in primitives,
                                  // initializes logic engine.

// ... register interfaces, impls, promotions, rules ...
// ... run inference pass ...

n00b_tc_ctx_free(ctx);            // Frees all type nodes, logic engine, state.
```

All `n00b_tc_type_t` nodes are allocated from `all_types`. This
makes context teardown trivial (free the list) and keeps type nodes
cache-friendly.

### 8.2 Union-find operations

The union-find is the core data structure for type inference. Every
`n00b_tc_type_t` has a `forward` pointer.

#### Find (with path compression)

```c
n00b_tc_type_t *
n00b_tc_find(n00b_tc_type_t *t)
{
    while (t->forward) {
        // Path compression: point directly to root.
        if (t->forward->forward) {
            t->forward = t->forward->forward;
        }
        t = t->forward;
    }
    return t;
}
```

Every operation calls `find` first. After find, the returned node
is the **canonical representative** — its `kind` variant is the
actual type.

#### Unify

The heart of the type checker. Pseudocode:

```
unify(ctx, a, b):
    a = find(a)
    b = find(b)
    if a == b: return ok(a)           // Same node.

    // --- Var + anything ---
    if a is Var:
        if occurs(a, b): return err(OCCURS_CHECK)
        if !check_constraints(ctx, a.constraints, b): return err
        a.forward = b                 // Bind a → b.
        return ok(b)
    if b is Var:
        return unify(ctx, b, a)       // Swap so Var is always on left.

    // --- Same kind ---
    if a.kind_tag != b.kind_tag:
        // Special cases: Sum injection, maybe/option normalization.
        return try_special_unify(ctx, a, b)

    match (a.kind, b.kind):
        (Prim, Prim):
            if a.name != b.name: return err(UNIFY_FAIL)
            return ok(a)

        (Param, Param):
            if a.name != b.name: return err(UNIFY_FAIL)
            if len(a.params) != len(b.params): return err(PARAM_MISMATCH)
            for i in 0..len(a.params):
                unify(ctx, a.params[i], b.params[i])!
            return ok(a)

        (Fn, Fn):
            unify_fn(ctx, a, b)       // Positional, vargs, kargs record, return.

        (Sum, Sum):
            unify_sum(ctx, a, b)      // Sorted pairwise.

        (Record, Record):
            unify_record(ctx, a, b)   // Name + fields + open/closed + ordered check.

        (Tuple, Tuple):
            unify_tuple(ctx, a, b)    // Elements + open/closed.
```

After successful unification, one node's `forward` points to the
other (the more-specific one becomes canonical).

#### Occurs check

Prevents infinite types. Before binding Var `a` to type `b`, walk
`b` recursively. If `a` appears anywhere inside `b`, it's a cycle:

```
occurs(var, type):
    type = find(type)
    if type == var: return true
    for each child type in type's kind payload:
        if occurs(var, child): return true
    return false
```

We keep the occurs check and reject recursive types.

#### Record unification

Records have two axes: ordered/unordered and open/closed. The
unification algorithm handles all combinations:

```
unify_record(ctx, a, b):
    // 1. Ordered/unordered must match.
    if a.ordered != b.ordered:
        return err(UNIFY_FAIL, "ordered/unordered mismatch")

    // 2. Named records must have matching names.
    //    Anonymous records (empty name) skip the name check.
    if a.name != "" and b.name != "" and a.name != b.name:
        return err(UNIFY_FAIL)

    // 3. Type parameters (if any).
    if a.type_params and b.type_params:
        if len(a.type_params) != len(b.type_params):
            return err(PARAM_MISMATCH)
        for i in 0..len(a.type_params):
            unify(ctx, a.type_params[i], b.type_params[i])!

    // 4. Field matching — depends on ordered flag.
    if a.ordered:
        unify_record_ordered(ctx, a, b)
    else:
        unify_record_unordered(ctx, a, b)

unify_record_ordered(ctx, a, b):
    // Ordered: fields match by position AND name.
    // open+closed: open side's fields must be a prefix of closed.
    // closed+closed: exact field count match.
    if a.open and !b.open:
        if len(a.fields) > len(b.fields): return err(UNIFY_FAIL)
        for i in 0..len(a.fields):
            if a.field_names[i] != b.field_names[i]: return err(UNIFY_FAIL)
            unify(ctx, a.field_types[i], b.field_types[i])!
    else if !a.open and b.open:
        return unify_record_ordered(ctx, b, a)  // Swap.
    else if a.open and b.open:
        // Both open: unify the common prefix, result stays open.
        n = min(len(a.fields), len(b.fields))
        for i in 0..n:
            if a.field_names[i] != b.field_names[i]: return err(UNIFY_FAIL)
            unify(ctx, a.field_types[i], b.field_types[i])!
    else:
        // Both closed: exact match.
        if len(a.fields) != len(b.fields): return err(UNIFY_FAIL)
        for i in 0..len(a.fields):
            if a.field_names[i] != b.field_names[i]: return err(UNIFY_FAIL)
            unify(ctx, a.field_types[i], b.field_types[i])!

unify_record_unordered(ctx, a, b):
    // Unordered: fields match by name only.
    // Build name→index maps for both sides.
    if a.open and !b.open:
        // Every field in a (open) must exist in b (closed).
        for field in a.fields:
            match = find field.name in b.fields
            if !match: return err(UNKNOWN_KEYWORD, field.name)
            unify(ctx, field.type, match.type)!
        // Every closed-side field NOT in the open side must have a default.
        for field in b.fields:
            if field.name not in a.fields:
                if !field.has_default:
                    return err(MISSING_KEYWORD, field.name)
    else if !a.open and b.open:
        return unify_record_unordered(ctx, b, a)  // Swap.
    else if a.open and b.open:
        // Both open: unify common fields, result stays open.
        for field in a.fields:
            match = find field.name in b.fields
            if match: unify(ctx, field.type, match.type)!
    else:
        // Both closed: every field in each must exist in the other.
        if len(a.fields) != len(b.fields): return err(UNIFY_FAIL)
        for field in a.fields:
            match = find field.name in b.fields
            if !match: return err(NO_SUCH_FIELD, field.name)
            unify(ctx, field.type, match.type)!
```

The `has_default` check only applies to **unordered** records (kargs).
For ordered records (structs), missing fields are always an error —
there's no concept of "optional fields with defaults" in a struct
layout.

### 8.3 Constraint evaluation

Constraints on a Var are checked when the Var is bound (its
`forward` is set). Each constraint kind has a checker:

| Constraint | Check at binding time |
|------------|----------------------|
| **Unifies** | `unify(ctx, constraint.target, bound_type)` — union-find |
| **OneOf** | `bound_type` matches at least one element in the set — CSP domain check |
| **Implements** | Datalog query: `implements(bound_type.name, iface_name)` |
| **HasField** | `bound_type` is Record with the named field; unify field type — union-find |
| **HasParam** | `bound_type` is Param; unify the Nth param — union-find |
| **Promotes** | Datalog query: `promotes(bound_type.name, target.name)` (transitive) |
| **Not** | `bound_type` does NOT unify with the excluded type (try-unify via CSP push/pop) |

**Constraint interaction with open rows:** When checking HasField
against a closed Record, the field must exist. Against an open
Record (or a Var with HasField constraints), the constraint is
deferred — it becomes a constraint on the Var that will be checked
when the Var is eventually bound to a concrete type.

**Deferred constraints:** Some constraints can't be checked
immediately because the Var isn't bound yet. These stay on the
Var's constraint list and propagate when the Var's forward is set:
if Var `a` is unified with Var `b`, `b` inherits `a`'s constraints.

### 8.4 Freezing and instantiation

When we infer a function's type, we're working within the **type
context** — type variables, bindings, and constraints in scope.
When inference completes, we **freeze**: unbound type variables
(with their constraints) that don't escape the scope become the
function's type parameters. The frozen type is exported for linking.

There's no separate "scheme" or "generic" struct — a frozen type is
just a regular `n00b_tc_type_t *` that happens to contain unbound
Vars. Those Vars *are* the type parameters, and their constraints
travel with them:

```c
// A frozen function type: (A:Comparable) -> bool
// The Var `A has given_name = "A" and constraints = [Implements("Comparable")]
// It's just a regular n00b_tc_fn_t with a Var in its positional list.
```

**Freezing (generalization):** After inferring a function's type,
collect the unbound Vars that don't escape the scope:

```
freeze(ctx, type, scope):
    free_vars = find_unbound_vars(type)
    escaped   = vars_visible_outside(scope)
    type_params = free_vars - escaped
    // Store type_params on the symbol entry for instantiation.
    sym->type_params = type_params
    sym->resolved_type = type
```

The type params list is needed so we know which Vars to freshen
at each use site.

**Instantiation:** When a frozen type is used at a call site,
each type parameter Var is replaced with a fresh Var (inheriting
its `given_name` and constraints):

```
instantiate(ctx, sym):
    if sym.type_params is empty: return sym.resolved_type
    fresh_map = {}
    for tvar in sym.type_params:
        fresh = n00b_tc_var(ctx, tvar.given_name)
        fresh.constraints = copy(tvar.constraints)
        fresh_map[tvar] = fresh
    return deep_copy(sym.resolved_type, fresh_map)
```

Each use site gets independent type variables. Error messages show
the user's names (`` `T `` not `t_42`) because `given_name` is
preserved through instantiation.

### 8.5 Rule evaluation

Type rules (from §4.3–4.4) are evaluated during the inference pass.
A rule is a data structure:

```c
typedef struct {
    n00b_string_t                     name;       // Production or rule name.
    n00b_list_t(n00b_tc_rule_case_t)  *cases;     // when/else cases (or one unconditional).
} n00b_tc_rule_t;

typedef struct {
    n00b_list_t(n00b_tc_rule_guard_t)  *guards;    // Value guards ($op == "+").
    n00b_list_t(n00b_tc_rule_bind_t)   *bindings;  // $child is Type assertions.
    n00b_tc_type_t                     *result;     // $result type.
} n00b_tc_rule_case_t;

typedef struct {
    n00b_string_t    child_name;   // "$lhs", "$rhs", etc.
    n00b_tc_type_t  *type;         // The type/pattern to unify with.
} n00b_tc_rule_bind_t;

typedef struct {
    n00b_string_t    child_name;   // "$op"
    n00b_string_t    value;        // "+"
} n00b_tc_rule_guard_t;
```

**Evaluation algorithm:**

```
evaluate_rule(ctx, rule, node, children):
    for each case in rule.cases:
        // 1. Check value guards against parse tree.
        if any guard fails: continue to next case

        // 2. Try unifying child types (non-destructively).
        snapshot = save_uf_state(ctx)
        ok = true
        for each binding in case.bindings:
            child_type = lookup_child_type(children, binding.child_name)
            r = n00b_tc_unify(ctx, child_type, binding.type)
            if r is err:
                ok = false
                break

        if !ok:
            restore_uf_state(ctx, snapshot)  // Undo trial unifications.
            continue to next case

        // 3. First matching case: commit and set result type.
        node.type = find(case.result)
        return ok

    // No case matched.
    emit_error(ctx, NO_MATCHING_RULE, node, rule)
```

**Non-destructive trial:** For when/else rules, each case is tried
speculatively. The union-find state is saved before the trial and
restored if the case fails. This uses the same push/pop pattern
as the CLP(FD) store's backtracking:

- **Save:** snapshot the `forward` pointers of any Vars that get
  bound during the trial.
- **Restore:** null out those `forward` pointers.

This is lightweight — just a list of (Var, old_forward) pairs.

### 8.6 The inference walk

The inference pass is a bottom-up tree traversal that evaluates
type rules and propagates types.

**Input:**
- Parse tree (from PWZ or Earley).
- Grammar with `@rule` annotations.
- Parsed type rules (from BNF file or C API).
- Symbol table (from annotation walk).
- Type context (initialized with primitives, interfaces, promotions).

**Output:**
- `n00b_nt_node_t.type` populated on annotated nodes.
- `n00b_sym_entry_t.resolved_type` populated on symbol entries.
- `n00b_tc_infer_result_t` with errors and coercions.

**Algorithm:**

```
infer_walk(ctx, node):
    if node is leaf (token):
        return  // Tokens get types from their parent's rule.

    // 1. Recurse into children first (bottom-up).
    for each child of node:
        infer_walk(ctx, child)

    // 2. Look up the type rule for this production.
    rule = ctx->rules[node.rule_name]
    if rule is NULL: return  // No @rule annotation.

    // 3. Create a fresh type variable for this node if needed.
    if node.type is NULL:
        node.type = n00b_tc_var(ctx, node.name)

    // 4. Evaluate the rule.
    evaluate_rule(ctx, rule, node, node.children)

    // 5. If this node declares a symbol, propagate to symtab.
    sym = symtab_lookup(node)
    if sym and sym.resolved_type is NULL:
        sym.resolved_type = find(node.type)
```

**Scope interaction:** Type variables declared in `@scope`
annotations are created once when the scope is entered (during the
annotation walk or at the start of the inference walk over that
subtree). All rules within that scope share the same Var instances.
The default scope for a bare `` `R `` in a rule is the innermost
enclosing `@scope`.

**Handling polymorphism:** When the inference walk encounters a
reference to a polymorphic function (one with type params), it
instantiates — replacing each type variable with a fresh copy:

```
// In the call_expr rule evaluation:
callee_sym = symtab_lookup(callee_node)
callee_type = instantiate(ctx, callee_sym)
// If sym has type_params, returns a fresh copy.
// If not, returns sym.resolved_type directly.
```

### 8.7 Backtracking and the union-find trail

The when/else rule evaluation needs speculative unification with
rollback. We use **two coordinated trails**:

1. **Union-find trail** — records `(var, old_forward)` pairs for
   undoing forward pointer mutations.
2. **CSP push/pop** — `n00b_csp_push_state()` / `n00b_csp_pop_state()`
   on the logic engine's CSP store, which handles domain snapshots
   and constraint removal.

Both are saved/restored together during speculative evaluation:

```c
typedef struct {
    n00b_list_t(n00b_tc_uf_entry_t) *trail;  // (var, old_forward) pairs.
} n00b_tc_uf_snapshot_t;

typedef struct {
    n00b_tc_type_t  *var;          // The Var that was bound.
    n00b_tc_type_t  *old_forward;  // Its previous forward (usually NULL).
} n00b_tc_uf_entry_t;
```

**Save:** record the trail length + push CSP state.
**Bind:** when setting `var->forward`, push `(var, var->forward)`
onto the UF trail. Any CSP domain narrowing happens through the CSP
store (which has its own trail).
**Restore:** pop UF trail entries back to the saved length + pop
CSP state. Both trails unwind in lockstep.

This avoids duplicating the backtracking machinery that the CSP
store already has — we just add the thin UF trail on top of it.

### 8.8 Implementation plan

The implementation builds bottom-up in dependency order:

| Phase | Files | What | Depends on |
|-------|-------|------|------------|
| **1** | `types.h`, `construct.c` | Type node structs (with `n00b_variant_t`), kind payloads, constructors | `core/variant.h`, `core/list.h` |
| **2** | `context.c` | Context lifecycle (embeds `n00b_logic_t`), built-in primitives, interface/impl/promotion registration as Datalog facts | Phase 1, `logic/logic_program.h` |
| **3** | `unify.c` | Union-find (find, unify, occurs check), constraint checking (queries Datalog for Implements/Promotes, uses CSP push/pop for speculative eval) | Phase 2 |
| **4** | `freeze.c` | Type freezing (generalization), instantiation with given_name preservation, deep copy | Phase 3 |
| **5** | `display.c` | Type-to-string (respects `given_name`), error-to-string | Phase 1 |
| **6** | `exhaust.c` | Sum decomposition and exhaustiveness algorithm | Phase 3 |
| **7** | `type_rules.c` | Rule text parser (slay module, depends on slay tokenizer) | Phase 1 |
| **8** | `infer.c` | Inference tree walk, rule evaluation, type propagation, post-inference CSP propagation | Phases 3, 4, 7 |

Each phase gets its own unit tests. Phase 3 (unification) is the
most critical — it should be heavily tested with edge cases:
- Var + Var, Var + concrete, concrete + concrete
- Recursive Param nesting (`list[list[int]]`)
- Constraint failures (implements, has_field, not)
- Open vs closed row unification
- Trail save/restore correctness

Phase 8 (inference walk) is integration testing against real
grammars with `@rule` annotations.

### 8.9 Hybrid architecture: union-find + Datalog + CSP

The type checker uses **three engines** that each handle what
they're best at. This isn't reinventing the wheel — it's using the
right tool for each subproblem.

#### What goes where

| Subproblem | Engine | Why |
|------------|--------|-----|
| Structural unification (Var+T, Param+Param, Record+Record) | **Union-find** (hand-written) | Types are structured terms, not integers. Union-find with path compression is the standard O(α(n)) approach. The CSP's integer domains can't model nested type structure. |
| Interface implementations (`dict` implements `Indexable`) | **Datalog** | Naturally relational: `implements(type, iface)`. Datalog's semi-naive fixpoint handles transitive queries (e.g., "what interfaces does `dict` satisfy?") without hand-coding graph traversal. |
| Promotion graph (`i32` promotes to `i64`) | **Datalog** | Promotion is transitive: `promotes(A, C) :- promotes(A, B), promotes(B, C)`. One Datalog rule replaces a hand-written BFS. The bridge then feeds promotion facts into CSP constraints. |
| OneOf narrowing, deferred constraints, row polymorphism | **CSP** | These are genuine constraint satisfaction problems. "`` `T `` is one of {int, float, string}" is a finite domain. "`` `R `` has at least fields .x and .y" narrows as more fields are discovered. AC-3 propagation handles constraint interaction automatically. |
| When/else speculative evaluation | **CSP push/pop** | The CSP store's `push_state`/`pop_state` already implements trail-based backtracking. We use it directly for speculative type unification instead of rolling our own trail. |

#### The logic program inside the type context

The `n00b_logic_t logic` field in `n00b_tc_ctx_t` is initialized
with three Datalog relations:

```c
// During n00b_tc_ctx_new():
n00b_logic_init(&ctx->logic);

// Relations.
ctx->rel_implements = n00b_logic_relation(&ctx->logic, r"implements", 2);
// implements(type_name, iface_name)

ctx->rel_promotes = n00b_logic_relation(&ctx->logic, r"promotes", 2);
// promotes(from_type, to_type)

ctx->rel_iface_param = n00b_logic_relation(&ctx->logic, r"iface_param", 3);
// iface_param(iface_name, param_name, type_id)
```

Registering an implementation becomes a Datalog fact:

```c
void
n00b_tc_impl(n00b_tc_ctx_t *ctx, n00b_string_t type, n00b_string_t iface, ...)
{
    n00b_logic_fact(&ctx->logic, r"implements", &type, &iface);
    // ... also record param bindings via iface_param facts ...
}
```

Registering a promotion:

```c
void
n00b_tc_add_promotion(n00b_tc_ctx_t *ctx, n00b_tc_type_t *from, n00b_tc_type_t *to)
{
    auto from_name = n00b_tc_type_name(from);
    auto to_name   = n00b_tc_type_name(to);
    n00b_logic_fact(&ctx->logic, r"promotes", &from_name, &to_name);
}
```

Before the first constraint check, the Datalog engine runs once:

```c
// Adds transitive promotion rule:
//   promotes(A, C) :- promotes(A, B), promotes(B, C).
// Then: n00b_logic_run_datalog(&ctx->logic);
```

#### CSP for deferred type constraints

When a Var is constrained but not yet bound, the constraint is
posted to the CSP store. Each type Var that has deferred constraints
gets a CSP variable whose domain represents its possible types.

For **OneOf** constraints: the domain is the set of type indices
that match the allowed types. AC-3 propagation narrows as other
constraints eliminate possibilities.

For **row polymorphism**: open records are modeled as CSP variables
whose domain represents "sets of required fields." When two open
records unify, their field sets merge and the CSP propagates to
check consistency.

For **Implements**: when a Var is constrained to implement an
interface, the CSP domain is narrowed to types that have an
`implements(T, iface)` fact in the Datalog store.

#### Why not all-CSP or all-Datalog?

**All-CSP fails** because the CSP store models integer domains, not
structured type terms. You can't represent "this variable is
`list[dict[string, int]]`" as a finite integer domain — the space
of possible types is open-ended and structurally recursive.

**All-Datalog fails** because Datalog computes relations over
ground facts, but type inference discovers facts incrementally.
Unification is inherently imperative — it mutates forward pointers.
You'd have to encode the entire union-find as Datalog facts and
re-derive on every unification step, which is slower and harder to
understand.

**The hybrid works** because:
- Union-find handles the 90% case (structural unification) with
  O(α(n)) amortized cost.
- Datalog handles the relational queries (who implements what, what
  promotes to what) with automatic transitive closure.
- CSP handles the constraint narrowing (OneOf, row polymorphism,
  deferred checks) with AC-3 propagation and push/pop backtracking.

The three engines interact through a clean boundary: the union-find
is the source of truth for what types *are*; Datalog is queried for
what types *can do*; CSP narrows what types *could be*.

#### Var name preservation

Type variables store the user's name as `n00b_option_t(n00b_string_t)
given_name`. When present, error messages and `display.c` show the
user's name (`` `T ``) instead of a generated one (`t_42`). When a
frozen type is instantiated, fresh Vars inherit the `given_name`
from the frozen type's parameters (see §8.4). This means:

```
error: cannot unify `T with `U
  `T (declared at line 3) is int
  `U (declared at line 7) is string
```

...instead of:

```
error: cannot unify t_42 with t_43
```

### 8.10 Putting it together: the full pipeline

```
1. n00b_tc_ctx_new()
     → allocates context, registers built-in primitives
     → initializes logic engine with implements/promotes relations

2. Grammar author registers:
     → interfaces + implementations (→ Datalog facts)
     → promotions (→ Datalog facts)
     → type rules (→ ctx->rules dict)

3. n00b_logic_run_datalog(&ctx->logic)
     → computes transitive closure of promotions
     → materializes all derived implements/promotes facts

4. Inference walk (§8.6)
     → bottom-up tree traversal
     → per-node: evaluate type rule
       → structural unification via union-find
       → interface checks via Datalog query
       → deferred constraints via CSP posting
       → when/else speculative eval via CSP push/pop
     → propagate types into parse tree nodes + symbol table

5. Post-inference CSP propagation
     → run AC-3 on remaining deferred constraints
     → check that all Vars are ground or validly constrained

6. Return n00b_tc_infer_result_t
     → errors, coercions, type context
```

Phase 8 (inference walk) is integration testing against real
grammars with `@rule` annotations.

---

## In Scope — Design TBD

### Flow-sensitive narrowing

After `if x != nil`, the type of `x` in the true branch should
exclude `nil`. After a pattern match on a sum variant, the variable
is narrowed to that variant. This requires the inference pass to
consume CFG edges, not just bottom-up tree structure. The
infrastructure exists (CFG is built, inference pass accepts it) but
the narrowing logic needs design.

Key questions:
- How do narrowings propagate across basic blocks?
- How do narrowings merge at join points (phi-like)?
- Do narrowings produce new type variables or mutate existing ones?
- How does this interact with `when/else` type rules?

Design will be a follow-up doc once the core inference pass works.

### Serialization and linking format

Frozen types (§8.4) need a wire format so compilation units can
export their type signatures and consumers can import them. Key
design questions:

- **Format.** Binary (compact, fast) vs text (debuggable)?
  Probably binary with a text dump tool.
- **Identity.** How are types identified across units? By name?
  By structural hash? Both?
- **Freshening.** When importing a frozen type, which Vars get
  freshened and which are shared? (A type parameter like `T` in
  `list[T]` is freshened per use site; a module-level type alias
  is shared.)
- **Constraint serialization.** Constraints reference types,
  interfaces, and Datalog facts. The format must handle forward
  references and cross-unit interface resolution.
- **Versioning.** Schema evolution for the wire format itself.

Defer until the core inference engine is working end-to-end.

### Multi-file / module-level type checking

Currently the type checker operates on a single parse result.
For multi-file programs:

- **Import resolution.** How does `import foo` locate `foo`'s
  type exports? File system convention? Package manifest?
- **Incremental checking.** Can we avoid re-checking unchanged
  modules? (Frozen type signatures are the natural cache key.)
- **Circular imports.** Two modules that reference each other's
  types. Requires either a two-pass approach or lazy resolution.
- **Namespace merging.** How do imported symbols interact with
  the local symbol table and type context?

Defer until single-file type checking is solid and the
serialization format is designed.

## Open Questions

1. **Variance.** Should parameterized types support co/contravariance
   annotations? (`list[+T]` = covariant, `fn[-A, +R]` =
   contravariant input, covariant output.) This affects subtyping
   and sum injection. Defer for now, but the representation should
   not preclude it.

2. **`@exhaust` flag on rule data structures.** The `@exhaust`
   annotation (§3.3) triggers exhaustiveness checking after a
   match scope, but the `n00b_tc_rule_t` / `n00b_tc_rule_case_t`
   structs (§8.5) don't carry this flag. Two options: (a) add
   an `exhaust_child` field to `n00b_tc_rule_t` naming which child
   to check, or (b) make it a property of the `@scope` annotation
   rather than the rule (the scope knows "check exhaustiveness on
   exit"). Decision deferred to implementation of Phase 8.

3. **Flow-sensitive narrowing.** Already covered in "In Scope —
   Design TBD" above. The key dependency is the CFG integration —
   the inference pass needs to walk CFG edges, not just tree
   structure. The type system primitives (sum subtraction, Not
   constraints) are already in place; the missing piece is the
   narrowing propagation logic at CFG join points.

## Explicitly Not In Scope

- **Recursive types.** Not necessary for our use cases. The occurs
  check prevents `'T = ... 'T ...` and we keep it that way.
