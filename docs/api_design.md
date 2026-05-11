# C API design for n00b

A short manifesto for anyone designing or reviewing a public n00b C
API.  These principles aren't aspirational; they're the standard.  If
you're proposing a new API and one of these is hard to satisfy, that's
a flag — say so explicitly and explain the tradeoff.

n00b's reason to exist is to give working engineers access to
sophisticated capabilities — type systems, parsers, ML, async I/O,
GC — *without compromising on power, flexibility, or performance, and
without making them feel like a beginner.*  Every API decision should
make that bargain easier to keep.

---

## The bargain

When a developer reaches for an n00b API, the contract is:

1. **The 80% case is one or two lines** with no setup ceremony.
2. **The 20% case stays accessible** in the same source file via the
   same conceptual vocabulary — no separate "advanced" library.
3. **Power, performance, and safety are defaults**, not opt-ins.
4. **Reading the call site teaches you the API** — names and kwargs
   reveal intent without a doc lookup.

Every principle below is in service of that bargain.

---

## Principles

### 1. Easy and powerful are not opposites

The single biggest C API failure mode is choosing between an
"approachable" surface (lots of wrappers, hides power) and a "real"
surface (lots of primitives, hides ergonomics).  n00b refuses the
choice.

A good n00b API has *one* set of types that read naturally for
beginners and stay reachable for experts.  Beginners use the typical
constructors and methods; experts pass kwargs to opt into power.  No
one has to learn a different vocabulary to graduate.

> **Anti-pattern:** `lib_simple.h` and `lib_advanced.h`.
> **Pattern:** one header, kwargs unlock advanced usage on the same
> calls.

### 2. The 80% case is one or two lines

Common usage should look like:

```c
some_thing_t *t = n00b_thing_new();
n00b_thing_do(t, input);
```

If your API needs three or more setup statements before the user can
do useful work, the API has too much ceremony.  Default values,
kargs, and ergonomic constructors make this disappear.  When you
*must* require explicit setup (because there's a real choice the
user is making), make it visible in the call site, not hidden in
mutator calls afterwards.

### 3. Errors are values, not exceptions or magic sentinels

Anything that can fail returns `n00b_result_t(T)`.  Anything that
can be absent returns `n00b_option_t(T)`.  No `errno`, no `NULL`-as-
"failure-or-absent" ambiguity, no magic `-1` returns.  Every error
type has a string accessor (`n00b_xxx_err_str(err)`) so debug output
is self-documenting.

```c
n00b_result_t(my_t *) r = n00b_my_load(blob);
if (n00b_result_is_err(r)) {
    fprintf(stderr, "%s\n", n00b_my_err_str(n00b_result_get_err(r)));
    return 1;
}
my_t *m = n00b_result_get(r);
```

### 4. Mutability is opt-out

Containers and facades are thread-safe by default.  Each carries an
optional `n00b_rwlock_t *lock` field; default constructors allocate
one.  An `_kargs` knob (`.no_lock = true`, or a `_private` variant
constructor) lets users opt out when they've reasoned about ownership.

The cost of the default lock is negligible; the cost of *forgetting*
to lock data shared across threads is a Tuesday-afternoon SEGV that's
hard to diagnose.  Default toward safety.

### 5. Defaults are sensible; overrides are loud

Pick defaults that work for the typical case.  Document them in the
header next to the kwarg.  When a user overrides, the kwarg name in
their call site explains what's going on:

```c
n00b_ml_trainer_new(.learning_rate = 0.1f, .l2 = 1e-4f)
```

is self-describing.

> **Anti-pattern:** unnamed-positional-argument trains where the
> third float is "the regularizer."
> **Pattern:** `_kargs` with explicit names; defaults that match the
> doc.

### 6. Facades + primitives, both public, both first-class

Build a layered API: a facade type that bundles common primitives
into a single ergonomic shape, and the underlying primitives
themselves accessible through the facade's struct fields.

```c
typedef struct {
    n00b_ml_feature_config_t *config;   // primitive, public field
    n00b_ml_model_t          *model;    // primitive, public field
    // ... + hyperparameters + lock
} n00b_ml_trainer_t;
```

Beginners use the facade.  Experts construct the primitives
directly when they need to.  Nobody has to learn a separate
"internal" or "advanced" namespace.  *Don't hide what you don't
have to.*

### 7. Naming reveals intent — and minimizes jargon

The single biggest barrier to API adoption is specialist vocabulary
in names.  Domain-of-art terms feel "more correct" to the person
writing the API; they feel like a language barrier to the person
*reading* it.  Pick names that point at what the user gets, not at
how the code works.

Two structural rules:

- Functions are verbs: `n00b_xxx_observe`, `n00b_xxx_predict`,
  `n00b_xxx_save`.
- Types are nouns: `n00b_ml_trainer_t`.
- Variants of the same operation differ by *one* clear suffix:
  `_cstr` (takes a C string), `_private` (no lock).
