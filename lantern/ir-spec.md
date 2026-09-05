# IR Specification

**Status: draft, nothing frozen.** A proposal. No spelling here is a
compatibility obligation; §17 records what changed from the previous draft and
§16 lists what is still open.

A typed intermediate representation that sits between a front end's annotated
parse tree and a back end. Types come from typehash (`~/th`); layout does not.
Control flow is basic blocks; structured control flow is recovered when a back
end needs it. Non-SSA and SSA are the same IR under different established
properties, not two representations.

| | |
|---|---|
| Type model | typehash terms (`th_type`), identity by `th_hash` |
| Type syntax | typelang (`typelang.bnf`), unchanged |
| Control flow | basic blocks with terminators; regions as a derived overlay |
| SSA | a property of a function, established by a pass |
| Consumers | C emitter, MIR emitter, analyses, agents |
| Persisted form | text, round-trippable |

---

## §1 Purpose

### 1.1 What it is for

Four consumers, and the representation answers to all of them:

- **A C back end.** bCe compiles to C. C needs structured control flow, so the
  IR must be able to hand it structure, and must not assume it everywhere else.
- **A MIR back end.** MIR is register-and-block shaped and gives execution —
  JIT, interpretation, and running a test without a C compiler in the loop.
- **Analyses.** slay already has a CFG, a data-flow graph, a control-dependence
  graph and six diagnostics built over *parse-tree nodes*
  (`n00b/include/slay/{cfg,dfg,cdg,analyze}.h`). Those want an IR underneath
  them, and moving them there is the first milestone, not a later phase.
- **Agents and people.** The IR is read, written, diffed and transformed by
  hand and by tooling. That makes readability and a lossless round trip
  load-bearing properties, not conveniences.

### 1.2 What it is not

- **Not a machine IR.** No registers, no calling-convention lowering, no stack
  frames. MIR is downstream and does that work.
- **Not a type checker.** The IR is checked *against* types it is given. A
  front end's inference happens before lowering.
- **Not the place where layout is decided.** §5 says where.
- **Not tied to one language.** bCe is the first front end; slay is the second.
  Nothing in §7 is a bCe feature.

### 1.3 Normative language

**must** and **must not** mark requirements. **should** marks a strong
recommendation. **may** marks a genuine implementation choice.

---

## §2 Design principles

1. **One spelling per concept.** If two constructions mean the same thing, one
   of them is a defect. This is the principle the previous draft violated most
   often, and most of §17 is its consequences.
2. **The abstract IR is normative; the surface syntax renders it.** Spellings
   are expected to change. §7's opcode table is the single source of truth from
   which the enum, the parser, the printer and the verifier's operand rules are
   generated, so changing a spelling is one edit.
3. **Every instruction has a typing rule.** An opcode without a rule in §7 is
   not in the IR. This is what makes checking a specification rather than a
   wish.
4. **Identity is typehash; representation is separate and settable.** §4 and §5.
5. **Automation fills what the user did not pin.** Layout, SSA naming, block
   naming and structure recovery are all automatic, and all overridable. An
   automation that cannot be overridden forces a fork the first time someone
   needs a different answer.
6. **Forms are properties, not representations.** SSA, structure and resolved
   layout are facts *established* about a module. Passes declare what they
   require and what they establish. §3.
7. **Lossless round trip.** `parse(print(m))` is `m`, and `print` is
   idempotent. This is testable, and it is the only thing that keeps a text
   format honest.

---

## §3 Forms and properties

A module carries a set of **properties**: facts that have been established
about it. A pass declares which properties it requires, which it establishes,
and which it invalidates. A back end declares which it requires.

| Property | Meaning |
|---|---|
| `parsed` | Structurally well-formed: blocks terminated, names resolved, operand counts right. |
| `typed` | Every value has a type and every instruction satisfies its §7 rule. |
| `ssa` | Every local has been promoted to values; joins are `phi`. §8. |
| `structured` | A region overlay exists and covers every block. §9. |
| `laid-out` | Every type reachable from the module has a resolved descriptor for the target. §5. |
| `no-critical-edges` | No edge runs from a block with several successors to a block with several predecessors. |
| `verified` | The full §12 rule set passes. |

Properties are monotone within a pass and invalidated explicitly. A pass that
edits instructions without declaring `invalidates ssa` is a bug the pass
manager **must** be able to catch, so the verifier is runnable between any two
passes.

```
promote      requires parsed, typed      establishes ssa
structure    requires parsed             establishes structured
layout       requires typed              establishes laid-out
split-edges  requires parsed             establishes no-critical-edges
```

Nothing in this document requires `ssa` in order to be well-formed. Non-SSA is
not a degraded form; it is the form a front end lowers into, and the form a
person writes by hand.

---

## §4 Types

### 4.1 typehash is the type model

The IR does not define a type representation. A type in the IR **is** a
`th_type *` as built by `~/th`, written in typelang syntax
(`~/th/typelang.bnf`) in the textual form, and compared by `th_hash`.

Consequences worth stating, because the previous draft got each of them wrong:

- **Type equality is hash equality.** Not a structural walk, not a name
  comparison. Two types are the same type when their digests agree, which is
  the one question typehash answers (`~/th/spec.md` §1.1).
- **There is one type notation.** `ptr[i32]`, `(int, *string) -> void`,
  `variant Result[i32, Error]`, `forall[\`a] Vec[\`a]` mean in the IR exactly
  what they mean in `typelang.bnf`. The IR adds no type syntax of its own.
- **Nominal types carry no definition in their identity.** `th_hash` of a
  `Named` node is a function of its name and arguments alone (`~/th/spec.md`
  §11). Members live in the term, and the IR module's type table is where a
  name's definition is found.
- **Aliases are resolved before the IR sees them** (`~/th/spec.md` §9.1).

### 4.2 What the IR requires from typehash

Two gaps stand between typehash as shipped and this specification. Both are
work in `~/th`, and the IR **must not** work around either by re-deriving type
structure from printed text.

**A read API.** `struct th_type` is private to `c/typehash.c` and the header
exports only `th_kind_of` and `th_is_scheme`. The verifier and every back end
must walk terms — a record's members, an indirection's referent and attributes,
a function's parameters and modes, a sequence's element and extent, a named
type's arguments. typehash's own §2 claims the term is "a real, decodable
serialization"; the C API has to honor that.

**A member lookup.** Field access is by name (§7.6), so resolving a name to a
member type must be a typehash call, not an IR-side scan.

Unification is **not** required. The IR checks types it is given; it does not
infer them.

### 4.3 The scalar profile

typehash takes no position on what a name means: `i32` is a name and nothing
more (`~/th/spec.md` §9.2). The IR needs to know that it is a signed 32-bit
integer in order to type `add` and to emit anything at all. That knowledge is
the **scalar profile**, and it belongs to the IR, not to typehash.

A machine class is one of:

```
sint(w) uint(w) float(w) bool ptr void aggregate opaque
```

The profile maps **`Named` types only**. Every other typehash kind gets its
machine class structurally, with no profile entry and no ambiguity:

| typehash kind | Machine class |
|---|---|
| `BitInt` | `sint(w)` or `uint(w)`, from the node's own width and sign |
| `Indirection` | `ptr` |
| `Sequence` | `aggregate` |
| `Record` | `aggregate` |
| `Function` | `ptr` when used as a value; a signature when used as a callee |
| `Sum` | `aggregate` |
| `Binder`, `Variable`, `Var` | `opaque` — see §4.4 |
| `Named` | profile lookup; `opaque` if absent |

A module names the profile it was lowered against. A profile is data, so a
front end supplies its own: bCe's names its C scalars, slay's names n00b's. A
`Named` type with no profile entry is `opaque`, which is legal — an opaque type
can be pointed at, passed and stored, and cannot be added.

