# Typehash

**Status: draft, nothing frozen.** A proposal, not a record of shipped behaviour. No value here is a
compatibility obligation. §14 lists what changed against the encoding this started from; §15 lists
what is still open.

Typehash is a function from a canonical type term to a 64-bit unsigned integer. It answers exactly
one question — **are these the same type?** — and it is one of two facets of a type's identity, the
other being a representational descriptor (§12) that is deliberately not a hash.

`typelang.bnf` is the surface syntax for terms, `typelang.py` parses, hashes and canonically prints
it, and every vector in §13 was generated through both, so the grammar and the encoding are checked
against each other rather than asserted to agree.

There are two implementations. `typehash.c` is an embeddable C library — arena-allocated, no
dependency beyond xxHash, with its own parser — intended to sit inside a compiler. It agrees with the
Python reference byte-for-byte on every vector here, on cycles, and on a set of adversarial cases;
`make check` runs the comparison. Where they first disagreed is now §3's normalizer rule.

| | |
|---|---|
| Digest | XXH3-128, low 64 bits |
| Byte order | big-endian throughout |
| Node kinds | 10 assigned, 242 free |
| Stream version | 0, hashed as a 4-byte prefix (§6) |
| Vectors | §13, generated |

---

## §1 Scope

### 1.1 What it answers

One relation: exact identity of the conceptual type. Two terms have the same hash when a front end
considers them the same type, under that front end's own rules for what "same" means.

### 1.2 What it does not answer

- **Compatibility.** C's compatible-type relation is not an equivalence — `int[]` is compatible with
  both `int[4]` and `int[3]`, which are not compatible with each other — so it has no quotient and
  cannot be hashed. Compatibility is computed by a type checker over the term (§2), never by
  comparing digests.
- **Subtyping, assignability, coercion.** Same reasoning, out of scope by construction.
- **Layout.** Sizes, offsets, alignment, padding and field order are absent from the stream. They
  live in the descriptor (§12), which is target-scoped where identity is not.
- **Mutability and access discipline.** Whether a binding may be written, and whether accesses to it
  may be optimized, are properties of *storage* and of *access paths*, not of the values a type
  admits. See §7 on what became of `const` and `volatile`.

### 1.3 The two facets

A type has a **conceptual identity** — representation-independent, target-independent, the subject
of this document — and a **representational descriptor**, a structured, decodable artifact
describing how it is laid out on one target. The descriptor is not a digest: generating an FFI
adaptor needs the offsets themselves, and a hash gives only equal-or-not.

The pairing is what makes automated FFI tractable. Equal conceptual identity with unequal
descriptors means *the same type laid out differently* — emit an adaptor, and the descriptor delta
says what it does. Unequal conceptual identity is a type error. Equal on both is a no-op.

### 1.4 Mapping obligations

The term must round-trip against each source type system it models: reconstructing a front end's own
type from the term must be possible without consulting the front end. Toward representation the
mapping is a function and not a bijection, and cannot be made one — several source types can lower
to one representation, and one source type can lower to several. That asymmetry is a property of
lowering, not of any encoding, and §12 exists because of it.

### 1.5 Normative language

**must** and **must not** mark requirements for byte-compatibility. **should** marks a strong
recommendation whose violation changes no hash. **may** marks a genuine implementation choice.

---

## §2 The type term

The hash consumes a **type term**: a directed graph of nodes, possibly cyclic. The term is the
primary artifact and is a real, decodable serialization; the hash is a digest over its canonical
encoding. Anything needing structure — lowering, adaptor synthesis, error reporting, reflection —
reads the term. Only identity comparison reads the digest.

That distinction matters because §4 is deliberately lossy in one direction. A `Named` type does not
encode its definition, no node encodes layout, and nothing encodes variance or access discipline.
Those facts are absent from the *digest* and must still be present in the *term*, or §1.4 fails —
and a checker deciding assignability needs every one of them.

Nodes are compared by object identity, not by value: the traversal detects a cycle by meeting the
same node again (§5). A front end that eagerly copies nodes encodes cycles differently from one that
shares them, so front ends **must** share the node object when closing a self-reference.

| Kind | Fields | Models |
|---|---|---|
| `Named` | `name`, `args`, `is_variant` | Every type whose identity is a name: primitives, nominal types, generic applications, tuples. |
| `BitInt` | `width`, `signed` | An integer of explicit bit width. |
| `Indirection` | `referent`, `attrs` | Every pointer, reference and owning handle — see §4.3. |
| `Sequence` | `element`, `extent?` | An array. The extent is optional and language-chosen. |
| `Record` | `members` | A type identified by the field set it contains. |
| `Function` | `ret`, `params`, `rest?`, `kwargs?` | A function type, parameters already adjusted (§9.4). |
| `Binder` | `arity`, `body` | Introduces a parameter list. |
| `Variable` | `distance`, `position` | A reference into an enclosing binder. |
| `Sum` | `comb`, `arms` | An order-insensitive union or intersection. |
| `Var` | `index` | A free type variable — the base type of a generic, and the unknown a unifier solves for. |

A **member** carries a name and a type. A **parameter** carries a mode, a type, and — for keyword
parameters — a name. An **argument** is a type, an integer or a string (§7.3).

There is no scalar registry, no node kind for aggregates-with-a-class, and no enumeration kind. If a
type's identity is its name, then only the name participates — not whether it is a struct, a union
or an enumeration, not its scope, not its members. All of those are `Named`. A language with
separate namespaces for tags distinguishes them by qualifying the name, which is a §9 obligation.

---

## §3 Primitives

All multi-byte values are big-endian on every host, regardless of native byte order.

**`U8(v)`** — one byte, the low 8 bits of `v`.

**`U32(v)`** — four bytes, `v` as a big-endian unsigned 32-bit integer. Used only for the version
prefix (§6).

**`U64(v)`** — eight bytes, `v` as a big-endian unsigned 64-bit integer. Narrower values are
zero-extended; signed values **must not** appear except where §7.3 names a two's-complement field.

**`STR(s)`** — `U64(n)` followed by the `n` bytes of `s`, where `n` is the length **in bytes, not
characters**. UTF-8, NUL-terminated nowhere. An absent string and an empty one both encode as
`U64(0)` and are indistinguishable.

```
STR("eof")  =  00 00 00 00 00 00 00 03  65 6f 66
```

**`NAME(s)`** — `STR` of `s` normalized to **NFC**. A type name is a Unicode identifier,
`(XID_Start | "_") (XID_Continue | "_")*`, and a qualified name is such identifiers joined by `.`.
The hash itself cares only about bytes and length; the identifier rule is what the term language
admits, and the normalization is what keeps one identifier from having two encodings.

NFC is required at the *source* level, not merely before hashing: a combining mark is `XID_Continue`
but is not matched by a naive word-character class, so a decomposed identifier fails to tokenize at
all unless the input is normalized first.

**An implementation without NFC available must reject a non-ASCII name**, not encode it as given.
NFC needs Unicode tables that a small embeddable library will not carry, so the realistic shape is a
caller-supplied normalizer — but the failure mode when one is absent has to be a diagnostic. Encoding
an unnormalized name produces a *different hash for the same name*, silently, which is precisely the
failure §10 exists to prevent. ASCII is its own NFC, so the common case needs nothing.

This was found by disagreement between two implementations rather than by reading: the reference C
library had no Unicode tables and hashed `café` written decomposed to a different value than the same
name written precomposed.

---

## §4 Node encoding

`ENCODE(node)` appends to a single running stream, called once on the root and recursively on
children. The whole stream is digested (§6); there is no per-node hash and no Merkle structure.

