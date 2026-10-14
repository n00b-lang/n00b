# writ — Call Plan Design

The call plan is the physical lowering of one function on one platform. It
is computed once, by `writ_plan_build`, and every consumer reads it:

- the C, Rust-transport and n00b generators print it;
- the conformance-test generator derives test scenarios from it;
- n00bb reads it through `writ_model_fn_plan`;
- the run-time `ffi` module interprets it.

The contract says *what is true*; the plan says *what to do, in order*.
Nothing downstream re-derives shapes from the contract.

---

## 1. Principles

1. **One plan per (function, platform, binding options).** Plans are
   immutable once built, and they are arena-allocated in the session.
2. **A closed set of eighteen steps (§3).** Every consumer implements
   every step kind once. A construct the steps can't express is a plan
   design change, never a generator special case.
3. **Fixed control flow (§2.5).** The phases come in a fixed order, and
   the plan names every edge between them. Generators print one skeleton,
   not an arbitrary control-flow graph.
4. **One C call per plan.** Protocols that span several calls (open /
   iterate / close, short-write loops, asynchronous completion) belong to
   bindings, built from several plans.
5. **Everything by index.** Steps refer to logical items, physical slots,
   locals and outcomes by index, and to expressions by `writ_expr_id`.
   There are no pointers inside a plan.
6. **Direction-neutral.** An import binding runs the steps in order. An
   export binding runs each step's mirror (§6). One plan serves both sides
   of a boundary.
7. **Safe defaults, explicit choices.** Where a contract allows more than
   one realization, the plan records a **choice point** and takes the safe
   default unless the binding says otherwise (§7).

---

## 2. The parts of a plan

### 2.1 Logical items

What a binding presents to its caller: the parameters, the result, and
the error detail. Slots that exist only to carry another slot's length,
capacity or context have no logical item of their own.

| Field | Meaning |
|---|---|
| `name` | Symbol |
| `role` | `input`, `output`, `input_output`, `result`, `error_detail`, `pack` |
| `shape` | `scalar`, `view` (enum, bool or newtype over a scalar), `handle`, `optional`, `bytes`, `text`, `array`, `record`, `rest_pack`, `keyword_pack` |
| `type` | `writ_type_id` |
| `after_call` | `borrowed`, `caller_owns`, `callee_owns`, `borrowed_until`, `borrowed_with`, `static` |
| `release` | Release function, when `after_call` is `caller_owns` |
| `span_events` / `span_with` | Event list or item index, for `borrowed_until` / `borrowed_with` |

### 2.2 Physical slots

The C ABI parameters, in declaration order, followed by the return value.

| Field | Meaning |
|---|---|
| `c_type` | The C-level type: a scalar, a pointer or a by-value record |
| `pass` | `value`, `in_pointer`, `out_pointer`, `inout_pointer`, `variadic_tail` |
| `item` / `part` | The logical item it carries, and which part of it: `whole`, `pointer`, `length`, `capacity`, `length_out`, `elements` |
| `abi_class` | Argument-passing facts from the probes, when known |

A function declared with `...` (or with a `va_list` parameter) has exactly
one slot with `pass = variadic_tail`, after its fixed slots (§5).

### 2.3 What the caller gets back

A binding's result is assembled from the plan, by these rules:

1. The C return value is a result, **unless outcomes consume it**. A
   status code tested by `classify` decides the outcome and never reaches
   the caller as a value.
2. Each `out` item whose storage the plan allocates is a result.
3. One result is returned directly. Several are returned together, as a
   tuple in Rust or as multiple values in n00b. None means the function
   returns nothing.
4. `inout` items are never results. The caller provided their storage,
   so they stay parameters and are updated in place.

Memory is copied only when it belongs to the library: results borrowed
`until` an event or `with` another object. Storage the caller provided
(`inout` references, and output buffers the caller chose to supply) is
never copied. Output buffers the plan allocates are allocated in the
target language's memory, so handing them over is a move.

### 2.4 Locals

Temporaries the lowering needs: allocated output buffers, encoded text,
cells for `inout` lengths, expanded variadic arguments. Each local has a
type, and a release rule for every outcome and for the error edges.

### 2.5 Phases, outcomes and edges

| Phase | Runs | Holds |
|---|---|---|
| `prepare` | Once | Checks and lowering of inputs |
| `attempt` | Once, or repeatedly under `sizing` | Output allocation, variadic expansion, the call, the sizing decision |
| `classify` | Once per attempt | Picks exactly one **outcome** |
| one block per outcome | When that outcome is picked | Lifting outputs and the result, fetching detail, error-path ownership |
| `finally` | Always | Releasing locals that weren't handed over |