A profile carries more than machine classes. It also carries the typing and
lowering rules for the language's own types, and any extension opcodes — §7.2.
One artifact answers both "what is this type" and "what do operations on it
mean", which is why an opaque type without rules can be moved around and not
operated on.

### 4.4 Generics

Generic types are carried through the IR. There is no monomorphization pass and
no requirement that a module's types be ground.

A type variable's machine class is `opaque`, which means its size is not known
at the IR level. The lowering consults the type: where a value's type is not
ground, it is passed **boxed**, and the back end inserts the box and unbox. A
back end therefore needs the type at every value — which it has, because every
value is typed — and needs no separate instantiation machinery.

This is why §7 has no `dynamic_sizeof`, `dynamic_typehash` or
`generic_call` opcodes. "The type argument is not ground" is a fact about the
operand's type, discoverable with `th_kind_of`, not a second opcode.

---

## §5 Representation descriptors

### 5.1 Why this is separate from the type

typehash deliberately excludes size, alignment, offsets, field order and
padding, because identity must survive a representation change and must be the
same under a cross-compile (`~/th/spec.md` §1.2, §12). That is right, and it is
also exactly why the IR has to carry the excluded facts itself: a back end
cannot emit a field access without an offset, and a garbage collector cannot
scan without a map.

So layout lives in the IR, in a **descriptor**, keyed by the type's `th_hash`
and scoped to a target. Two types with one identity and two descriptors are the
same type laid out differently. The type never changes; the descriptor does.

The previous draft put `tag_field_index`, `uses_niche` and a bitfield table
inside the type itself, which made two spellings of one type compare unequal
for layout reasons and made a second target inexpressible.

### 5.2 What a descriptor holds

```c
typedef struct {
    ir_layout_val_t size;
    ir_layout_val_t align;
    ir_member_layout_t *members;
    int n_members;
    ir_layout_val_t stride;
    ir_indirect_repr_t indirect;
    ir_variant_repr_t variant;
} ir_descriptor_t;

typedef struct {
    const char *name;
    ir_layout_val_t bit_offset;
    ir_layout_val_t bit_width;
} ir_member_layout_t;
```

Bit offset and bit width are recorded for **every** member, not only for
bitfields. A bitfield is then not a special case: it is a member whose bit width
is narrower than its storage, and whose type is already `bitint[n]` per
typehash §4.5. Anonymous padding has no name, contributes nothing to identity,
and is a descriptor fact only.

`variant` records where a variant's tag lives and whether a niche encoding is
in use. `indirect` records whether an indirection is fat and what it carries.

### 5.3 Unset, pinned, resolved

Every layout value is in one of three states. This is the general shape
principle 5 asks for, and it applies to more than layout.

```c
typedef enum { IR_LAYOUT_UNSET, IR_LAYOUT_PINNED, IR_LAYOUT_SYMBOLIC,
               IR_LAYOUT_RESOLVED } ir_layout_state_t;
```

- **Unset** — the `layout` pass computes it.
- **Pinned** — supplied through the API or the text form, and **must** be
  honored. The `layout` pass fills around a pin and **must** fail with a
  diagnostic if a pin is unsatisfiable, never silently override it.
- **Symbolic** — an expression the target's compiler resolves: `sizeof(T)`,
  `offsetof(T, f)`, `alignof(T)`. Target-independent, so a module carrying
  symbolic layout cross-compiles. The C back end may emit the expression
  verbatim.
- **Resolved** — a number for a named target.

A module may hold a mixture. `laid-out` for a given target means every value
reachable from the module is `PINNED` or `RESOLVED` for it. The MIR back end
requires that; the C back end does not, because C can carry the symbols.

The API sets pins before the automation runs:

```c
void ir_layout_pin_size(ir_module_t *m, th_type *t, uint64_t bytes);
void ir_layout_pin_align(ir_module_t *m, th_type *t, uint64_t bytes);
void ir_layout_pin_member(ir_module_t *m, th_type *t, const char *field,
                          uint64_t bit_offset, uint64_t bit_width);
void ir_layout_pin_variant(ir_module_t *m, th_type *t, ir_variant_repr_t r);
```

The intended use is partial: pin the two fields whose offsets a foreign ABI
fixes, or pin a size while debugging, and let the pass place the rest.

---

## §6 Module structure

### 6.1 Shape

```c
typedef struct ir_module_t   ir_module_t;
typedef struct ir_func_t     ir_func_t;
typedef struct ir_block_t    ir_block_t;
typedef struct ir_instr_t    ir_instr_t;
typedef struct ir_value_t    ir_value_t;
typedef struct ir_local_t    ir_local_t;
typedef struct ir_global_t   ir_global_t;
typedef struct ir_region_t   ir_region_t;
```

A **module** owns a type table (interned `th_type *` with descriptors), a
profile name, a target name, globals, functions, and its established
properties.

A **function** owns a typehash `Function` type, a linkage, an ordered list of
parameters, a list of locals, an ordered list of blocks whose first element is
the entry, an optional region overlay, and its own properties.

A **block** owns a name, an ordered list of instructions, and exactly one
terminator. A block has no parameters; joins are `phi` (§8.3).

An **instruction** owns an opcode, an optional result value, an ordered list of
operands, modifiers, a source location, and metadata.

A **value** is a parameter, an instruction result, a constant, or a reference to
a global or a function. Every value has a `th_type *`. A value is defined once.

### 6.2 Locals are variables; memory is memory

These are different things and get different mechanisms. This is the one place
the IR deliberately has two ways to hold a mutable thing, and they are not two
spellings of one concept.

A **local** is a named, typed, mutable variable. Reading it is writing its name
as an operand; writing it is naming it as an instruction's destination (§7.4).
A local has no address.

**Memory** is reached through a pointer. `load` and `store` access it, `field`
and `index` compute addresses into it, `alloca` produces a pointer to a fresh
stack object. Anything address-taken, and any aggregate accessed piecewise,
lives here.

`promote` (§8.2) converts locals into values and `phi`. It does not touch
memory. A separate, optional pass may promote non-escaping `alloca`s into
locals first.

### 6.3 Source locations

The previous draft had no location field anywhere, which makes a diagnostic
from verified IR impossible to attribute. Locations are therefore specified
here in full, including how they survive passes — an origin that is correct at
lowering and wrong after three passes is worse than none.

#### 6.3.1 Spans

```c
typedef struct {
    int32_t  file;
    uint32_t start_byte, end_byte;
    uint32_t start_line, start_col;
    uint32_t end_line,   end_col;
} ir_span_t;
```

A span is a half-open byte range plus the line and column of each end. Byte
offsets are carried because tooling wants them and recomputing them from line
and column requires the source; lines and columns are carried because humans
and `#line` want them. Both **must** describe the same range.

`file` indexes the module's **file table**, so a path is stored once. Index `-1`
is "unknown", which is legal.

Lines are 1-based, columns are 1-based and counted in **Unicode scalar values**,
not bytes — the same unit the front end's tokenizer reports, since typehash
already requires NFC-normalized source.

#### 6.3.2 Origins

A span says where something is. An **origin** says how it got there, and is what
makes an inlined or generated instruction attributable.

```c
typedef enum {
    IR_ORIGIN_SOURCE,
    IR_ORIGIN_INLINED_FROM,
    IR_ORIGIN_EXPANDED_FROM,
    IR_ORIGIN_LOWERED_FROM,
    IR_ORIGIN_GENERATED_BY,
} ir_origin_kind_t;

typedef struct ir_origin_t ir_origin_t;

struct ir_origin_t {
    ir_origin_kind_t  kind;
    ir_span_t         span;
    const char       *agent;
    ir_origin_t      *parent;
};
```