Every node begins with one byte: the kind code from §7.1 **plus one**. The bias keeps `0x00` out of
the kind space, and §7.1 keeps `0xFD`–`0xFF` out of it, so no node prologue can be confused with a
traversal sentinel (§5).

### 4.1 Named

```
U8(0x01)
U8(is_variant)
NAME(name)
U64(arg_count)
arg_count × argument
```

Each argument is a one-byte tag from §7.3 followed by its payload. The definition is **never**
encoded, which is what makes §11 work and what lets a name have an identity from a forward
declaration.

A tuple is this node: `__ty.tuple[a, b]` is an n-ary product constructor, taking order and arity
from the argument list. Its surface spelling is `t(a, b)`, which is also its canonical form. The
prefix matters: parentheses are then unambiguously grouping, which §16.6's intersections need, and
`t()` and `t(a)` become spellable where `()` and `(a)` could not be.

`__ty.` is a **reserved prefix** for the small shared vocabulary the format leans on — currently
`__ty.tuple` and §4.6's `__ty.untyped`. These are not any language's own types; they are names two
front ends must agree on for their tuples, or their bindings to C variadics, to be the same type. A
front end **must not** mint names under the prefix. It matches §6's `__ty_%016llx` derived-name
convention, and it turns a collision from a silent merge into something a checker can find.

### 4.2 BitInt

```
U8(0x02)
U64(width)
U8(signed)
```

The width is the declared bit count, never a rounded storage size.

### 4.3 Indirection

One node models every kind of reference, parameterized by an attribute set (§7.2) rather than split
across a kind per language feature.

```
U8(0x03)
U8(attrs)
ENCODE(referent)
```

The point is that the *relationships* between reference forms become derivable from the encoding
instead of asserted by a table. A C pointer permits null, reseating and offsetting; a C++ reference
permits none of the three; a Rust `&mut` adds exclusivity; a `Box` adds ownership. An adaptor
between two indirections is then a question about the attribute delta and its direction — `T&` to
`T*` weakens guarantees and is free, `T*` to `T&` strengthens them and needs a null check — which is
exactly what an FFI generator has to decide.

Two references from different languages with the same attributes are the same type by construction.
C++ `int&` and Rust `&i32` both encode `attrs = 0x00` and hash identically. That is intended: it is
what "model the relationships so they map cleanly" has to mean.

### 4.4 Sequence

```
U8(0x04)
U8(has_extent)
U64(extent)        only when has_extent is 1
ENCODE(element)
```

Whether the extent participates is the source language's decision, expressed by supplying it or not.
An extent that cannot be folded to a constant is a rejection (§10.1), not an encoding. A
multidimensional array nests outermost first.

A language that treats sized arrays as a parameterized family may skip this node and use `Named`
with a value argument instead: `core.Array[int, 4]`. Both are in §13.

### 4.5 Record

A record is identified by **what it contains**: the set of its fields, and nothing else.

```
U8(0x05)
U64(member_count)
member_count × member record
```

A member record is:

```
NAME(name)
ENCODE(type)
```

Member records are sorted ascending as byte strings before encoding. Sorting on the fully encoded
record makes the order total without needing a sub-digest.

**Field order does not participate.** It is a layout fact and belongs to §12. It could only have
earned its place for tuples or for records; tuples are `Named` and take order from their argument
list, and a record with named fields has no order to speak of.

**A bitfield width is part of the member's type, not of the member.** C's `int x:3` is a member `x`
of type `bitint[3]` (§4.2), which is where a value range belongs and which carries signedness
already. Two consequences: `int x:3` and `long x:3` become one type, differing only in a storage
unit the descriptor still separates; and an unnamed padding bitfield (`int :0;`) is not expressible
here at all, which is correct — it has no name, contributes nothing to identity, and is a pure
descriptor instruction.

A record has no name and no declaration site. A type identified by *where it was written* — C's
untagged structs, per C 6.7.2.1 — is a `Named` type whose name the front end synthesizes from the
location. That is naming policy (§9.2), not format: locations are not reproducible outside a
compiler reading source, are sensitive to build directories and `#line` directives, and shift under
edits that change nothing semantic, so they must not reach the encoding. Folding one into a name
confines the damage to the front end that chose it. The reference front end spells it
`_anon_%016x` over the digest of `STR(path) U64(line) U64(column)`.

### 4.6 Function

A function type is a return type, an ordered list of positional parameters, an optional rest
parameter, and an optional set of keyword parameters. Every parameter carries a **mode** (§7.4).

```
U8(0x06)
U8(flags)
ENCODE(ret)
U64(positional_count)
positional_count × ( U8(mode), ENCODE(type) )
U8(mode), ENCODE(rest_element)      only when flags bit 0 is set
U64(keyword_count)                  only when flags bit 1 is set
keyword_count × keyword record      only when flags bit 1 is set
```

| Bit | Meaning |
|---|---|
| `0x01` | A rest parameter is present. |
| `0x02` | Keyword parameters are present. |
| `0x04`–`0x80` | Unassigned |

A keyword record is `NAME(name)`, `U8(mode)`, `ENCODE(type)`, and keyword records are sorted
ascending as byte strings — they are matched by name, so their names participate and their order
does not. They are *not* a `Record` node: a keyword parameter is a parameter, and every parameter
has a mode.

Return type before parameter count, which is the reading order. Positional parameter names are never
encoded. A return value's mode is not encoded either, because it can only be `out`.

**A rest parameter is typed.** There is no untyped variadic. All of a rest parameter's arguments have
one type, so that type is simply another entry in the term, and the source language handles the
arity and promotion rules that go with the syntax. A language whose variadics genuinely carry no
type — C's `...` — names one: `(ptr[char], *__ty.untyped) -> int`. Every C variadic then agrees with
every other, and none collides with a typed rest. `__ty.untyped` is in the reserved vocabulary
(§4.1) because a Rust caller binding to a C variadic has to produce the same name. It is *not* a top
type and not a type a value can have — it says the argument's type is unstated, which is exactly
what C's `...` says.

Only the positional list is ordered. A front end **may** accept the three kinds of parameter
interleaved in any order and sort them out while building the term; §16.4 says what the surface
language does.

**The positional list is not obligatory.** In the ML tradition a function's domain is a single type,
and a parameter list *is* a tuple: `int * int -> unit`. A front end holding that view emits one
positional whose type is a tuple, and gets exactly that reading. A front end whose language has
genuine arity emits the list. The format supports both and identifies neither with the other, so
`(int, i64) -> void` and `((int, i64)) -> void` are different types here — which is right, because
their calling conventions differ and crossing between them is an adaptor, not a no-op. Modes and
keyword parameters are the reason the list cannot simply *be* a tuple: a mode belongs to a parameter
slot, and a tuple has no slots to put one on (§7.4).

### 4.7 Binder

```
U8(0x07)
U64(arity)
ENCODE(body)
```

Introduces `arity` parameters, referred to by `Variable` nodes in the body. Parameter names are
encoded nowhere.

### 4.8 Variable

```
U8(0x08)
U64(distance)
U64(position)
```

`distance` is how many binders out the variable was bound, counting the innermost enclosing binder
as `1`. `position` is its zero-based slot in that binder's parameter list.