**Outcomes.** A plan has an ordered list of outcomes, each with:

| Field | Meaning |
|---|---|
| `name` | Symbol |
| `kind` | `success` or `failure` |
| `when` | Predicate over the result and outputs. The first match wins, and the last outcome may have no predicate (it is "otherwise"). |
| `retry` | Optional: loop back to `attempt`, bounded by the profile's retry limit |
| steps | The outcome's own lift, fetch, wrap and release steps |

A contract's plain `error P via X` produces two outcomes:
`error: failure when P` and `ok: success`. Contracts with more outcomes
declare them (§2.6). Bindings present the outcomes:

- several `success` outcomes become a result that carries the outcome
  (Rust: an enum; n00b: a tagged result);
- several `failure` outcomes become distinct error kinds.

**Edges.** These are the only edges in any plan:

| # | Edge | When |
|---|---|---|
| 1 | `prepare` → `finally` | A `check` fails. Reported as an argument error; the call is never made. |
| 2 | `attempt` → `attempt` | Sizing needs a bigger buffer (`size_query`, `size_retry`) |
| 3 | `classify` → `attempt` | The picked outcome has `retry`, and the bound isn't reached |
| 4 | `classify` → outcome block → `finally` | Normal completion |
| 5 | outcome block → `finally` | A lifting step fails after the call succeeded (invalid UTF-8, unknown value for a closed enum). Every owned output already wrapped is released first. Reported as a contract violation. |

**Plan-wide facts.** Two facts apply to the whole plan, not to one step.
Generators print them around the phases:

- **`serialize on <item>`.** The function takes a handle whose thread rule
  is neither `shared` nor `thread_affine` (`dsl.md` §4.3). Generators
  acquire the handle's lock before `prepare`, and release it after
  `finally`.
- **`reset <channels>`.** The function reports errors through a channel
  marked `reset before call` (`errno` for `strtol`, `readdir`,
  `getpwnam`). Generators clear those channels at the start of `prepare`,
  so a stale value can't be mistaken for a new failure.

### 2.6 Contract syntax for outcomes

Outcomes are a contract fact, added to `dsl.md` §6.4:

```writ
fn inflate(strm: inout *z_stream, flush: c_int) -> c_int
  outcome done     success when ret == Z_STREAM_END
  outcome progress success when ret == Z_OK
  outcome stalled  failure when ret == Z_BUF_ERROR  on stalled strm: partial
  outcome corrupt  failure when ret == Z_DATA_ERROR via strm.msg
  outcome failed   failure via ret as ZStatus
```

- `error P via X` is shorthand for one failure outcome named `error`,
  followed by an implicit `ok` success outcome.
- `on <outcome> x: ...` states an output's validity for one outcome.
  `on error x: ...` applies to every failure outcome.
- The checker requires the outcomes to cover every result value: the
  last outcome has no predicate, or the predicates are exhaustive over a
  closed enum view.

---

## 3. The step set

| Step | Operands | Does | Mirror (§6) |
|---|---|---|---|
| `check` | expr | Fails the call (edge 1) if the predicate is false: non-null items, lengths that fit their physical type, closed enum values | the same check, on incoming arguments |
| `pass` | item or expr → slot or local | A scalar, view, raw handle value, pointer, or computed value such as a capacity, unchanged | `take` |
| `split` | item → pointer slot, length slot | An array, `bytes` or `utf8` item to pointer and length. Empty yields a dangling non-null pointer, unless `null if empty` is set. | `join` |
| `encode_cstr` | item → local | Copies text, adds a NUL, rejects interior NULs | `decode_cstr` |
| `expand` | pack item → variadic tail | Expands a rest or keyword pack into variadic arguments (§5) | `collect` |
| `alloc_out` | item → local, capacity expr | Allocates an output buffer **in the target language's own memory** (a `Vec`, an n00b buffer), so `lift_buffer` hands it over without copying | `write_out` (writes into the caller's buffer, honoring its capacity) |
| `bind_out` | local → slot | Passes a local's address in an out or inout slot | `bind_out` |
| `call` | convention | Makes the C call, with the fixed or variadic calling convention | implementation call |
| `size_query` | substitutions, needs expr | `sizing query`: calls with the substitutions, then sizes the buffer | answer the query |
| `size_retry` | needs expr | `sizing retry`: if the total exceeds the capacity, reallocates and repeats `attempt` (edge 2) | report the total |
| `classify` | outcome list | Evaluates outcome predicates in order and picks one (edges 3, 4) | produce the outcome's result and channel values |
| `fetch_detail` | channel or expr → error item | Reads detail immediately after the call | set the channel |
| `lift_value` | slot → item | Applies the success-domain view and `none` mapping | `lower_value` |
| `lift_buffer` | local → item, length expr | Sets an output buffer's length to the reported length and hands it over. This is a move, not a copy. | `fill_buffer` |
| `wrap_handle` | slot → item | Wraps an owned result with its release function | `box_handle` |
| `copy_borrowed` | slot → item | Copies a result that points into library-owned memory before its span can end. Never applied to storage the caller provided. | `lend` |
| `mark_consumed` | item | Marks a consumed input as dead in the caller | `adopt` |
| `release_local` | local | Frees a temporary not handed over | same |