`agent` names the pass, macro, template or front-end construct responsible, and
is required for every kind but `IR_ORIGIN_SOURCE`. `parent` chains outward, so
an instruction inlined from a function that was itself instantiated from a
template carries both, in that order. A chain **must** be finite and **must not**
contain a cycle.

Reporting renders the innermost span as the primary location and each parent as
a note — "expanded from here", "inlined from here" — which is the only way a
diagnostic inside generated code is actionable.

#### 6.3.3 What carries one

Every instruction, block, local, global and function carries a span and an
optional origin chain. Types do not: a type is an identity and identities have
no location, which is the same reason typehash keeps declaration sites out of
the encoding (`~/th/spec.md` §4.5).

Locations are **not** part of any identity, are never compared for equality, and
an unknown location is legal.

#### 6.3.4 Propagation

A pass **must** follow these rules. They exist so that location quality is a
property the verifier can check rather than a matter of each pass's diligence.

| Situation | Rule |
|---|---|
| An instruction replaces another | Inherits its span and origin unchanged. |
| Several instructions fold into one | Takes the span covering all of them, in one file; if they span files, takes the primary operand's. |
| One instruction expands into several | Every result inherits the original's span; none is left unknown. |
| A pass creates something with no source counterpart | Takes the span of the nearest enclosing construct, with an origin of `GENERATED_BY` naming the pass. |
| A block is created | Takes the span of the source construct it implements — a loop header takes the loop's span. |
| A `phi` is created by `promote` | Takes the span of the local's declaration, with origin `GENERATED_BY "promote"`. |
| An instruction is moved | Keeps its own span. Position in the IR is not position in the source. |
| A function is inlined | Every inlined instruction keeps its own span and gains an `INLINED_FROM` origin naming the call site. |

**Monotonicity.** A pass **must not** turn a known location into an unknown one.
If an input instruction had a span, every instruction derived from it has one.
This is checkable, and §12 checks it.

Spans and origins survive the §10.3 round trip. A printer may abbreviate
repeated file indices, but no information is lost.

### 6.4 Annotations

Every instruction, block, local, global, function and module carries an
**annotation map**: namespaced keys to typed values. Annotations are how a
resolved decision gets written back onto the node that provoked it, so a later
pass or a back end reads a fact instead of recomputing it — and so a person
reading the IR can see what was decided.

```c
typedef enum {
    IR_ANNOT_BOOL, IR_ANNOT_INT, IR_ANNOT_STR,
    IR_ANNOT_TYPE, IR_ANNOT_VALUE, IR_ANNOT_LIST,
} ir_annot_kind_t;
```

**Keys are namespaced**, on the same rule as extension opcodes (§7.2): `ir.` is
reserved for this document, and everything else takes its profile's prefix —
`n00b.`, `bce.`. An unknown namespace is preserved verbatim through parse,
print and every pass; a pass **must not** discard annotations it does not
understand.

**Annotations round-trip.** They are printed and parsed like anything else,
because a rule's output that vanished on save and load would make the text form
lossy and would make a two-stage pipeline behave differently from a one-stage
one.

#### 6.4.1 Provenance

Each annotation is either **authored** — written by a front end or a person, and
carried through untouched — or **derived**, produced by a pass and valid only
while that pass's property holds.

```c
typedef struct {
    const char      *key;
    ir_annot_kind_t  kind;
    ir_annot_val_t   val;
    const char      *derived_from;
} ir_annot_t;
```

`derived_from` is null for an authored annotation and names a property (§3) for
a derived one. Invalidating a property invalidates every annotation derived from
it. This is what keeps a stale resolution from outliving the edit that
invalidated it — the failure mode where a back end reads a decision that was
true two passes ago.

Authored annotations are never invalidated. A pass that must contradict one
raises a diagnostic (§11) rather than overwriting it, which is §2.5 applied to
annotations rather than to layout.

#### 6.4.2 Rules annotate

A typing rule (§7.2) does not only accept or reject. It may attach annotations
to the instruction it typed, recording what it resolved:

```c
typedef struct {
    bool          ok;
    th_type      *result;
    ir_annot_t   *annots;
    int           n_annots;
} ir_rule_result_t;
```

Annotations a rule writes are derived from `typed`. Typical content: which rule
in §7.2's ladder fired, the concrete operation a language-level operator
resolved to, a coercion that has to be inserted, or whether an operand needs
boxing because its type is not ground (§4.4).

The lowering rule for the same instruction then reads the annotation instead of
repeating the dispatch, so the two hook points agree by construction rather than
by both implementing the same decision procedure.

Two reserved keys, since the verifier and the tooling depend on them:

| Key | Meaning |
|---|---|
| `ir.rule` | Which rule resolved this instruction. Makes §7.2's dispatch inspectable. |
| `ir.boxed` | Operands passed boxed because their type is not ground. |

---

## §7 Instructions

### 7.1 The opcode table is the source of truth

Each opcode is one row: mnemonic, operand arity and kinds, modifiers, and the
rule computing the result type. The enum, the parser, the printer, the
verifier's operand checks and this section are all generated from that table.
Adding an opcode without a typing rule is not possible, which is the point.

Notation in the rules below: `T`, `U` are types; `class(T)` is the machine
class from §4.3; `hash(T)` is `th_hash`. A rule of the form `a: T, b: T -> T`
means both operands must have identical type — identical by hash — and the
result has that type.

The rules in §7.3 onward are the **defaults**. They are not the whole story.

### 7.2 Rules are hooked, and dispatch on type

A typing rule is a function, not a fixed row, and it is resolved by dispatching
on the operand types. This is what lets a language give its own types their own
operations — `add` on a string meaning concatenation, `lt` on a language's
decimal type, `index` on a user container — without either inventing an opcode
per language or hard-coding one language's vocabulary into the IR.

```c
typedef ir_rule_result_t (*ir_rule_fn)(ir_ctx_t *, ir_instr_t *, ir_diags_t *);
```

A rule accepts or rejects, gives the result type, may raise diagnostics of its
own (§11), and may **annotate the instruction** with what it resolved — §6.4.2
defines `ir_rule_result_t` and what annotating means.

There are two hook points, because legality and lowering are different
questions:

- A **typing rule** decides whether an instruction is legal for its operand
  types and what its result type is. The verifier calls it.
- A **lowering rule** decides what a back end emits for a given opcode and
  operand type. A back end calls it.

**Resolution is a specificity ladder**, most specific first, and it is
deterministic:

| Order | Key | Example |
|---|---|---|
| 1 | Exact type identity, by `th_hash` | `add` on exactly `n00b.decimal` |
| 2 | `Named` constructor name, any arguments | `index` on `list[_]` |
| 3 | typehash node kind | `eq` on any `Record` |
| 4 | Machine class (§4.3) | `add` on any `sint(w)` — the built-in defaults |
| 5 | Opcode default | Fail with a diagnostic |

The first rule that matches wins. A rule may decline, in which case resolution
continues down the ladder, so a hook can specialize one case and delegate the
rest.

**Rules live in the profile.** §4.3's profile already carries what a `Named`
type *is*; it also carries what operations on it *mean*. One place answers both
questions about a language's types, and a module names its profile, so an IR
file plus its profile is self-contained.

**What a hook may not do.** A hook may not change an opcode's arity, its operand
kinds, or its spelling — those stay in §7.1's table. This is deliberate: it
means any IR file parses and prints identically regardless of which profile is
loaded, so the §10.3 round trip does not depend on the profile being present.
Only legality, result type and lowering are hooked.

**Extension opcodes.** A profile may register opcodes of its own. They **must**
be namespaced with the profile's name — `n00b.concat`, `bce.rpc_call` — so that
they cannot collide with §7 or with each other. A module using them is
parseable structurally without its profile, but cannot be verified or lowered
without it, and the verifier **must** say so rather than reporting the opcode as
unknown. Prefer a hooked rule on an existing opcode; reach for an extension
opcode only when no existing one has the right shape.