This is the whole of α-equivalence: `Vec[`T]` and `Vec[`U]` produce byte-identical streams because
no parameter name reaches the encoding. A front end that already normalizes type variables by
renumbering them in a canonical traversal is doing the same construction; with one level of binder,
`distance` is always `1` and `position` is the normalized number.

Take `position` from the binder's **declared parameter order**, never from occurrence order in the
body. Occurrence order is not well-defined here: member records are sorted by their encoded bytes
(§4.5), so occurrence order would depend on the encoding, which would depend on the numbering.
Declared order breaks the cycle.

### 4.9 Sum

An order-insensitive composite: a union (`A | B`) or an intersection (`A & B`).

```
U8(0x09)
U8(comb)
U64(arm_count)
arm_count × encoded arm, ascending as byte strings
```

`comb` is `0` for union and `1` for intersection. Arms are encoded individually and the resulting
byte strings sorted before emission. Sorting on the fully encoded arm makes the order total without
needing a sub-digest, and it is what buys commutativity — the front end cannot impose an order of
its own, because the only order it has is spelling, and spelling is exactly what must not matter.

**A valid term satisfies three conditions**, and a builder **must** establish them:

- **Flat.** No arm is itself a `Sum` with the same `comb`. Nested ones are spliced in.
- **Distinct.** No two arms encode identically. Duplicates are dropped.
- **Plural.** At least two arms remain. A one-arm `Sum` **is** its arm, and collapses to it.

Together with the sort, these give the **semilattice** laws, for both combinators: `a|b` = `b|a`,
`(a|b)|c` = `a|(b|c)` = `a|b|c`, and `a|a` = `a` (§13.8). Union is a join-semilattice and
intersection a meet-semilattice; the same three conditions serve both, and the combinator byte keeps
them from flattening into each other.

The **lattice** laws are deliberately *not* performed:

| Law | |
|---|---|
| `a & (a \| b)` = `a` | absorption |
| `a \| (a & b)` = `a` | absorption |
| `a & (b \| c)` = `(a&b) \| (a&c)` | distribution |
| `a & ⊥` = `⊥`, `a \| ⊤` = `⊤` | annihilation |

Each needs something the encoder does not have. Absorption and distribution are cross-combinator —
normalizing an intersection would have to reach inside a union — and distribution can blow up
exponentially. Annihilation needs the format to recognize a top and a bottom type, and a name is
just a name here (§9.2). All four are also only *valid* if the language's `|` and `&` really are join
and meet over its subtyping order, which is a language question and not a format one. A front end
that wants them normalizes before encoding, exactly as §9.8 requires for recursive types.

The first two conditions could be enforced by the encoder; the third cannot. Collapsing a one-arm
`Sum` to its arm changes how deep the arm sits on the visit stack (§5), so an encoder deciding it
mid-stream would have to encode the arm at a depth it then abandons. Normalizing at term
construction avoids the question, and is why this is stated as a condition on terms rather than a
step in `ENCODE`.

This is the one kind whose encoding inspects a sub-stream, and the one kind excluded from §11's
derivation shortcut.

### 4.10 Var

A **free** type variable. `Vec[`t]` is the base type of the `Vec` generic and has a hash like any
other type; a free variable is also what a unifier solves for.

```
U8(0x0A)
U64(index)
```

`index` is the variable's position in **first-occurrence order** over the whole term, so the name
never reaches the encoding: `Vec[`t]` and `Vec[`u]` are byte-identical, `Map[`k,`v]` and `Map[`v,`k]`
agree, and `Map[`k,`k]` differs from both because sharing participates.

**The numbering traversal orders `Sum` arms by their variable-erased encoding.** Without that it
would be circular: arms are sorted by encoded bytes, the bytes contain the indices, and the indices
come from the traversal order. Erasing every variable to one placeholder makes the ordering key
independent of the numbering, and the two spellings of a sum then converge (§13.5). Record and
keyword members need no such care — their names are unique, so the name prefix already decides every
comparison.

A term **must** be normalized before encoding; an implementation encountering an un-indexed `Var`
**must** fail rather than guess.

**A free variable is not a `Binder`.** `Vec[`a]` is an *open* type with one unknown, and
`forall[`a] Vec[`a]` is a *scheme* that is instantiated fresh at every use. They hash differently and
should: `generalize` maps the first to the second, `instantiate` maps back, and a type checker needs
both — an inferred function's remaining unbound variables are promoted to a scheme, and each call
site then instantiates its own copy so callers cannot interfere.

---

## §5 Traversal and cycles

The encoder maintains a **visit stack** of node identities and emits a back-reference rather than
recursing forever.

```
ENCODE(node):
    if node is absent:
        emit U8(0xFF); return
    for i in 0 .. depth-1:
        if stack[i] is node:
            emit U8(0xFE); emit U64(depth - i); return
    if depth >= 256:
        emit U8(0xFD); return
    stack.push(node)
    emit_body(node)
    stack.pop()
```

| Byte | Sentinel | Meaning |
|---|---|---|
| `0xFF` | absent | A null child where a node was expected. |
| `0xFE` | back-reference | Followed by `U64(depth − i)`, the *relative* distance back up the stack. Relative, so the digest describes the shape of the cycle and not where it was entered. |
| `0xFD` | depth cutoff | The visit stack is full; the subtree is elided. |

All three **must** be implemented. `0xFD` makes the function total rather than divergent, and the
limit of **256** is part of the format: a different limit changes the hash of any type reaching it.

The stack is a **path, not a visited set**. It pops on exit, so a node reached twice by different
paths is encoded in full both times. Memoizing encoded nodes is not an optimization; it is a
different format.

---

## §6 Finalization

Let `S` be `U32(VERSION)` followed by the stream from `ENCODE(root)`.

```
typehash = XXH3_128bits(U32(VERSION) || ENCODE(root)).low64
```

`VERSION` is currently **0**. It is the first thing hashed, so every revision of §3–§8 is
domain-separated from every other: a stream produced under one version can never collide with one
produced under another, and a reader that is handed a value from the wrong revision gets a mismatch
rather than a plausible wrong answer. Bump it on any change to the encoding. It costs four bytes at
the head of one hash and is impossible to add after the first value escapes.

The prefix applies once, to the whole stream. Sub-streams encoded for ordering — `Sum` arms (§4.9),
record members (§4.5) — carry no prefix of their own; they are spliced into the parent stream, which
the top-level prefix already covers.

Default secret, no seed. The high half is reserved, so a future 128-bit identity is the same stream
at full width and moves no 64-bit value already in circulation.

`XXH3_64bits(S)` is **not** the low 64 bits of `XXH3_128bits(S)`. Both entry points appear in this
document — §8 uses the 64-bit one deliberately — so the distinction has to be carried per call site.

The result is **not** coerced away from zero. A type whose digest is zero has typehash `0`, and
consumers **must not** read `0` as "no type". The arm tag in §8 *is* coerced, because it has a
sentinel slot.

XXH3 is value-stable from **xxHash 0.8.0** onward. Every implementation **must** use 0.8.0 or later
and **should** assert it at build time; a silent vendor downgrade moves every identity at once.

Where a spelling is needed rather than an integer, the derived name is 16 lowercase hex digits,
zero-padded, after a fixed prefix: `__ty_%016llx`.

---

## §7 Registries

Codes are assignments, not ordinals. Nothing derives meaning from a code's numeric value, so gaps
are free and ordering carries no information.

**There is no qualifier byte.** An earlier draft carried `const`, `volatile` and `atomic` in every
node prologue. Those are three unrelated things wearing one hat, and only one of them is a type
fact: `const` is a promise about a parameter slot or a property of a storage location, so it became
§7.4's modes; `volatile` is a codegen instruction about an access path, so it joined `restrict` in
being excluded outright; and `atomic` is a genuinely different type with its own size, alignment and
operation set, so it is a `Named` application, `atomic[t]`. Nothing was left to qualify, and the
byte went with them.

### 7.1 Node kinds

Encoded as `U8(code + 1)`.

| Code | Byte | Kind |
|---|---|---|
| 0 | `0x01` | Named |
| 1 | `0x02` | BitInt |
| 2 | `0x03` | Indirection |
| 3 | `0x04` | Sequence |
| 4 | `0x05` | Record |
| 5 | `0x06` | Function |
| 6 | `0x07` | Binder |
| 7 | `0x08` | Variable |
| 8 | `0x09` | Sum |
| 9 | `0x0A` | Var |
| 10–251 | `0x0B`–`0xFC` | Unassigned |
| — | `0xFD`–`0xFF` | Reserved: traversal sentinels (§5) |

Adding a kind requires registration: there is no private-use range, because the format has no length
prefixes and a reader cannot skip an unknown kind. Before adding one, check whether the shape is a
`Named` application — a parameterized shape almost always is, and that costs no registry space and
no new code in any implementation.

### 7.2 Indirection attributes

| Value | Attribute | Set when |
|---|---|---|
| `0x01` | `NULLABLE` | The reference may hold no referent. |
| `0x02` | `RESEATABLE` | The reference itself can be repointed after initialization. |
| `0x04` | `INDEXABLE` | Offsetting the reference to another element is defined. |
| `0x08` | `OWNING` | The reference owns its referent's storage. |
| `0x10` | `EXCLUSIVE` | No other live reference may alias the referent. |
| `0x20` | `TEMPORARY` | Binds an expiring value. |
| `0x40`–`0x80` | — | Unassigned |

| Form | Attributes | Value |
|---|---|---|
| C `T *` | `NULLABLE\|RESEATABLE\|INDEXABLE` | `0x07` |
| C `T * const` | `NULLABLE\|INDEXABLE` | `0x05` |
| C++ `T &` | none | `0x00` |
| C++ `T &&` | `TEMPORARY` | `0x20` |
| Rust `&T` | none | `0x00` |
| Rust `&mut T` | `EXCLUSIVE` | `0x10` |
| `Box<T>` | `OWNING` | `0x08` |
| GC reference | `NULLABLE\|RESEATABLE` | `0x03` |

C's `T * const` is a pointer that cannot be repointed, which is what `RESEATABLE` says; there is no
second mechanism for it. C's `const T *` — a pointer through which you may not write — has no
attribute here at all, because that is a checking rule about an access path and not a property of
the values the type admits. `const char *` and `char *` have the same value set, the same layout and
the same marshalling; they differ only in what a C compiler permits, which is the C front end's
business.

### 7.3 Argument tags

| Tag | Argument | Payload |
|---|---|---|
| 0 | type | `ENCODE(node)` |
| 1 | unsigned integer | `U64(v)` |
| 2 | signed integer | `U64(two's complement)` |
| 3 | byte string | `STR(v)` |
| 4–255 | — | Unassigned |

Values are encoded already evaluated. An argument that is an unevaluated expression has no identity
(§10.1).

### 7.4 Parameter modes

| Code | Mode | Meaning |
|---|---|---|
| 0 | `in` | The callee reads the parameter. The default. |
| 1 | `out` | The callee writes it. |
| 2 | `inout` | Both. |
| 3–255 | — | Unassigned |

A mode is a property of a **parameter slot**, not of the type sitting in it. A variable passed to an
`inout` parameter is not itself of an "inout type"; the mode belongs to the function's type, which
is why it is encoded in §4.6 and appears nowhere in §2's node table as a field of a type. A return
value's mode is necessarily `out` and is not encoded.

---

## §8 Variant arm tag

One auxiliary identity, and the only place the 64-bit XXH3 entry point is used.

```
tag = XXH3_64bits(NFC(arm_name))
return tag == 0 ? 1 : tag
```

The arm name's raw normalized bytes, with **no `STR` framing** — passing them through `STR` for
consistency with the rest of the document produces the wrong tag. The tag is a function of the name
alone, not of the variant type, the payload type or the arm's position, so `ok` is the same selector
in every variant in a program. An unnamed arm is an error.

---

## §9 Normalization contract

The hash normalizes nothing but Unicode form and the orderings §4 names. Everything below is the
front end's job while building the term, and two front ends that normalize differently disagree even
with a byte-perfect encoder.

**9.1 Aliases resolve.** An alias is not a node. It resolves transitively to the node its target
denotes; alias names never reach the stream.

**9.2 Names are chosen, not spelled.** There is no registry, so a name means whatever the front ends
sharing it agree it means. Two front ends that want a 64-bit signed integer to be one type **must**
agree on one name for it. Two that want C's `long` and `long long` to stay distinct at equal width
give them distinct names. A language with separate tag namespaces qualifies them apart. All of this
is naming policy, and the format takes no position beyond §3's identifier rule.

**9.3 Access discipline is dropped.** Immutability of a binding, write-permission through a pointer,
and volatility of an access are not encoded (§7). A front end that enforces them does so before
building the term.

**9.4 Function parameters adjust.** Parameters **must** be adjusted by the source language's rules
before encoding. In C an array parameter becomes an indirection to its element and a function
parameter an indirection to function, and top-level qualifiers are dropped. This is a lowering rule
living inside the identity contract, and it is here only because C's own type identity is defined
after adjustment.

**9.5 Extents fold.** A constant-expression extent **must** be folded to an integer. One that cannot
be folded is a rejection (§10.1), not a fallback.

**9.6 Sums normalize at construction.** A `Sum` term **must** be flat, distinct and plural (§4.9).
The encoder supplies the ordering; the builder supplies the other three.

**9.7 Type variables are positional.** `Variable.position` comes from the binder's declared
parameter order (§4.8).

**9.8 Recursive structural types must be minimized.** Back-references are relative stack distances
(§5), so the encoding is a tree unfolding with cycle markers, not a bisimulation quotient. `A = (next:
ptr[A])` and the pair `B = (next: ptr[C])`, `C = (next: ptr[B])` denote the same type and hash
differently, because the cycle closes at different depths. A front end whose language calls those
equal **must** minimize the type automaton before encoding — this is Amadio–Cardelli territory, and
recursive type equality is DFA equivalence, which no serialization computes for you. Naming a
recursive type instead (`Named`, §4.1) sidesteps it entirely, and is the reason to prefer that.

**9.9 What must survive.** The distinction between two names a language keeps distinct; parameter
modes; and, where a language supplies extents, distinct extents.

---

## §10 Rejection

Three inputs have no identity. An implementation **must** reject them with a diagnostic rather than
encode a placeholder, because a placeholder silently makes two different types equal.

**10.1 Unevaluable constant.** A sequence extent or value argument the type layer cannot evaluate —
typically because it depends on target layout (`sizeof`, `_Alignof`, a cast). The type layer does
not know the target's layout; only the backend does.

**10.2 Runtime-valued extent.** A variable-length array. Not a compile-time entity at all.

**10.3 Un-normalized free variable.** A `Var` with no index (§4.10). Free variables are perfectly
legal — they are how a generic's base type is written — but their indices come from a whole-term
traversal, so a node cannot be encoded until that has run. Normalize, or fail; never guess an index.

---

## §11 Derivation

Instantiating a generic **must not** require the constructor's definition. This is a normative
property of the encoding, not an optimization:

> The stream of a `Named` node is a function of its name and the encodings of its arguments alone.

It holds because §4.1 never encodes the definition. The stream is the prologue, `NAME(name)`,
`U64(arg_count)`, and each argument's encoding concatenated — so given the name and the argument
streams, an implementation produces the application's hash with no access to the generic's body, and
a JIT and an ahead-of-time compiler agree without exchanging definitions.

Substitution into a `Binder` body is deterministic on the same grounds: replacing each `Variable` at
`(distance, position)` with the argument at that slot and re-encoding yields one stream, and
successive partial bindings compose — `D[`K,`V]` to `D[int,`V]` to `D[int,char]`, each step's hash
derivable from the last.

**`Sum` is excluded.** A `Sum` node's arms are emitted in sorted order (§4.9), and substituting into
an arm changes that arm's encoding, which can change where it sorts. So `h(Sum[σa, σb])` is *not*
derivable from `h(Sum[a, b])` and σ — the arms must be re-encoded and re-sorted. Every other kind
keeps the shortcut.

**Closedness condition.** This holds when arguments are **closed** terms. Back-references are
relative stack distances (§5), so an argument containing a back-reference that reaches outside itself
has its distances shifted by substitution, and the derived stream will not match the stream of the
substituted term. Front ends **must** close arguments before substituting, or encode the substituted
term directly rather than deriving.

A consequence worth stating plainly: because the definition is never encoded, the hash cannot detect
that a layout optimization changed how a generic expands. `Option[ptr[int]]` niche-packed and
`Option[int]` as flag-plus-payload are both just their argument list. That is the intended trade —
identity survives representation changes — and detecting the change is §12's job.

---

## §12 The representational descriptor

Sketch, not specified here. Recorded so §1.3's split is not lost.

The descriptor is a **structured, decodable, target-scoped** artifact describing how one type is
laid out on one target: aggregate size and alignment; **field order**; per-member bit offset and bit
width, in bits for every member and not only bitfields, so bitfield packing is expressible at all;
the representation chosen for each indirection, including whether it is fat; a sequence's stride.

Three properties are load-bearing:

- **It is not a digest.** Generating an adaptor needs the offsets. A hash over the descriptor is a
  fast-path cache key; it never replaces the descriptor.
- **It is keyed by the conceptual identity**, so the pair travels together and §1.3's three-way
  decision — no-op, adaptor, type error — is a lookup.
- **It never crosses a machine as an identity.** Offsets and sizes are ABI facts, so any
  descriptor-derived value differs across a cross-compile by construction. Valid for a GC scan
  descriptor or an ABI-drift check, both per-target. Never valid as a marshalling wire tag.

Keeping layout out of §4 also removes the ahead-of-time problem a layout-bearing *identity* creates.
A compiler whose layout knowledge is the host's cannot commit a layout-bearing hash under a
cross-compile, but it can emit a descriptor symbolically — `offsetof` and `sizeof` expressions the
target compiler resolves — while the conceptual identity stays a true translation-time constant.

---

## §13 Conformance vectors

Generated by `typelang.py` over `typelang.bnf` against xxHash 0.8.x, at stream version 0. The
Type column is surface syntax; streams are hex and exclude the §6 version prefix, which is
hashed but not part of `ENCODE`.

### 13.1 Names

| Type | Stream | Typehash |
|---|---|---|
| `int` | `01000000000000000003696e740000000000000000` | 4692178662541719424 |
| `i64` | `010000000000000000036936340000000000000000` | 2345581113092380360 |
| `string` | `01000000000000000006737472696e670000000000000000` | 9125363554618730262 |
| `Point` | `01000000000000000005506f696e740000000000000000` | 61927974207728736 |
| `my.mod.Widget` | `0100000000000000000d6d792e6d6f642e5769646765740000000000000000` | 14092847191071880578 |
| `café` | `01000000000000000005636166c3a90000000000000000` | 653193918278987289 |
| `bitint[7]` | `02000000000000000701` | 10578868988700533128 |
| `ubitint[7]` | `02000000000000000700` | 13863079266222671404 |
| `atomic[int]` | `0100000000000000000661746f6d696300000000000000010001000000000000000003696e740000000000000000` | 5203914890576534338 |

`café` written decomposed (`cafe` + U+0301) yields the same value, per §3's NFC rule.

### 13.2 Indirections

C++ `int&` and Rust `&i32` coincide by design (§4.3), and both are `ref[int]`. The last row is
C's `int * const`.

| Type | Stream | Typehash |
|---|---|---|
| `ptr[int]` | `030701000000000000000003696e740000000000000000` | 10823981672375649634 |
| `ptr[ptr[int]]` | `0307030701000000000000000003696e740000000000000000` | 4356964810979264118 |
| `ref[int]` | `030001000000000000000003696e740000000000000000` | 9842916253882920978 |
| `mutref[int]` | `031001000000000000000003696e740000000000000000` | 15111697610749857526 |
| `own[int]` | `030801000000000000000003696e740000000000000000` | 14540415123153874431 |
| `gcref[int]` | `030301000000000000000003696e740000000000000000` | 17803393085695289489 |
| `tmpref[int]` | `032001000000000000000003696e740000000000000000` | 759194846836287271 |
| `ref[int; nullable, indexable]` | `030501000000000000000003696e740000000000000000` | 16995997302793471537 |

### 13.3 Sequences

| Type | Stream | Typehash |
|---|---|---|
| `array[int]` | `040001000000000000000003696e740000000000000000` | 3851546464347734961 |
| `array[int; 4]` | `0401000000000000000401000000000000000003696e740000000000000000` | 6679426691399913996 |
| `array[int; 3]` | `0401000000000000000301000000000000000003696e740000000000000000` | 914624473392712112 |
| `array[array[int; 3]; 4]` | `040100000000000000040401000000000000000301000000000000000003696e740000000000000000` | 3101888567962527639 |

### 13.4 Functions

| Type | Typehash |
|---|---|
| `(int) -> int` | 15126490878935496149 |
| `(char, i64) -> int` | 5377411045681565112 |
| `ptr[(int) -> int]` | 9209100992988457837 |
| `(int, *string) -> void` | 8139151780150265837 |
| `(int, *int) -> void` | 2187972423419068875 |
| `(ptr[char], *__ty.untyped) -> int` | 13591021113435421938 |
| `(int, x: int, y: char) -> void` | 5569193554611101133 |
| `(y: char, int, x: int) -> void` | 5569193554611101133 |
| `(out int) -> void` | 5588208211570253644 |
| `(inout int) -> void` | 5648110481110389469 |
| `(int, out i64) -> void` | 8517216940690017409 |
| `(out *string) -> void` | 13873111717702311979 |
| `(x: out int, y: char) -> void` | 12222942062556835935 |

A rest parameter's type participates, so `*string` and `*int` differ. A rest parameter is not a
positional, so `(int, *string) -> void` and `(int, string) -> void` differ. Keyword parameters
commute, and rows 7 and 8 show they may be written interleaved with positionals. Modes participate:
`in`, `out` and `inout` give three distinct types, and `in` is the default.

### 13.5 Free variables

`Vec[`t]` is stream `010000000000000000035665630000000000000001000a0000000000000000` — the
variable encodes as its index, so the name is absent.

| Type | Typehash |
|---|---|
| ``a` | 5851570934447095280 |
| `Vec[`t]` | 6906495525769281655 |
| `Vec[`u]` | 6906495525769281655 |
| `Map[`k, `v]` | 4280065276152460987 |
| `Map[`v, `k]` | 4280065276152460987 |
| `Map[`k, `k]` | 4558013079054113660 |
| `(`a) -> `a` | 6963596648155318882 |
| `forall[`a] (`a) -> `a` | 1486266877764136791 |
| `forall[`a] Vec[`a]` | 14365919947622227246 |
| `(Vec[`a] \| Set[`b])` | 12922645968586099357 |
| `(Set[`b] \| Vec[`a])` | 12922645968586099357 |

Five things to read off this. A free variable is a type with a hash, so a generic has a base type.
Names do not participate — `Vec[`t]` and `Vec[`u]` agree. Position does not either, so `Map[`k,`v]`
and `Map[`v,`k]` agree, while `Map[`k,`k]` differs from both because *sharing* participates. The two
sums agree, which is the variable-erased ordering of §4.10 doing its job — under a naive
first-occurrence walk they would not. And a scheme is not an open type: `forall[`a] (`a) -> `a` and
`(`a) -> `a` are different types.

### 13.6 Generics

| Type | Typehash |
|---|---|
| `core.Vec[int]` | 12320463950420700199 |
| `variant core.Result[int, char]` | 14310787509736015389 |
| `core.Array[int, 4]` | 11667540125080351483 |
| `core.Array[int, 3]` | 2195293896148443996 |

### 13.7 Tuples and records

| Type | Typehash |
|---|---|
| `t(int, i64)` | 3589617709486911988 |
| `__ty.tuple[int, i64]` | 3589617709486911988 |
| `t(i64, int)` | 16224033872291544837 |
| `t()` | 10762572153719642477 |
| `t(int)` | 17994860318831566908 |
| `(int, i64) -> void` | 17588491397557555252 |
| `(t(int, i64)) -> void` | 16902525102167310929 |
| `(x: int, y: i64)` | 17975023459832379820 |
| `(y: i64, x: int)` | 17975023459832379820 |
| `(a: bitint[3], b: ubitint[5])` | 15612925422900405387 |
| `@"t.c":1:9` | 10258477704435304467 |
| `_anon_7da51262ab9bf491` | 10258477704435304467 |

Four things to read off this. `t(...)` is exactly `__ty.tuple[...]`, and `t()` and `t(int)` are
types in their own right. Tuple order participates, because it is argument order; record field order
does not, because it is layout. Two positionals are not one tuple parameter (§4.6). And the `@` form
is nothing but a way to spell a synthesized name without computing it by hand.

### 13.8 Arm tags

| Arm | Value |
|---|---|
| `ok` | 4084568044382880290 |
| `err` | 4181668707079015515 |

### 13.9 Sums

| Type | Typehash |
|---|---|
| `(int\|char\|i64)` | 14686915048163509193 |
| `(int & char)` | 8367061870837786733 |
| `(int & char & i64)` | 1627537287430127648 |
| `(int \| char & i64)` | 7349451963655261678 |
| `((int \| char) & i64)` | 1436324571633937604 |

| Identity | Holds because |
|---|---|
| `(int \| char)` = `(char \| int)` | arms are sorted (§4.9) |
| `((int \| char) \| i64)` = `(int \| (char \| i64))` = `(int \| char \| i64)` | flat |
| `(int \| int)` = `int` | distinct, then plural |
| `(int \| char \| int)` = `(int \| char)` | distinct |
| `(int & char)` = `(char & int)` | the meet laws mirror the join laws |
| `(int \| char)` ≠ `(int \| i64)` | arms differ |
| `(int \| char)` ≠ `int` | two arms survive |

The last two rows of the table above differ, so `&` binding tighter than `|` is a real distinction
and not a spelling convenience. The lattice laws are not performed (§4.9).

### 13.10 Cycles

Not expressible in `typelang` — there is no recursive binding syntax — so these are built through a
construction API and are the only vectors in §13 not produced by the parser. They exercise the
`0xFE` back-reference of §5, which nothing else here reaches.

| Term | Stream | Typehash |
|---|---|---|
| `A = (next: ptr[A])` | `05000000000000000100000000000000046e6578740307fe0000000000000002` | 8712469699649908478 |
| `B = (next: ptr[C])`, `C = (next: ptr[B])` | `05000000000000000100000000000000046e657874030705000000000000000100000000000000046e6578740307fe0000000000000004` | 18439698686230469072 |

The two denote the same type under bisimulation and hash differently, because the cycle closes at
different depths — §9.8's obligation, made concrete. A front end whose language calls them equal
minimizes the automaton first; one that names its recursive types never reaches this at all.

### 13.11 Rejections

```
ptr             reserved word, not a type name
const int       not part of a type (§7)
(int, i64)      parentheses group a single type; a tuple is t(a, b)
un-indexed Var  normalize before encoding (§10.3)
café            non-ASCII with no NFC normalizer available (§3)
```

---

## §14 Changes from the inherited encoding

The encoding this started from was ncc's. None of these are migrations; nothing shipped.

| Change | Why |
|---|---|
| Scalar registry deleted | It was 50 multi-word C spellings, which was neither what the source language accepted nor a vocabulary any other language could use. A type name is now just a name (§3). |
| `Scalar` and `Constructor` merged into `Named` | Without a registry, a bare name and a nullary application are the same thing, and two encodings for one concept is a defect. |
| Pre-hashed constructor identity deleted | The name goes inline. Arity comes from the argument count already in the stream, so the auxiliary hash bought nothing and its name-plus-arity input collided by construction. |
| Nominal aggregates and enumerations became `Named` | If identity is the name, nothing else participates. The class byte and the scope field had no effect on anything. |
| Tuples became `__ty.tuple[...]`, spelled `t(...)` | A tuple is an n-ary product constructor. Order and arity come from the argument list, so the tuple case needed no flag and no member records. The `t` prefix keeps parentheses unambiguously for grouping, which intersections need, and makes `t()` and `t(a)` spellable. |
| `ORDERED` deleted | With tuples gone it had no remaining job: a named field set has no order to speak of. Field order is a §12 fact. |
| `MEMBERS`/`NAMES` deleted | A `Record` exists to be looked inside; a type that is not looked inside is `Named`. |
| Record anchors deleted | Locations are not reproducible outside a compiler reading source, are sensitive to build layout, and shift under semantically empty edits. C's per-declaration rule is served by a synthesized name instead (§4.5). |
| Bitfield width moved into the member's type | The width was in identity *and* in the descriptor. It is a property of the type: `int x:3` is a member of type `bitint[3]`. |
| `Sum` added | `A \| B` = `B \| A` holds in every language that has unions, so it is a genuine conceptual equality, and every equality the format declines to perform becomes an obligation a front end gets wrong. |
| Function grew a rest parameter, keywords and modes | A single `variadic` bit lost the rest type and dropped keyword parameters entirely, so n00b types could not round-trip (§1.4). Untyped variadics went away — C's `...` is a rest over a name the C profile chooses. |
| Qualifier byte deleted | `const`, `volatile` and `atomic` are three unrelated things. Mutability is a parameter mode or a property of storage; volatility is a codegen instruction about an access path; atomicity is a distinct type, `atomic[t]`. Nothing was left to qualify, so a byte left every node. |
| Attributes renamed | `rebindable` → `reseatable` (rebinding means name-rebinding in half the languages that use it), `arith` → `indexable`, `unique` → `exclusive` (matches how Rust describes `&mut`). |
| Names are NFC-normalized (§3) | Two encodings of one identifier would otherwise hash differently — and the decomposed form does not even tokenize. |
| Pointer became attribute-parameterized `Indirection` | Six opaque kinds for C/C++/Rust reference forms express no relationships. Attributes make the adaptor question derivable. |
| Extent optional, extent-kind enum dropped | Whether size participates is the language's call. Two of the four extent kinds existed only to be rejected. |
| `Binder` and `Variable` added | Name-based parameters break α-equivalence: `Vec[`T]` and `Vec[`U]` must agree. |
| `Var` added; free variables are hashable | `Vec[`t]` is the base type of a generic and needs an identity. Indices come from first-occurrence order, so names never reach the encoding. An earlier draft rejected free variables outright, which made a generic's own type inexpressible. |
| Version prefix added (§6) | Nothing in a stream said which revision produced it. Four bytes at the head of one hash, free now, impossible after the first value escapes. |
| Compatibility explicitly out of scope | The old text argued from C's *compatibility* rules to justify an *identity* design. They are different relations, and compatibility is not an equivalence. |

---

## §15 Open questions

- **Effect and capability annotations.** Whether `throws`, `async` or an effect row is part of a
  function type's identity is a language decision. The format deliberately takes no position and has
  no field. A language can already reach the Rust outcome without one, by putting the effect in the
  return type — `async fn` desugaring to `Future[T]` is an ordinary `Named` application. What that
  does not serve is a language where the effect is a side channel rather than a monad: Swift's
  `() throws -> Int` really does return `Int`, and rewriting it as `Result[Int, Error]` changes what
  the return type is, which breaks §1.4. If that case has to be served later, the shape is one
  optional slot — §4.6 flags bit `0x04`, holding a single term whose contents the format does not
  define — and the bit is unassigned, so the upgrade stays cheap.
- **Variance.** Deliberately absent from the digest, and not merely out of scope: variance cannot
  change an identity, because two applications of one constructor to the same arguments are the same
  type whatever the annotation says. It **must** still be carried in the term (§2), since deciding
  assignability needs it. Two modules declaring one name with different variance is a naming
  collision, which §9.2 assigns to the front end.

---

## §16 Surface syntax

`typelang.bnf` defines the textual notation, derived from the `<type-spec>` productions of
`n00b.bnf` and extended only where typehash draws a distinction n00b's surface syntax cannot state.

### 16.1 Inherited from n00b

Backtick type variables, `name[args]` application, `(params) -> ret` functions, `(x: int, y: i64)`
records, `a | b` unions with the upper/lower tier split that forces a union into
parentheses in declaration position, `.`-qualified names.

### 16.2 Added, and why

| Addition | Because |
|---|---|
| `ptr` `ref` `mutref` `own` `gcref` `tmpref`, and `ref[t; attrs]` | n00b has one `ref[t]`. §7.2 has an attribute set, and the sugar is defined in terms of it. |
| `array[t]` and `array[t; n]` | Whether an extent participates is the language's call (§4.4). n00b has no sized-array type. |
| `@"path":line:col` | Sugar for the synthesized name a C front end gives an untagged type (§4.5). It lowers to a `Named`, so `@"t.c":1:9` and `_anon_7da51262ab9bf491` are one type. |
| `forall[...]` | Marks a *scheme*, distinct from an open type (§4.10). n00b's bare `` `t `` is the open form and is written exactly as n00b writes it. |
| `bitint[n]`, `ubitint[n]` | §4.2. |
| `variant name[args]` | `is_variant` participates in identity (§4.1). |
| `in` `out` `inout` | §7.4 modes. Ada's spelling, except that `inout` is one word. |

Nothing was added for nominal types: `Point`, `struct Point` and `enum Point` would all be one
identity, so the grammar admits only the name.

### 16.3 Rejections

A reserved word is not a type name. `const` and `volatile` are rejected with a pointer to §7 rather
than silently parsed as names. A bare `(a, b)` in type position is rejected with a pointer at
`t(a, b)`. Everything else that lexes as an identifier is a valid type name.

A free type variable is **not** rejected — `` `t `` is written exactly as n00b writes it and denotes
an open type (§4.10). Two separately parsed types have disjoint variables, so `` `a `` in one is not
`` `a `` in the other; sharing is established within a single term, or by instantiating one scheme
twice.

### 16.4 Parameters may be written in any order

A parameter is classified by its own syntax, not by where it sits: a bare type is positional, `*t`
is the rest, and `name: t` is a keyword. So all of these are one type —

```
(int, x: int, y: char) -> void
(y: char, int, x: int) -> void
(x: int, y: char, int) -> void
```

— because only the relative order of the *positionals* is meaningful, and there is one of those.
Keyword parameters carry no `**` marker; the name is the marker. This is the one place `typelang`
departs from n00b's spelling.

A mode precedes the type for a positional or rest parameter (`out int`, `out *string`) and follows
the colon for a keyword (`x: out int`), which is Ada's placement. `out x: int` is also accepted and
canonicalizes to `x: out int`.

### 16.5 Canonical form

`unparse` renders a term back to source, and it renders the *normalized* term, so it doubles as the
canonical spelling of a type and as evidence for §2's claim that the term is decodable. It is
idempotent, and it preserves the hash.

| Written | Canonical |
|---|---|
| `(y: i64, x: int)` | `(x: int, y: i64)` |
| `__ty.tuple[int, i64]` | `t(int, i64)` |
| `(int)` | `int` |
| `(int \| char & i64)` | `(int \| (i64 & char))` |
| `@"t.c":1:9` | `_anon_7da51262ab9bf491` |
| `(y: char, int, x: int, *string) -> void` | `(int, *string, x: int, y: char) -> void` |
| `(out x: int) -> void` | `(x: out int) -> void` |
| `(out int, in i64) -> void` | `(out int, i64) -> void` |
| `forall[`K, `V] core.D[`K, `V]` | `forall[`a, `b] core.D[`a, `b]` |
| `((int\|char)\|(char\|i64))` | `(i64 \| int \| char)` |
| `(int\|int)` | `int` |

Type variables are renamed to `a`, `b`, … because their source names are not in the term at all
(§4.8) — the canonical form cannot recover a name that was never encoded, and does not pretend to.
Sum arms, record fields and keyword parameters come out in the encoder's sort order, so there is one
notion of canonical rather than two.

### 16.6 Unions and intersections

`a | b` and `a & b` build a `Sum` (§4.9), normalized on construction: flattened, deduplicated, and
collapsed to the bare arm if only one survives. The parser does not choose an order — the encoder
sorts.

`&` binds tighter than `|`, as in Scala, TypeScript, Java and Kotlin, so `a | b & c` is
`a | (b & c)`. Grouping is by parentheses, which is why a tuple is `t(a, b)` rather than `(a, b)`
(§4.1) — otherwise `(a | b) & c` and a two-element tuple would want the same syntax.

`&` is the second and last place `typelang` departs from n00b, whose `<type-spec>` has no
intersection form.

---

## §17 Modelling catalogue

What to do with each common type-system feature, or why it is not supported. Entries marked
**term only** have an identity but are deliberately absent from the digest: they are inputs to
relations the digest does not answer (§1.2), and §2 requires the term to carry them.

The recurring principle, which resolves most of the hard entries:

> Anything whose meaning is *"the set of types satisfying P"* has an identity — the syntactic P —
> but its **denotation** has no digest. The digest answers "same type?"; membership, assignability
> and satisfaction are checker queries over the term.

### 17.1 Scalars

| Feature | Model |
|---|---|
| Sized integers, floats, `bool`, `char` | A name. `i8`, `u32`, `f64` — §9.2 is the whole story. |
| Explicit-width integers | `bitint[n]` / `ubitint[n]` (§4.2), when the width is the point rather than the name. |
| Platform-dependent integers (C `long`) | A name. Front ends wanting `long` ≡ `i64` agree on one name; wanting them distinct, two. |
| Unit / `void` | A name. A 0-tuple is not spellable and does not need to be. |
| Atomics | `atomic[t]` — a distinct type with its own size, alignment and operations, not a qualifier (§7). |

### 17.2 Aggregates

| Feature | Model |
|---|---|
| Nominal struct, class, record | `Named` (§4.1). Members live in the term, never in the digest. |
| Structural record | `Record` (§4.5). Field set is the identity; field order is not. |
| Tuple | `t(a, b)` — `__ty.tuple` applied to the elements (§4.1). `t()` is a unit, `t(a)` a 1-tuple. |
| C untagged struct | `Named` over a synthesized name (§4.5). Not a location — locations never reach the encoding. |
| C `union` | `Named`. A storage overlay is nominal; it is **not** a `Sum`, which is a set-like composite with no storage story. |
| Tagged variant, ML datatype, Rust `enum` | `variant Named[...]` (§4.1). Arm names get selectors from §8; arm payloads live in the term. |
| Enumeration | `Named`. Enumerator names and values are term-only — two enumerations sharing a name are one type. |
| Bitfields | The member's type is `bitint[n]` (§4.5). Packing is §12. |
| Anonymous padding (`int :0;`) | Not expressible, and correctly so: no name, no identity contribution, pure descriptor instruction. |

### 17.3 References

| Feature | Model |
|---|---|
| Pointer, reference, rvalue reference, owning box, GC reference, unique reference | `Indirection` plus attributes (§7.2). The attribute delta is what an FFI adaptor is computed from. |
| C `T * const` | `ref[t; nullable, indexable]` — a pointer that cannot be repointed. |
| C `const T *` | `ptr[t]`. Write-permission through a path is a checking rule, not a property of the values a type admits (§7.2). |
| Optional / nullable | Two different things. `nullable` is an attribute of a *reference*; `Option[t]` is a `Named` wrapping any type. Pick by which one the language means. |
| Fat pointers, `dyn Trait` | Identity is a name; fatness is representational and belongs to §12. |
| Weak references | No attribute. Either name the wrapper or register a new attribute bit — §7.2 has two free. |

### 17.4 Sequences

| Feature | Model |
|---|---|
| Fixed-size array | `array[t; n]` (§4.4), or `Named` with a value argument if the language treats sized arrays as a parameterized family. |
| Slice, unsized array | `array[t]`. |
| Growable vector, list | `Named` — it is a library type, not a format concern. |
| Variable-length array | **Rejected** (§10.2). Not a compile-time entity. |
| Array whose extent needs target layout | **Rejected** (§10.1). The type layer does not know the target's layout. |

### 17.5 Functions

| Feature | Model |
|---|---|
| Positional parameters | The ordered list (§4.6). |
| Keyword parameters | Keyword records — name-matched, so names participate and order does not. |
| Typed varargs | A rest parameter, `*t`. |
| C `...` | `*__ty.untyped` — a rest over the reserved placeholder, so every C variadic agrees with every other. Not a top type. |
| Parameter modes | `in` / `out` / `inout` (§7.4). A mode is a property of the slot, never of the type in it. |
| Curried functions | `(a) -> (b) -> c`. Right-associative and unambiguous. |
| ML domain-as-tuple | One positional whose type is a tuple. `(a, b) -> c` and `((a, b)) -> c` stay distinct (§4.6). |
| Overloading | Not a type feature. One name denotes one type; overload sets are a front-end symbol-table concern. |
| `throws`, `async`, effect rows | **Not supported.** Put the effect in the return type where the language allows it; §15 records the reasoning and the upgrade path. |

### 17.6 Polymorphism

| Feature | Model |
|---|---|
| Generic types | `Named` with arguments. `h(F[A])` is derivable from the name and the argument encodings (§11). |
| Type variables | `Variable` under a `Binder` (§4.7, §4.8), indexed positionally so α-equivalence is free. |
| Partial application of arguments | Successive substitution; each step's hash derives from the last (§11). |
| Const generics, value parameters | Argument tags 1–3 (§7.3), encoded already evaluated. |
| Bounded quantification, trait bounds | **Term only.** A bound says which instantiations are legal, not what shape this is. |
| Variance | **Term only.** Two applications of one constructor to the same arguments are the same type whatever the annotation says. |
| Existentials, `dyn Trait` | `Named`. Whether two languages' existentials should agree is a naming decision; their dispatch semantics usually differ enough that they should not. |
| Higher-kinded types | **Not supported.** An argument is a type or a value, so `F[Vec]` passes a *nullary application* of `Vec`, not the constructor. Supporting them needs an argument tag for an unapplied constructor. |

### 17.7 Set-like composites

| Feature | Model |
|---|---|
| Union types (`A \| B`) | `Sum`, `comb = 0` (§4.9). Commutative, associative, idempotent — performed by the format. |
| Intersection types (`A & B`) | `Sum`, `comb = 1`. Encoded; §15 notes there is no spelling yet. |
| Go-style interface | A wrapper over a method record — `go.iface[(read: () -> int)]`. **Not** a bare `Record`, which would collide with a struct having the same function-typed fields. |
| Interface embedding | Flatten the method sets in the front end, as `Sum` flattens. |
| Interface satisfaction | **Not supported**, and not a gap: `T` implements `I` iff methods(T) ⊇ methods(I) is subtyping. |
| Row polymorphism, open records | **Not supported.** A row variable is a binder plus a set-like composite, but making `{x:int \| ρ}` equal the right closed record is unification, not hashing. |
| Refinement and dependent types | **Not supported.** A value-indexed type has no identity until the value is known, which is §10.1's rule. |

### 17.8 Recursion

| Feature | Model |
|---|---|
| Recursive type through a name | `Named` — the definition is never encoded, so the cycle never arises in the digest. Prefer this. |
| Recursive structural type | Cycles are encoded with relative back-references (§5). |
| Equality of bisimilar recursive types | **Front end's job** (§9.8). The encoding is a tree unfolding, not a bisimulation quotient, so `A = (next: ptr[A])` and `B = (next: ptr[C]), C = (next: ptr[B])` hash differently. Minimize the automaton, or use a name. |

### 17.9 Nominal, structural, abstract

| Feature | Model |
|---|---|
| Nominal typing | `Named`. |
| Structural typing | `Record`, `Sum`, `array`, `Indirection` — everything that is not a name. |
| Opaque / abstract type | `Named` with the definition present in the term and absent from the digest. Identity survives a forward declaration for free. |
| Newtype, strong typedef | `Named` with a distinct name. This is the `Meters` / `Seconds` case, and it is the whole reason identity is not layout. |
| Transparent alias | Resolved away before encoding (§9.1). An alias is a spelling, not a type. |
| Separate tag namespaces | Qualify the name (§9.2). |

### 17.10 Access and storage

| Feature | Model |
|---|---|
| `const`, `readonly`, immutability | **Not in the type.** A parameter mode (§7.4), or a property of storage. |
| `volatile` | **Not in the type.** A codegen instruction about an access path. |
| `restrict` | **Not in the type.** Qualifies a declaration. Aliasing that *is* a type property is `EXCLUSIVE` (§7.2). |
| Alignment, packing, offsets, field order | §12's descriptor. Never identity. |

### 17.11 Out of scope by construction

| Feature | Why |
|---|---|
| Subtyping, assignability, coercion | Relations, not identities. The digest answers one equality. |
| C type compatibility | Not an equivalence — `int[]` is compatible with `int[4]` and `int[3]`, which are not compatible with each other — so it has no quotient to hash (§1.2). |
| Implementation inheritance | A semantics question about behaviour, not about which values a type admits. |
| Layout agreement | §12. Pair the two facets; do not conflate them. |