That is eighteen steps. A run-time interpreter of plans is a `switch`
over them, and each generator is a printer of them.

The three lifting steps stay separate on purpose. Each does different work
at run time: wrap with a release function, copy before a span ends, or
convert in place. Folding them into one step with a flag would only move
that `switch` into every generator.

---

## 4. Worked examples

These use the Slice 0 contract:

```writ
library zlib
link "z"
header <zlib.h>

const Z_OK: c_int = 0

fn compressBound(sourceLen: c_ulong) -> c_ulong  pure

fn compress2(dest: out *[u8; cap destLen.in, len destLen.out], destLen: inout *c_ulong,
             source: bytes[sourceLen], sourceLen: c_ulong, level: c_int) -> c_int
  sizing needs compressBound(sourceLen)
  error ret != Z_OK via ret

fn uncompress(dest: out *[u8; cap destLen.in, len destLen.out], destLen: inout *c_ulong,
              source: bytes[sourceLen], sourceLen: c_ulong) -> c_int
  error ret != Z_OK via ret
```

`sizing needs E` gives the capacity before the call, from an expression
(`dsl.md` §6.1).

### 4.1 `compress2`

| Logical item | Role | Shape | After call |
|---|---|---|---|
| `source` | input | `bytes` | `borrowed` |
| `level` | input | `scalar` | — |
| `dest` | result | `bytes` | `caller_owns` (the binding's own memory) |
| `status` | error_detail | `scalar` | — |

| Slot | C type | Pass | Carries |
|---|---|---|---|
| 0 | `uint8_t *` | `out_pointer` | `dest` · pointer |
| 1 | `unsigned long *` | `inout_pointer` | `dest` · capacity in, length out |
| 2 | `const uint8_t *` | `in_pointer` | `source` · pointer |
| 3 | `unsigned long` | `value` | `source` · length |
| 4 | `int` | `value` | `level` |
| ret | `int` | `value` | `status` |

| Local | Type | Released |
|---|---|---|
| L0 | `[u8; cap]` | Handed to `dest` in `ok`; freed in `error` |
| L1 | `c_ulong` | In place |

```
prepare   split          source -> s2, s3
          pass           level  -> s4
attempt   alloc_out      dest   -> L0, cap compressBound(s3)
          pass           cap(L0) -> L1
          bind_out       L0 -> s0
          bind_out       L1 -> s1
          call           fixed
classify  error: failure when ret != Z_OK
          ok:    success
error     fetch_detail   ret -> status
ok        lift_buffer    L0 -> dest, len L1
finally   release_local  L0 unless handed over
```

The generated C shim, the n00b wrapper and the conformance test are three
printings of these lines.

### 4.2 `uncompress`

`uncompress` has no sizing protocol: zlib can't tell the caller how big
the output will be. The plan therefore turns the output's capacity into a
logical **input** that the caller supplies:

| Logical item | Role | Shape |
|---|---|---|
| `source` | input | `bytes` |
| `dest_capacity` | input | `scalar` (derived from `cap destLen.in`) |
| `dest` | result | `bytes` |
| `status` | error_detail | `scalar` |

The steps are those of §4.1, with `alloc_out dest -> L0, cap dest_capacity`.
A caller that passes too small a capacity gets `Z_BUF_ERROR` as its error
detail.

### 4.3 `inflate` (several outcomes)

With the outcomes of §2.6:

```
prepare   pass           strm -> s0
          pass           flush -> s1
attempt   call           fixed
classify  done:     success when ret == Z_STREAM_END
          progress: success when ret == Z_OK
          stalled:  failure when ret == Z_BUF_ERROR
          corrupt:  failure when ret == Z_DATA_ERROR
          failed:   failure
done      lift_value     ret -> result
progress  lift_value     ret -> result
stalled   lift_value     ret -> status
corrupt   fetch_detail   strm.msg -> detail
failed    fetch_detail   ret -> status
```

A Rust binding returns `Result<Inflated, InflateError>`, where `Inflated`
is `Done | Progress` and `InflateError` is
`Stalled | Corrupt(String) | Failed(ZStatus)`.

### 4.4 A sizing retry (Slice 2)

For resharp's `find_all`, the `attempt` phase is:

```
attempt   alloc_out   out -> L0, cap initial_guess
          bind_out    L0 -> s3
          pass        cap(L0) -> s4
          call        fixed
          size_retry  needs ret
```

`size_retry` loops back to `alloc_out` with capacity `ret` whenever
`ret > cap(L0)`. `initial_guess` comes from the binding's options, with a
default from the profile.

---

## 5. Variadic calls

A plan describes a variadic function once, whatever the number of
arguments a particular call passes. The pack is a logical item with role
`pack`, carried by the single `variadic_tail` slot. `expand` turns it into
arguments, by the pack's element rule from `dsl.md` §6.6:

| Element rule | `expand` produces |
|---|---|
| `... of T until s` | Each element as `T`, then the sentinel `s` |
| `... of T count n` | Each element as `T`. The count goes in its fixed slot, through `pass`. |
| `... pairs(K, V) until s` | Key, value, key, value, …, then `s` |
| `... pairs(K, by_name) until s` | The same, with each value's type taken from the `keys` table |
| `format printf(fmt)` | Under the default choice (§7): the formatted text as one `cstr` argument, with `fmt` replaced by `"%s"`. Under pass-through: one argument per conversion in the format, typed by the conversion. |

Every argument gets C's default argument promotions (`float` to `double`,
narrow integers to `int`) as it is expanded. The `call` step uses the
variadic calling convention whenever the function is variadic.