**Dispatch is inspectable.** Which rule fires for a given opcode and type is a
question the tooling answers directly, so a surprising diagnostic can be traced
to the rule that produced it rather than guessed at.

### 7.3 Constants

| Opcode | Operands | Result | Notes |
|---|---|---|---|
| `const` | literal, type | `T` | Integer, float, bool, null pointer, string. The literal must be representable in `T`. |
| `aggregate` | values..., type | `T` | A constant record, array or tuple. Operand types must match the members of `T` in typehash order. |
| `undef` | type | `T` | An unspecified value of `T`. Reading it is legal; nothing may depend on which value it is. |

Integer literals are written in the source radix and typed by their annotation;
there is no signed-64-bit-literal-then-cast dance. `const` with a `Named` type
whose machine class is `opaque` is an error.

### 7.4 Locals

`local` is a function-level declaration, not an instruction: it names a local,
gives its type and an optional initializer, and is function-scoped rather than
block-scoped.

Neither reading nor writing a local is an opcode.

- **Reading** is writing the local's name as an operand. It has the local's
  type.
- **Writing** is naming the local as an instruction's **destination**. Every
  instruction has an optional destination, which is either a fresh value
  (`%v: T`) or an existing local (`$x`), and the destination's type must equal
  the instruction's result type.

| Opcode | Operands | Result | Notes |
|---|---|---|---|
| `copy` | v: `T` | `T` | The only instruction whose whole job is to move a value. |

So `$x = add wrap $x, 1` writes a local, `%v: i32 = add wrap $x, 1` writes a
fresh value, and `$x = copy %v` moves one into the other. There is one way to
read a local and one way to write one, and no `set` opcode is needed — a store
to a local is not a different operation from any other instruction, only a
different destination.

After `promote`, a function has no locals and no local destinations.

### 7.5 Memory

| Opcode | Operands | Result | Notes |
|---|---|---|---|
| `alloca` | type, count? | `ptr[T]` | Stack allocation, freed at function exit. |
| `load` | p: `ptr[T]` | `T` | Modifiers: `volatile`, `atomic(ordering)`. |
| `store` | p: `ptr[T]`, v: `T` | — | Same modifiers. |
| `field` | p: `ptr[T]`, name | `ptr[U]` | `U` is the type of member `name` of `T`, from typehash. `T` must be a `Record` or a `Named` with a record definition in the type table. |
| `index` | p: `ptr[T]`, i: integer | `ptr[U]` | `T` must be a `Sequence` with element `U`, or `U` when the indirection is `INDEXABLE`. |
| `offset` | p: `ptr[T]`, i: integer | `ptr[T]` | Pointer arithmetic in units of `T`. Requires `INDEXABLE`. |
| `diff` | a: `ptr[T]`, b: `ptr[T]` | `sint` | Pointer difference in units of `T`. |
| `memcpy` | d: `ptr`, s: `ptr`, n: integer | — | |
| `memmove` | d: `ptr`, s: `ptr`, n: integer | — | |
| `memset` | d: `ptr`, v: `uint(8)`, n: integer | — | |
| `memcmp` | a: `ptr`, b: `ptr`, n: integer | `sint(32)` | |

`volatile` is a modifier on the *access*, not a property of the type. typehash
excludes it from types on exactly those grounds (`~/th/spec.md` §7), so this is
where it has to live, and the previous draft had nowhere to put it.

There is one pointer-offset opcode. The previous draft had three, two of which
it declared identical.

### 7.6 Aggregate values

| Opcode | Operands | Result | Notes |
|---|---|---|---|
| `extract` | v: `T`, name-or-index | `U` | Member of an aggregate *value*. |
| `insert` | v: `T`, name-or-index, x: `U` | `T` | Produces a new aggregate; does not mutate. |

Member names are resolved through typehash, never through an offset. Offsets
are §5's business and appear only in a back end.

### 7.7 Variants

Three opcodes, because there are three operations.

| Opcode | Operands | Result | Notes |
|---|---|---|---|
| `variant.make` | arm-name, payload: `U`, type `T` | `T` | `T` must be a `variant Named[...]`. The tag is `th_arm_tag(arm-name)`. |
| `variant.tag` | v: `T` | `uint(64)` | The arm tag, per typehash §8. |
| `variant.payload` | v: `T`, arm-name | `U` | Reading the wrong arm is undefined; a front end guards it. |

Option, result, `try`, `unwrap` and `typecase` are **lowerings onto these**, not
opcodes. Their expansions are front-end policy and belong in the front end's own
documentation. The previous draft spent roughly fifteen opcodes on these three
operations.

### 7.8 Arithmetic

| Opcode | Operands | Result |
|---|---|---|
| `add`, `sub`, `mul` | a: `T`, b: `T` | `T` |
| `div`, `rem` | a: `T`, b: `T` | `T` |
| `neg` | a: `T` | `T` |

`class(T)` must be `sint`, `uint` or `float`. Signedness is **not** in the
opcode; it is in the type, and the verifier and back ends read it from the
profile. There is one `div`, not a signed and an unsigned one.

Overflow behavior is a required modifier on the integer forms, so it is always
stated and never defaulted silently. All four answer one question: what happens
when the true result does not fit in `T`.

| Modifier | When the result does not fit | `u8`: `200 + 100` | `i8`: `-100 + -100` |
|---|---|---|---|
| `wrap` | Two's-complement wraparound. | `44` | `56` |
| `trap` | Traps at run time. | trap | trap |
| `sat` | Clamps to the nearest representable value. | `255` | `-128` |
| `undef` | Undefined behavior. | unspecified | unspecified |

`sat` is **clamping**, not rounding and not truncation: the result is pinned to
the type's maximum or minimum. Rounding is a separate axis and appears only on
float conversions (§7.11).

`div` and `rem` take a modifier for division by zero, `trap` or `undef`, and
signed `div` and `rem` also take one of the four above, because `INT_MIN / -1`
overflows.

This replaces three overlapping opcode families in the previous draft —
`IR_ADD` documented as trapping, `IR_ADDU` unchecked, and `IR_CHECKED_ADD` —
with one opcode and a modifier.

### 7.9 Bitwise

| Opcode | Operands | Result |
|---|---|---|
| `and`, `or`, `xor` | a: `T`, b: `T` | `T` |
| `not` | a: `T` | `T` |
| `shl`, `shr` | a: `T`, b: integer | `T` |
| `rol`, `ror` | a: `T`, b: integer | `T` |
| `clz`, `ctz`, `popcount` | a: `T` | `uint(32)` |
| `bitreverse`, `byteswap` | a: `T` | `T` |

`class(T)` must be `sint` or `uint`. `shr` is arithmetic when `T` is signed and
logical when unsigned — the type already says which, so there is one opcode.
A shift amount at or above the width is undefined unless the `mask` modifier is
present, in which case it is taken modulo the width.

### 7.10 Comparison

| Opcode | Operands | Result |
|---|---|---|
| `eq`, `ne` | a: `T`, b: `T` | `bool` |
| `lt`, `le`, `gt`, `ge` | a: `T`, b: `T` | `bool` |

Signedness comes from `T`. Float comparisons take an `ordered` or `unordered`
modifier. `min` and `max` are not comparisons and are not here; they are
`select` over a comparison, and a back end pattern-matches them.

### 7.11 Conversion

| Opcode | Operands | Result | Notes |
|---|---|---|---|
| `cast` | v: `T`, type `U` | `U` | Value-preserving conversion. |
| `bitcast` | v: `T`, type `U` | `U` | Reinterpretation. Sizes must agree once `laid-out`. |