- Kwargs are short and concrete (`.threshold`, `.allocator`).  Never
  `.flags`, `.opts`, `.config` if you can name the actual thing.

Three tests for any name before you commit it:

- **The 6-month test.**  A reader finds a call site in their own
  code half a year from now.  Can they tell what it does without
  the docs?  If `top_k(t, 8)` makes them Google what "top-k" means
  *in this context*, that's a name failure.
- **The non-specialist test.**  Show the name to a competent
  engineer outside your domain.  Can they guess what it does?  If
  they need a glossary, rename.
- **The value-not-implementation test.**  `top_k` describes how the
  function is implemented (find the K-largest); `strongest_rules`
  describes what the caller gets.  Names should point at outcomes.

**Avoid jargon abbreviations.**  Spell out `learning_rate`, not
`lr`.  Spell out `weight_decay`, not `l2`.  Two seconds of typing
once vs. a Stack Overflow search every time a new reader hits the
call site.

Some specific replacements that come up in ML/DS APIs.  Whenever
you see the term in the left column, default to the right unless
there's a *strong* reason not to:

| Avoid | Prefer | Why |
|---|---|---|
| `subspace` | `rule_group` (or `feature_group`) | The user is grouping rules they propose, not doing linear algebra. |
| `top_k` | `strongest_rules` | `k` is a placeholder; the user wants the strongest. |
| `feature` (in user-facing names) | `rule` | Engineers think in rules; "feature" is a stats term. |
| `sample` | `input` | What you're classifying is an input. |
| `loss` (as a function-name part) | drop or expose as `error_kind` | Almost nobody outside ML knows the term. |
| `SGD` / `gradient` | `train_step` | Implementation detail, not value. |
| `sigmoid` (in user-facing names) | `probability` | Math leak. |
| `lr` | `learning_rate` | Two-letter abbreviations cost more than they save. |
| `l2` | `weight_decay` | Tells the user what the knob does. |
| `epoch` | `pass` (or describe duration in samples) | "Epoch" is jargon. |
| `inference` | `predict` / `score` / `classify` | Sounds clinical; the verb is what the user does. |

Domain experts will still recognize the fancy terminology from your
doc comments and your `@kw` defaults.  Beginners get to read the
call site without a glossary.  Both win.

### 8. One way for the common case; escape hatches for the rest

For each piece of common usage, there should be exactly one obvious
function or method.  If two paths exist for the same thing, pick
one to be primary and document the other as the escape hatch.

> **Anti-pattern:** four ways to register a feature subspace.
> **Pattern:** one primary registration call with optional kwargs;
> a `_cstr` convenience variant only when string-allocation
> elision matters in hot paths.

### 9. Composability over configurability

Many small types that compose cleanly beat one God-type with
thirty kwargs.  Compare:

```c
// Composable
n00b_ml_scorer_t  *s = ...;
n00b_ml_layered_t *l = n00b_ml_layered_new(s);
n00b_ml_monitor_t *m = n00b_ml_monitor_new(s);

// Config-bag (don't)
n00b_ml_classifier_new(.frozen = true,
                       .with_layered = true,
                       .with_monitor = true,
                       .lr = 0.005,
                       .shadow_lr = 0.05,
                       .feedback_lr = 0.005, ...);
```

The composable version is more code at the call site, but each line
*means* something.  The config-bag version hides whether features
interact, which configurations are valid, and which kwargs apply to
which subsystem.

### 10. Allocator-aware, GC-by-default, zero ceremony

Every constructor accepts `.allocator` as a kwarg.  Default is the
GC arena.  Power users override when they need bump-allocation,
arenas, pools, or aligned allocators:

```c
n00b_ml_vec_t *fast = n00b_ml_vec_new(64, .allocator = &my_pool);
```

No `_alloc_`/`_free_` parameter pairs, no separate allocator-passing
APIs.  GC is the default and the typical user never thinks about it.

### 11. Threading is a feature, not a requirement

A synchronous-feeling call should work on a single thread without
the user knowing about n00b's runtime threading.  Threading-aware
versions exist (rwlocks on facades, `n00b_thread_spawn`, the conduit
service threads) but they don't *replace* the simple form.

A user must never be required to know about STW, register threads,
or pick locking modes to use a basic API.  Threading is opt-in;
default behavior is "works on the calling thread."

### 12. State is locatable, not hidden

If a struct has fields, put them in the public typedef.  If a
struct has no public fields, make it opaque (forward-declared
typedef).  *Never* expose a typedef and then mark some fields "for
internal use" — that's the worst of both worlds.

If a facade owns primitives, expose the primitive pointers as
public struct fields.  Power users will reach for them; the cost of
exposing them is roughly zero.

### 13. Save/load is part of the contract

Anything you can build, you can save.  Anything you can save, you
can load.  Both ends are versioned, self-describing blobs:

- `n00b_buffer_t *blob = n00b_xxx_save(thing);`
- `n00b_result_t(xxx_t *) r = n00b_xxx_load(blob);`

Loaders validate magic, version, and structural consistency before
they construct.  A truncated, version-mismatched, or otherwise
invalid blob produces a clean error, not a partially-initialized
object or a SEGV.

When a domain has multiple facades that wrap the same underlying
data (e.g., trainer / scorer / layered / monitor over a shared
config + model), one save format covers them all.  Use flag bits
to mark optional tails.  A reader for the simpler shape ignores
tails for the more complex one and gets a working object back.

### 14. Doxygen is the primary docs medium

Every public function gets a Doxygen block in its header,
including:

- One-sentence description
- Each kwarg, with default value and meaning
- `@pre` / `@post` for any non-obvious invariants
- Cross-references to related calls

The kwarg defaults in code and in Doxygen *must* match.  If they
don't, the doc is wrong; fix it before the next commit.

### 15. The implementation teaches the idioms

Reading the source should teach n00b conventions: how `_kargs` work,
how facades nest primitives, how rwlocks integrate, how
save-and-load is versioned, how thread handoff works.  Don't bury
patterns in macros that look like noise.  When a macro is the right
abstraction, name it precisely and document the expansion.

### 16. The blast radius of any one change is small

When you ship a new API, a follower API in the same domain should
be able to compose with it without reinventing the basics.  When
you bump a struct, blob, or vtable layout, version-bump the
relevant carrier and document the migration.  The library is meant
to be a stable platform, not a moving target.

---

## Checklist for new APIs

Before merging a public API, run through this.

**Surface**

- [ ] Is the 80% case ≤ 2 lines?
- [ ] Are defaults sensible?  Documented?
- [ ] Are advanced behaviors reachable via kwargs on the same calls?
- [ ] Is there exactly one primary function for each common
      operation?  (Are the variants actually justified?)

**Naming**

- [ ] Functions verbs, types nouns?
- [ ] Variants distinguished by one clear suffix?
- [ ] No `.opts` / `.flags` bag where named kwargs would do?

**Errors**

- [ ] Fallible operations return `n00b_result_t(T)`?
- [ ] Error type has a `_err_str` accessor?
- [ ] No `NULL`-as-error / `-1`-as-error / `errno` patterns?

**Threading**

- [ ] Default constructors thread-safe?
- [ ] `_private` / `.no_lock` opt-out for solo use?
- [ ] No thread-registration requirement for typical usage?

**State**

- [ ] Public struct fields are *intentional* and stable?
- [ ] No "for internal use" fields in public typedefs?
- [ ] Allocator-aware (`.allocator` kwarg) where allocations happen?

**Persistence**

- [ ] Save is `n00b_xxx_save(thing) -> n00b_buffer_t *`?
- [ ] Load is `n00b_xxx_load(blob) -> n00b_result_t(thing)`?
- [ ] Blob is versioned, validated, and shared across related
      facades?

**Documentation**

- [ ] Every public function has a Doxygen block?
- [ ] Defaults in code match defaults in docs?
- [ ] `@pre` / `@post` for non-obvious invariants?
- [ ] One end-to-end example in the header (or a `docs/<module>.md`)?

If a checkbox can't be ticked, the PR description explains why and
what the tradeoff is.

---

## Anti-patterns to avoid

- **Stringly-typed APIs.**  If a parameter is a string that must be
  one of N values, make it an enum.
- **Out-parameters as primary returns.**  Return values, not
  `int my_func(thing_t *t, result_t *out)`.  Out-parameters are
  acceptable for *secondary* outputs only.
- **Init/deinit pairs as the only way to use a type.**  Prefer
  constructor functions that return ready-to-use objects.
- **"Forgot to call init()" as a possible state.**  Make
  uninitialized state unreachable.
- **Overly opaque types** that hide trivially-public fields.  Be
  honest about what's public.
- **APIs where the call order matters and isn't obvious.**  The
  type system + return values should encode "what's allowed next."
- **APIs that require the caller to manage lifetimes manually.**
  GC handles most of it; finalizers handle the rest.  When manual
  lifecycle is unavoidable (e.g., file descriptors), make it
  symmetric: every `_open` has a matching `_close`.
- **APIs with hidden global state.**  Anything mutable goes in a
  struct.  Singletons are explicit (`n00b_get_runtime()` etc.).

---

## When to break the rules

You may have to.  Performance, ABI compatibility, or genuinely
unique constraints in a domain sometimes force a tradeoff.  When
you do break a rule:

1. Say so in the PR description.
2. Pick *which* rule you're breaking.
3. Explain why the alternatives are worse.
4. Document the rough edge in the header so future readers don't
   trip on it.

The goal isn't slavish compliance; it's *explicit* compliance.