**How many arguments.** The plan is arity-generic, and each consumer
fixes the arity in its own way:

| Consumer | Arity known | Realization |
|---|---|---|
| A generator for a statically typed target (C, n00b, Rust) | At each call site | Prints one direct call per distinct arity used, through a shim per arity when the call needs one |
| The run-time `ffi` module and dynamic targets | At run time | A dynamic variadic call (libffi, or writ-generated thunks; `n00b-integration.md` §6) |

**`va_list` functions.** A function taking `va_list` is called through a
generated C wrapper with a `...` signature, which calls `va_start` and
forwards the `va_list`. The plan's `call` targets that wrapper. So a
`va_list` function is, to every consumer, an ordinary variadic call.

**The export direction.** `collect` is `expand`'s mirror. A generated C
entry point with the real `...` signature walks its arguments with
`va_arg`, by the same element rule, and builds the pack before calling
the implementation. It needs the element types to be known, so a
`by_name` pack requires its `keys` table here too.

---

## 6. Mirrors: the export direction

An export binding implements a function that someone else calls through
the C ABI. The Rust transport's `extern "C"` functions and n00b's
`ffi_bind` exports are both export bindings. They walk the same plan with
each step's mirror (§3), in the same phase order:

```
prepare   join           s2, s3 -> source
          take           s4 -> level
attempt   write_out      dest into s0, capacity *s1
          implementation call
classify  the implementation's outcome selects the block
ok        fill_buffer    dest -> s0; length into *s1
error     set the error channel, or return the status
```

Because both directions come from one plan, a conformance test can drive
an exported implementation through an imported binding of the same
contract, and check that every scenario round-trips.

---

## 7. Choice points

Where the contract allows more than one realization, the plan records the
choice and its default. A binding can change the choice through
`writ_plan_options_t`; a null options pointer means "all defaults".

| Choice point | Default | Alternative |
|---|---|---|
| `out` items (§2.3) | Promoted to results, in plan-allocated storage | Caller-supplied storage, kept as a parameter, never copied |
| Results in library-owned memory (`until`, `with`) | `copy_borrowed` | Lend, with the span enforced by the target language (Rust lifetimes). Rejected for languages whose profile can't enforce spans. |
| Output capacity with no sizing protocol | Caller-supplied input (§4.2) | A fixed capacity from the binding |
| Initial capacity under `sizing retry` | From the profile | From the binding |
| `format printf` | Format in the target language, pass `"%s"` | Pass-through, for compile-time-constant formats only |

Extended-tier constructs (policy parameters) add choice points when they
move into the core tier.