`cast` covers integer widening and narrowing, integer/float conversion, and
pointer/integer conversion. Which one it is, is determined by `class(T)` and
`class(U)` — deterministically, so it is checkable — and the author does not
spell it.

Two independent axes, and a conversion takes a modifier from each axis it
actually has. Conflating them was a defect in an earlier draft of this section,
which spelled one of each `trunc`.

**Fit** — what happens when the value is not representable in `U`. Same four
modifiers as §7.8, same meanings: `wrap`, `trap`, `sat`, `undef`.

**Rounding** — how a value between two representable ones is chosen. Only float
sources have this.

| Modifier | Chooses |
|---|---|
| `to_zero` | Toward zero. C's float-to-int conversion. |
| `nearest` | Nearest, ties to even. |
| `floor` | Toward negative infinity. |
| `ceil` | Toward positive infinity. |

| Conversion | Fit | Rounding |
|---|---|---|
| Integer to wider integer | — | — |
| Integer to narrower integer | required | — |
| Float to integer | required | required |
| Integer to float | — | required |
| Float to wider float | — | — |
| Float to narrower float | — | required |
| Pointer and integer, either direction | — | — |
| `bool` and integer, either direction | — | — |

Sign extension versus zero extension on widening is not a modifier: the source
type's signedness already decides it.

`bitcast` is a different operation and keeps its own opcode: it changes the
type without changing the bits, and conflating it with a converting cast is how
a sign-extension bug gets written.

### 7.12 Select and phi

| Opcode | Operands | Result | Notes |
|---|---|---|---|
| `select` | c: `bool`, a: `T`, b: `T` | `T` | Both arms are evaluated. Not short-circuit. |
| `phi` | (block, value: `T`)... | `T` | §8.3. |

Short-circuit `&&`, `||` and `?:` are control flow and lower to blocks. They
are not opcodes; an operand that must not be evaluated cannot be an operand.

### 7.13 Calls

| Opcode | Operands | Result | Notes |
|---|---|---|---|
| `call` | callee, args..., named args... | return type of callee | |

The callee is a function reference or a value of pointer-to-function type;
there is no separate indirect-call opcode, because the operand's type already
distinguishes them.

Arguments are checked against the callee's typehash `Function` node, which
already carries positional parameters, an optional typed rest parameter, keyword
parameters and a mode per parameter (`~/th/spec.md` §4.6). So:

- Keyword arguments are **argument syntax**, not a second opcode. The previous
  draft's `IR_KWARGS_CALL`, its `kw_struct_type`, and §6's entire hand-rolled
  keyword-struct model are all subsumed by the type.
- A rest parameter is typed. C's `...` is a rest over `__ty.untyped`.
- `out` and `inout` parameters are checked for argument form.

Whether a keyword argument is passed in a struct, and whether a rest parameter
is a pointer and a count, is a back end's lowering decision recorded in §5's
descriptor.

### 7.14 Comptime and reflection

| Opcode | Operands | Result | Notes |
|---|---|---|---|
| `sizeof` | type | `uint(64)` | Symbolic until `laid-out`. |
| `alignof` | type | `uint(64)` | Symbolic until `laid-out`. |
| `offsetof` | type, name | `uint(64)` | Symbolic until `laid-out`. |
| `typehash` | type | `uint(64)` | `th_hash`. Always a constant — identity needs no target. |
| `typename` | type | `ptr[char]` | `th_print` of the canonical term. |
| `armtag` | arm-name | `uint(64)` | `th_arm_tag` of the name. Always a constant, per typehash §8. |

`typehash` and `typename` are translation-time constants under any target,
because typehash identity is target-independent. `sizeof`, `alignof` and
`offsetof` are §5.3 symbolic values that a target resolves — which is what lets
a module cross-compile.

There are no `dynamic_` variants. §4.4.

### 7.15 Terminators

Every block ends in exactly one.

| Opcode | Operands | Notes |
|---|---|---|
| `jump` | block | |
| `branch` | c: `bool`, then-block, else-block | |
| `switch` | v: `T`, (constant, block)..., default-block | `T` must be integer or `bool`. Case constants must be distinct. |
| `return` | v: `T`? | `T` must equal the function's return type; absent iff the return type is `void`. |
| `unreachable` | | Control provably does not arrive here. |

There is no `goto`, no `break` and no `continue` in the IR. Those are front-end
constructs that lower to `jump`, and keeping them would be a second control-flow
mechanism. The previous draft had `IR_BREAK` and `IR_CONTINUE` in both the value
enum and the statement enum.

### 7.16 What is deliberately not an opcode

Each of these was an opcode in the previous draft. Each is a front-end lowering
onto §7, and every one of them was a hole in the checker.

| Was | Is now |
|---|---|
| `IR_OPTION_*`, `IR_RESULT_*`, `IR_TYPECASE*` | §7.7 plus branches |
| `IR_KWARGS_CALL` and the kwargs struct model | §7.13; the type carries it |
| `IR_DEFER`, `IR_WITH`, `IR_ONCE` | Explicit blocks and a guard variable |
| `IR_GC_REGISTER/DEREGISTER/SCAN` | A `gcroot` attribute on locals and globals, plus a stack map derived from §5 |
| `IR_RPC_CALL`, `IR_BLUEPRINT_INSTANTIATE` | Front-end lowerings; `call` and ordinary types |
| `IR_GENERIC_CALL`, `IR_DYNAMIC_*` | §4.4 |
| `LOGIC_AND`, `LOGIC_OR`, `IR_TERNARY` | §7.12 |
| `CMP_MIN/MAX_*` | `select` over a comparison |
| `IR_ARRAY_LEN` | The extent is in the type |
| `IR_TYPEOF` | Every value has a type |

---

## §8 SSA

### 8.1 The two forms

Both are legal IR and both round-trip. The difference is one property.

A function **without** `ssa` holds mutable state in locals: a local is declared
once, written by naming it as a destination, and read by name. A function
**with** `ssa` has no locals and no local destinations; every value is defined
exactly once, and control-flow joins are `phi`.

The instruction set is otherwise identical. There is no second data structure,
no separate SSA module type, and no pass that rewrites control flow as a side
effect of SSA construction — which is where the previous draft's §7 went wrong,
stuffing phi nodes into declaration initializers and mixing labels and gotos
into a nested `if`.

### 8.2 `promote`

`promote` requires `parsed` and `typed`, and establishes `ssa`. It computes the
dominator tree, places `phi` at dominance frontiers, and renames.

**Naming is derived from the source name.** A local `x` becomes `%x_0`, `%x_1`,
`%x_2` … in dominance order, so a reader can follow a variable through the
promoted form and a diff between the two forms is readable. A name that would
collide gets a disambiguating suffix. This is automatic and, per §2.5,
overridable: a caller may supply a naming function.

Blocks introduced by the pass are named after their role and their source
construct — `loop.header`, `loop.body`, `if.join` — not numbered.

Locals whose address is taken are not promoted; they are `alloca` first.

### 8.3 `phi`

- A `phi` **must** appear in the leading run of instructions of its block, and
  nothing but `phi` may precede it.
- Its operand list **must** name every predecessor of the block exactly once,
  and no other block.
- Every incoming value **must** have the block's `phi` type.
- An incoming value **must** dominate the end of its named predecessor, not the
  `phi` itself.

Joins are `phi` and not block parameters. Both are standard and both work; two
mechanisms for one concept would violate §2.1, and `phi` is the one that was
asked for.

---

## §9 Regions

Blocks are normative. A **region overlay** is a derived tree over them that says
which blocks form an `if`, a `while`, a `for`, a `switch` or a sequence. It is
metadata: it points at blocks and changes no instruction.

```c
typedef enum { IR_REGION_SEQ, IR_REGION_IF, IR_REGION_LOOP,
               IR_REGION_SWITCH, IR_REGION_BLOCK } ir_region_kind_t;
```

`structure` requires `parsed` and establishes `structured`. It is the relooper.
For reducible control flow it finds the structure; for irreducible flow it
**must** still succeed, by emitting a dispatch loop over a state variable, and
**must** record that it did so, since a C back end that silently produced a
dispatch loop where a `while` was expected is a performance bug nobody can find.

The C back end requires `structured`. The MIR back end does not, and neither
does any analysis.

Where a region overlay exists, the printer **may** render blocks nested inside
region syntax for readability. That is a printing mode over one representation,
not a second representation; the round trip in §10.3 holds in either mode.

---

## §10 Textual format

### 10.1 Status of this section

The abstract IR in §6 through §9 is normative. This syntax renders it and is
**expected to change**. Because §7.1's table drives the parser and the printer,
a spelling change is a table edit. Nothing in the IR's meaning depends on the
characters chosen here.

### 10.2 Lexis

- `%name` — a value: an instruction result or a parameter.
- `$name` — a local variable.
- `@name` — a global or a function.
- Block names are bare identifiers, unambiguous because they appear only after
  `block`, `jump`, `branch` and `switch`.
- Types are typelang, verbatim, after a `:`.
- `#` begins a comment to end of line.
- Identifiers are Unicode, NFC, as typehash requires.

### 10.3 Canonical form and round trip

`print` renders a canonical form: one instruction per line, blocks in layout
order with the entry first, types printed by `th_print` so there is one
canonical spelling of a type across typehash and the IR.

Two properties are testable and **must** hold:

```
parse(print(m))  ==  m
print(parse(t))  ==  print(parse(print(parse(t))))
```

Comments and source locations survive a round trip. Whitespace does not.

### 10.4 Non-SSA

```
module sum {
  profile bce
  target  x86_64-linux

  func @sum(%n: i32) -> i32 {
    local $acc: i32 = 0
    local $i:   i32 = 0

    block entry:
      jump loop

    block loop:
      %more: bool = lt $i, %n
      branch %more, body, done

    block body:
      $acc = add wrap $acc, $i
      $i   = add wrap $i, 1
      jump loop

    block done:
      return $acc
  }
}
```

`$i` as an operand is the read of the local; `$acc` before an `=` is the write.
`add wrap` states the overflow behavior; there is no default.

### 10.5 SSA

The same function after `promote`, with the naming discipline of §8.2:

```
  func @sum(%n: i32) -> i32 {
    block entry:
      jump loop

    block loop:
      %acc_1: i32 = phi [entry: 0], [body: %acc_2]
      %i_1:   i32 = phi [entry: 0], [body: %i_2]
      %more:  bool = lt %i_1, %n
      branch %more, body, done

    block body:
      %acc_2: i32 = add wrap %acc_1, %i_1
      %i_2:   i32 = add wrap %i_1, 1
      jump loop

    block done:
      return %acc_1
  }
```

### 10.6 Types, records and variants

```
  type Point = (x: i32, y: i32)

  layout Point {
    size  = 8
    align = 4
    x @ bits 0..32
    y @ bits 32..64
  }

  func @dist2(%p: ptr[Point]) -> i32 {
    block entry:
      %px: ptr[i32] = field %p, "x"
      %x:  i32      = load %px
      %py: ptr[i32] = field %p, "y"
      %y:  i32      = load %py
      %xx: i32      = mul wrap %x, %x
      %yy: i32      = mul wrap %y, %y
      %r:  i32      = add wrap %xx, %yy
      return %r
  }
```

The `layout` block is §5.3 pinning. Omit it and the `layout` pass computes it;
supply part of it and the pass fills the rest around what was pinned.

```
  func @unwrap(%r: variant Result[i32, Error]) -> i32 {
    block entry:
      %t:  u64  = variant.tag %r
      %ok: u64  = armtag "ok"
      %is: bool = eq %t, %ok
      branch %is, good, bad

    block good:
      %v: i32 = variant.payload %r, "ok"
      return %v

    block bad:
      unreachable
  }
```

### 10.7 Grammar

Notation follows `n00b.bnf` and `typelang.bnf`: `<nonterminal>`, `%"literal"`,
`%TOKEN`, `::=`, `|`, `(...)?`, `(...)*`, `(...)+`.

```
<module>     ::= %"module" %IDENTIFIER %"{" <module-item>* %"}"

<module-item> ::= <profile-decl> | <target-decl> | <type-decl>
                | <layout-decl> | <global-decl> | <func>

<profile-decl> ::= %"profile" %IDENTIFIER
<target-decl>  ::= %"target"  %IDENTIFIER
<type-decl>    ::= %"type" %IDENTIFIER %"=" <typelang-type>

<layout-decl> ::= %"layout" <typelang-type> %"{" <layout-item>* %"}"
<layout-item> ::= %"size"  %"=" <layout-val>
                | %"align" %"=" <layout-val>
                | %"stride" %"=" <layout-val>
                | %IDENTIFIER %"@" %"bits" <layout-val> %".." <layout-val>
<layout-val>  ::= %INT_LIT | <symbolic-expr>

<global-decl> ::= <linkage>? %"global" %"@" %IDENTIFIER %":" <typelang-type>
                  (%"=" <const>)?

<func> ::= <linkage>? %"func" %"@" %IDENTIFIER
           %"(" <params>? %")" %"->" <typelang-type>
           %"{" <local>* <block>+ %"}"

<params> ::= <param> (%"," <param>)*
<param>  ::= <mode>? %"%" %IDENTIFIER %":" <typelang-type>
<mode>   ::= %"in" | %"out" | %"inout"

<local>  ::= %"local" %"$" %IDENTIFIER %":" <typelang-type> (%"=" <const>)?

<block>  ::= %"block" %IDENTIFIER %":" <instr>* <terminator>

<instr>  ::= <dest>? %IDENTIFIER <modifier>* <operands>?
<dest>   ::= %"%" %IDENTIFIER %":" <typelang-type> %"="
           | %"$" %IDENTIFIER %"="
<modifier> ::= %IDENTIFIER
<operands> ::= <operand> (%"," <operand>)*
<operand>  ::= <value> | <const> | <block-ref> | <string> | <typelang-type>
             | <named-arg> | <phi-arm>

<named-arg> ::= %IDENTIFIER %"=" <operand>
<phi-arm>   ::= %"[" %IDENTIFIER %":" <operand> %"]"

<value>     ::= %"%" %IDENTIFIER | %"$" %IDENTIFIER | %"@" %IDENTIFIER
<block-ref> ::= %IDENTIFIER

<terminator> ::= %"jump" <block-ref>
               | %"branch" <value> %"," <block-ref> %"," <block-ref>
               | %"switch" <value> <switch-arm>* %"default" %":" <block-ref>
               | %"return" <operand>?
               | %"unreachable"

<switch-arm> ::= %"[" <const> %":" <block-ref> %"]"

<linkage> ::= %"static" | %"extern" | %"inline"
```

Instruction mnemonics and modifiers are `%IDENTIFIER` in the grammar and are
validated against §7.1's table rather than enumerated here, which is what keeps
the two from drifting.

---

## §11 Diagnostics

One interface, used by typing rules (§7.2), the verifier (§12), the analyses
(§13) and the back ends (§14). A pass does not print; it reports, and something
above it decides what to do with the report.

slay has an accumulator of its own (`n00b/include/slay/diagnostic.h`) which this
supersedes rather than reuses. What is added over it: several labelled secondary
spans instead of one `related`, machine-readable rendering, suggested fixes, a
policy layer, and byte offsets in spans. slay's entries map onto this without
loss when the two merge.

### 11.1 Shape

```c
typedef enum {
    IR_SEV_FATAL,
    IR_SEV_ERROR,
    IR_SEV_WARNING,
    IR_SEV_NOTE,
    IR_SEV_HELP,
} ir_severity_t;

typedef struct {
    ir_span_t    span;
    const char  *label;
} ir_label_t;

typedef struct {
    ir_span_t    span;
    const char  *replacement;
    const char  *description;
} ir_suggestion_t;

typedef struct {
    ir_severity_t     severity;
    const char       *code;
    const char       *message;
    ir_label_t        primary;
    ir_label_t       *secondary;
    int               n_secondary;
    ir_suggestion_t  *suggestions;
    int               n_suggestions;
    ir_origin_t      *origin;
} ir_diag_t;
```

`FATAL` means the pass cannot continue and the pipeline stops. `ERROR` means the
output is invalid but analysis continues, so one run reports many problems.
`WARNING` leaves the output valid. `NOTE` and `HELP` attach to a preceding
diagnostic and never stand alone.

The **primary** label says what is wrong at the point it is wrong. **Secondary**
labels point at the other places that make it wrong — the conflicting
declaration, the predecessor a `phi` failed to cover, the pin a layout could not
satisfy. A diagnostic with a cause elsewhere and no secondary label is
incomplete, and this is the main reason slay's single `related` field was not
enough.

`origin` is the chain from §6.3.2, so a problem inside inlined or generated code
renders with the trail that produced it.

### 11.2 Codes

Every diagnostic **must** carry a code, and a code is stable once released:
messages get rewritten, codes do not, and the code is what a suppression, a test
and a documentation lookup key on.

```
IR-E0101    error, defined by this document
IR-W0203    warning, defined by this document
n00b-E0007  error, defined by the n00b profile
bce-W0011   warning, defined by the bCe profile
```

The prefix is `ir` for this document and the profile's name otherwise, matching
§7.2's namespacing for extension opcodes and §6.4's for annotation keys — one
namespacing rule, three uses.

### 11.3 Spans are required

Every diagnostic **must** have a primary span. A rule or pass with no natural
location uses the enclosing instruction's, then the block's, then the function's
— in that order, never "unknown". §6.3.4's monotonicity rule is what makes this
possible: a location that was known at lowering is still known here.

A diagnostic whose primary span is unknown is itself a bug, and the verifier
reports it as one.

### 11.4 Policy

Severity as reported is not necessarily severity as acted on. A **policy** maps
codes to outcomes, and is supplied by whatever is driving the pipeline:

| Outcome | Effect |
|---|---|
| `default` | As the producer reported it. |
| `allow` | Suppressed entirely. |
| `warn` | Demoted to a warning. |
| `deny` | Promoted to an error. |
| `forbid` | Promoted to an error that cannot be re-allowed by a narrower scope. |

Policy applies by code and by prefix, so a whole profile's warnings can be
turned up at once. `FATAL` is not subject to policy — a pass that cannot
continue cannot be told to continue.

### 11.5 Accumulation and rendering

```c
ir_diags_t *ir_diags_new(ir_policy_t *policy);
void        ir_diag_emit(ir_diags_t *, ir_diag_t *);
bool        ir_diags_failed(ir_diags_t *);
void        ir_diags_merge(ir_diags_t *dst, ir_diags_t *src);
```

Diagnostics accumulate rather than abort, except on `FATAL`. Rendering is
ordered by file, then by start offset, then by severity, so output is
deterministic and diffable — a test that compares diagnostic output must not
depend on pass scheduling.

Two renderings are required: a human one with source context and carets, and a
machine one carrying every field, including spans as byte offsets, so tooling
does not parse prose.

---

## §12 Verification

### 12.1 Graded

The verifier does not answer yes or no. It reports which of §3's properties
hold, so a back end can state its requirements and a pass manager can check
prerequisites between passes.

```c
ir_props_t ir_verify(ir_module_t *m, ir_diags_t *out);
```

Every diagnostic carries the location from §6.3 and names the rule it violates.

### 12.2 Rules for `parsed`

- Every block ends in exactly one terminator, and no terminator appears
  elsewhere.
- Every block reference resolves within the function.
- Every value reference resolves, and every value is defined exactly once.
- Every operand count and kind matches §7.1.
- Modifiers are legal for their opcode, and required modifiers are present.
- Entry is the first block and has no predecessors.
- `phi` placement satisfies §8.3.

### 12.3 Rules for `typed`

- Every value has a type, and every instruction satisfies the rule that §7.2's
  ladder resolves for its operand types, with type equality decided by
  `th_hash`.
- Every extension opcode present resolves in the module's profile. An
  unresolved one is reported as a missing profile, not as an unknown opcode.
- Every `Named` type used as a scalar has a profile entry, or is used only in
  ways `opaque` permits.
- `field` and `extract` names resolve to members of their aggregate through
  typehash.
- `call` arguments match the callee's `Function` node: positional arity, rest
  element type, keyword names, and parameter modes.
- `return` matches the function's return type.
- `switch` case constants are distinct and have the scrutinee's type.

### 12.4 Rules for `ssa`

- No locals and no local destinations.
- Every use is dominated by its definition.
- §8.3 in full.

### 12.5 Rules for `laid-out`

- Every type reachable from the module has a descriptor for the target with no
  `UNSET` values.
- No pin was violated.
- Member bit ranges do not overlap except where a variant's niche encoding says
  they may.
- `bitcast` operand and result sizes agree.

---

## §13 Analyses

These exist in slay over parse-tree nodes and move to the IR unchanged in
substance. This is the first milestone's real content.

| Slay today | On the IR |
|---|---|
| `n00b_cfg_t` over `n00b_parse_tree_t *` | The blocks *are* the CFG; edges come from terminators. |
| `n00b_dfg_t`, def/use keyed by variable name | Def/use keyed by value and local. Under `ssa`, def/use is immediate. |
| `n00b_cdg_t` | Unchanged in shape, over IR blocks. |
| `analyze.h` W001–W005, E001 | Same six diagnostics, now attributable via §6.3 locations. |

The CFG needs no construction pass: it is the block list plus terminators, and
`n00b_cfg_edge_kind_t`'s distinctions (`LOOP_BACK`, `LOOP_EXIT`, `CASE_BRANCH`)
become derived facts from the dominator tree and the region overlay rather than
things the builder must be told.

Dead-code detection, use-before-def and unused-variable analysis all become
cheaper under `ssa`, which is a reason the property exists and is not a reason
to require it.

---

## §14 Back ends

Not in the first milestone. Recorded so the representation is not shaped
against them.

| | Requires | Notes |
|---|---|---|
| C | `verified`, `structured` | May carry symbolic layout and let the target's C compiler resolve it. Emits `#line` from §6.3. |
| MIR | `verified`, `laid-out` | Blocks map to MIR blocks; no relooper. Gives execution — JIT and interpretation — so IR tests can run without a C compiler. |

Neither back end may make a decision the IR does not record. Where a back end
chooses a representation — a keyword-argument struct, a fat pointer, a variant
niche — the choice is written into §5's descriptor, so it is inspectable and
pinnable.

---

## §15 slay integration

Planned, not built. The IR comes first; this section records what the merge
will need so the IR is not shaped wrongly in the meantime.

slay's annotated BNF already produces most of what an automatic AST-to-IR
lowering needs. The mapping is close to total:

| Annotation | IR |
|---|---|
| `@branch($1,$2,$3)`, `@loop($3,$4)`, `@switch($1,$3)`, `@jump("break")` | Blocks and terminators; this is `n00b_cf_label_t` already |
| `@assigns($0,$2)`, `@varref($0)` | A local destination and a local read — already the DFG's def/use pair |
| `@declares($0,$2,"param")`, `@scope("block")` | Parameters and locals |
| `@call($0,$2)` | `call` |
| `@literal("int", type-spec)` | `const` |
| `@infer("$0 unify $2")`, `@infer("list[$0]")` | The result type on the value |
| `@record`, `@adt`, `@field`, `@method` | typehash `Record` and `variant Named[...]` |

Four gaps stand between that and a mechanical lowering. Each is an annotation to
design, not IR work:

1. **No rule-to-opcode annotation.** `@operator("unwrap_result")` carries a bare
   string; the real mapping lives in `codegen.c`'s op map and per-NT C handlers.
   A production needs to be able to name the IR opcode it yields and how its
   children map to operands.
2. **No value-position annotation.** `<if-value-expr>`, `<block-value-expr>` and
   `<yielding-body>` carry `@infer` but nothing says which child is the value,
   so block-valued expressions cannot be lowered without a handler.
3. **No evaluation-order annotation.** `@concat` is a pretty-printer directive.
   The IR needs operand evaluation order stated, and C does not supply it.
4. **Nothing for scope exit.** `@capture` covers try/catch entry only; defer,
   with and finally have no annotation.

An escape hatch will be needed for constructs annotations cannot reach. The
preferred shape is an IR fragment written in §10's syntax with child
substitution, so a hard case stays declarative and inspectable — which is
possible only because §10 round-trips.

---

## §16 Open questions

- **Atomics and memory ordering.** §7.5 reserves a modifier; the ordering
  vocabulary is not specified. typehash models an atomic type as `atomic[t]`
  (`~/th/spec.md` §7), so the type side is settled and only the access side is
  open.
- **Exceptions and unwinding.** No opcode. bCe does not need one; a language
  with unwinding needs either an invoke-style terminator with a landing block,
  or a lowering to explicit checks. Deferred until a front end needs it, because
  guessing produces the wrong shape.
- **Debug information beyond locations.** Variable lifetimes and scopes for a
  debugger are not modelled. §6.3 covers diagnostics and `#line` only.
- **Where the scalar profile is written.** §4.3 makes it data. Whether it is
  declared in the grammar alongside slay's other annotations, or in a separate
  file, follows §15 and is not decided.
- **Vector types.** Absent. typehash has no vector kind either, so this is a
  question for both documents at once.
- **Whether the descriptor should be shared with typehash.** `~/th/spec.md` §12
  sketches a representational descriptor and does not specify it. §5 specifies
  one. If the two converge, §5 should move to `~/th` and this section should
  reference it.

---

## §17 Changes from the previous draft

| Change | Why |
|---|---|
| `ir_type_t` deleted; types are typehash terms | The draft had a full parallel type model at §2 *and* said at §9.1 that types were 64-bit ids into a sidecar. Both cannot be true, and neither was typehash. |
| Layout moved out of the type into a descriptor | `tag_field_index`, `uses_niche` and the bitfield table were fields of `ir_type_t`, so two spellings of one type compared unequal for layout reasons and a second target was inexpressible. |
| Layout became settable, with unset/pinned/symbolic/resolved states | Automation with no override forces a fork the first time someone needs a different answer. |
| Value/statement split collapsed into instructions in blocks | `ir_value_t` had a generic `op` arm *and* `binop`/`unop`/`cmp` arms keyed by `const char *` operator strings; `IR_RETURN`, `IR_BREAK`, `IR_CONTINUE` and `IR_GOTO` were in both the value enum and the statement enum; `x = 1` was expressible three ways. |
| Union arms replaced by a uniform operand list | The old `ir_value_t` union had no arm capable of holding `IR_LOCAL_SET`, `IR_STRUCT_SET`, `IR_ARRAY_GET`, `IR_ARRAY_SET` or `IR_PHI`. About a third of the opcode set was unrepresentable. |
| Typing rules became hookable, dispatching on operand type | A fixed rule per opcode makes a language's own types inoperable and has no home for `+` on a string. The profile already says what a `Named` type is; it now also says what operations on it mean. |
| Conversion modifiers split into fit and rounding | An earlier pass of this document spelled one modifier on each axis `trunc`, which made saturation read as truncation. They are independent questions and now have independent vocabularies. |
| Signedness left the opcodes | `CMP_LT_S`/`CMP_LT_U` and `IR_SHR_S`/`IR_SHR_U` duplicate what the type says, and let the opcode and the type disagree. |
| Three arithmetic families became one opcode plus a modifier | `IR_ADD` was documented as trapping, `IR_ADDU` as unchecked, and `IR_CHECKED_ADD` also existed. |
| Three pointer-offset opcodes became one | `IR_POINTER_SUB_OFFSET` was defined as "same as `IR_POINTER_ADD`", with `IR_POINTER_OFFSET` overlapping both. |
| Fifteen variant/option/result opcodes became three | They were three operations wearing fifteen names, and typehash already models the type. |
| `IR_KWARGS_CALL` and the kwargs struct model deleted | typehash's `Function` node already carries keyword parameters, a typed rest parameter and per-parameter modes. |
| `IR_GENERIC_CALL` and every `IR_DYNAMIC_*` deleted | "Not ground" is a fact about the operand's type. Lowering boxes and unboxes from the type. |
| `IR_GC_*`, `IR_RPC_CALL`, `IR_BLUEPRINT_INSTANTIATE`, `IR_ONCE`, `IR_DEFER`, `IR_WITH` deleted | Front-end lowerings. Each was an opcode with no typing rule, which is a hole in the checker. |
| `CMP_MIN/MAX_*` deleted | Not comparisons. `select` over a comparison. |
| Saturating arithmetic moved out of the Cast section | It is arithmetic, and it is now a modifier. |
| `IR_STR_REF`, `IR_VA_LIST` deleted | `IR_STR_REF` appeared in two emission tables and in no enum. `IR_VA_LIST` appeared once and was never used; C's `...` is `*__ty.untyped`. |
| Control flow became blocks with terminators | The draft claimed SSA was a derived view in its design principles and had its SSA pass rewrite control flow, defined no block structure, put phi nodes in declaration initializers, and never addressed turning a goto graph back into C. |
| Regions became a derived overlay | Blocks serve MIR, analyses and execution; C needs structure; one representation plus an overlay serves all four without a second instruction set. |
| SSA became a property, not a form | Non-SSA and SSA are now the same IR with the same instructions, which is what makes both representable at once. |
| `ir_fn_t` defined; module defined | `ir_fn_to_ssa(ir_fn_t *)` took a type that did not exist, functions were a statement kind, and `ir_global_t.next` was a linked list belonging to nothing. |
| Source locations added | The draft had none anywhere, which makes a diagnostic from verified IR unattributable. |
| `volatile` became an access modifier | typehash excludes it from types deliberately, so the access is the only place left, and the draft had no place at all. |
| Text format given a grammar and a round-trip obligation | The draft mixed `%x` and `$opt` for locals, `IR_CONST_I64` and `(const value)` for constants, embedded `IR_STRUCT("...")` inside s-expressions, and had no production for any of it. |
| Verification became graded properties with rules | The draft's validation section was five bullets. Checking cannot be a goal and a bullet list at the same time. |
| Locations given a propagation contract | The draft had no location field at all. A span that is right at lowering and wrong three passes later is worse than none, so §6.3.4 states the rules and §12 checks that no pass loses a known location. |
| Annotations added, with provenance | A typing rule that resolves an operator has to be able to write the resolution down, or every later stage repeats the dispatch and they drift. Derived annotations die with the property that produced them; authored ones are never overwritten. |
| Diagnostics specified | Passes were assumed to report problems with no interface for doing it. Codes are stable and namespaced on the same rule as opcodes and annotation keys; several labelled spans, not one; policy decides severity, producers do not. |
