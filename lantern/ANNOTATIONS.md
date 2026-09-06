# Annotation grammars

Read `PLAN.md` first.

Part 1 describes the annotations that **exist**. Part 2 is **proposals** —
nothing in it is decided, and none of it should be built until it is.

## The rule — built, see `PLAN.md`

Every annotation's arguments are defined by a **grammar**: `fe/annot.bnf`, one
production per annotation, parsed with that annotation's own start symbol.
Grammars compose — an `@infer` argument is the infer grammar; a `@type`
argument is a balanced span handed to the embedder's type parser, because the
type grammar belongs to the embedder, not to `fe`.

- **No hand-rolled parsing** beyond the BNF bootstrap. The fixed four-token
  argument model — `$N`, a bare name, a quoted literal, an integer — is gone.
- **Real syntax, with the quoted form still read.** `@infer($0 unify $1)` and
  `@type($n, list[int])` are the spellings; `@infer("$0 unify $1")` and
  `@type($n, "list[int]")` parse to exactly the same thing through the same
  grammar, because every existing grammar is written that way.
- **A child reference is always `$`-sigilled**: `$2` by index, `$name` by
  non-terminal name. A bare identifier is not a reference, so
  `@literal("int", $type-spec)` reads as what it is.
- Arity and argument kind are grammar facts, so a malformed annotation fails
  the grammar load rather than decoding to a zero or an empty string.

## Shared non-terminals

```
<ref>   ::= "$" <int>          # child by index
          | "$" <ident>        # child by non-terminal name

<tag>   ::= <quoted-string>    # a name-valued argument
<num>   ::= <digits>
<type>  ::= a balanced span, stopping at a top-level comma
<infer> ::= <infer-grammar>    # composed: operations over types
<pred>  ::= <ident> | <ident> "(" <pred-arg>,* ")"
```

"IR text" means the effect is visible in `ir_module_print` output, which emits
module annotations, typedefs, layouts, globals with `static`/`extern` and
mutability, funcs, locals, blocks and instructions.

A grammar annotation is a directive to the *front end*. Almost none of them
should appear in IR text; what appears is what they produced.

---

# Part 1 — What exists

## Scopes and symbols

| annotation | what it says | arguments | in IR text |
|---|---|---|---|
| `@scope` | This production opens a lexical scope. Symbols declared inside belong to it; it closes when the production ends. The optional ref names the scope after a child. | `<name>, <ref>?` | no — IR's structure is funcs and blocks |
| `@declares` | This production declares a symbol. The first ref is the child holding its name; the optional type and kind say what it is. | `<ref>, <type>?, <sym-kind>?` | **yes** — becomes an `ir_local_t` or `ir_global_t` |
| `@type_decl` | This production declares a *type* name — a typedef. | `<ref>` | **yes** — `put_typedefs` |
| `@type` | The child named by the ref has this type. | `<ref>, <type>` | **yes** — the printed type |
| `@visibility` | How visible the declared symbol is outside its scope. | `<visibility>` | **no, and it should be** — this is `is_static`/`is_extern` and `ir_func_t.linkage`, which nothing drives |
| `@static` | The declared member belongs to the type, not an instance. | none | `is_static` exists on globals |
| `@abstract` | The declared member has no body and must be overridden. | none | no |
| `@exposes` | The scope this production opened is reachable from outside — module exports. | `<ref>` | no |
| `@field` | This production declares a record field. | `<ref>, <type>` | the type reaches `th`'s record; the flag does not |
| `@method` | This production declares a method. | `<ref>, <type>` | as above |
| `@adt` | This production declares an algebraic data type: the keyword that introduced it, its name, the scope tag for its arms, and the keyword child. | `<adt-kind>, <ref>, <name>, <ref>` | the `variant` type reaches `th`; the keyword does not |
| `@record` | This production declares a structural record. **Binds a type name and sets up the layout information**: builds the `th` record from the `@field` symbols in its scope, types the node and its symbol, and binds the name in the *enclosing* scope. Field order is a layout fact, so it belongs to the descriptor, not the type. | `$ref` | the record type |
| `@inherits` | The declared type inherits from the type the child names. | `<ref>` | no directly — it changes field order, so it feeds layout |
| `@implements` | The declared type satisfies the named interface **structurally** — `methods(T) ⊇ methods(I)`. | `<ref>` | no — checked, not carried |

## Typing

| annotation | what it says | arguments | in IR text |
|---|---|---|---|
| `@infer` | Compute this production's type by unification. Operations over types only. | `<infer>` | **the resulting type only** — never the expression |

`@infer`'s whole vocabulary, read from `infer_expr.c`:

```
$N              the type of non-terminal child N
$return         the enclosing function's return type
`x              a type variable, same name same variable within one expression
lookup($N)      the type of the symbol child N names
return_of($N)   the return of a function-typed child
element_of($N)  the element of a container-typed child
unwrap_result($N)
method_return($N)
A unify B
A | B
(A)
```

It is about inference by unification and nothing else. Everything else about a
type is stated by other annotations. Never widen it.

## Control and data flow

Each labels a production so the lowering knows what construct it is. The label
never appears in IR text; the lowered structure does.

| annotation | what it says | arguments |
|---|---|---|
| `@assigns` | This production assigns the second child to the first. | `<ref>, <ref>` |
| `@branch` | Conditional: condition, taken branch, untaken branch. | `<ref>, <ref>, <ref>` |
| `@loop` | Loop: condition and body. | `<ref>, <ref>` |
| `@switch` | Multi-way branch: subject and arms. | `<ref>, <ref>` |
| `@jump` | A non-local transfer — break, continue, return. | `<label-kind>` |
| `@call` | A call: callee and arguments. | `<ref>, <ref>` |
| `@varref` | The child is a *use* of a variable, to be resolved against the symbol table. | `<ref>` |
| `@operator` | This production applies an operator of the named kind. | `<op-kind>` |
| `@literal` | "This is my literal syntax for *kind*." **Passes the data down** so the IR can load a literal: it types the node from the type child if there is one, else from the kind's name. The kind, the token text and the type are then all reachable from the node. | `<tag>, $ref?` |
| `@capture` | Bind this subtree to a tag so a later rule or rewrite can refer to it. `dynamic` resolves the tag at match time. | `<name>, "dynamic"?` |
| `@notrivia` | No whitespace or comment may attach to this child — it must be adjacent. | `<ref>` |

## Parsing control

| annotation | what it says | arguments |
|---|---|---|
| `@penalty` | Cost charged when this rule is used, biasing ambiguity resolution against it. | `<int>` |
| `@disambig` | Resolve ambiguity here with this predicate, at this cost. | `<pred>, <int>` |
| `@tokenizer` | Which tokenizer the grammar is scanned with. Sets `ncc_grammar_tokenizer`, resolved through the registry at scanner creation; the default is `"text"`. | `<tag>` |

## Pretty-printing

All take a single `<ref>` and are Wadler directives describing how to lay out
the **source language** being parsed — not the IR.

`@indent` `@group` `@align` `@concat` `@space` `@nospace` `@newline`
`@softline` `@hardline` `@blankline`

---

# Part 1b — Every annotation, against "no code required"

## How the expressiveness actually comes about

Two mechanisms do most of the work, and neither is an argument form:

**Annotations scope to the production, not the non-terminal.** An annotation
on one alternative covers only that alternative. Writing the non-terminal as a
single production with `|` shares the annotations across every branch. So the
grammar author chooses the granularity by how they split productions.

**Annotations accumulate against the symbol most recently declared.** The walk
carries `last_sym`, so an annotation on a nested or repeated production applies
to the declaration that encloses it.

Together these mean decomposition happens **into productions**, not into
cleverer arguments:

```
<modifier> @static   ::= "static"
<modifier> @abstract ::= "abstract"
<decl> @declares($2,"var") ::= <modifier>* <name> ";"
```

Any combination of modifiers, one production each, no explosion. The same
pattern covers multiple interfaces — `annot_implements` pushes onto a list, so
`<iface-ref> @implements($1) ::= <qualified-name>` under a `+` accumulates —
and it covers every keyword-valued argument: `@operator("add") ::= "+"`,
`@jump("break") ::= "break"`, `@visibility("public") ::= "public"`. Literal
arguments are right for those, and no ref is wanted.

## What already works well

`$N` is **0-based over all children**, terminals included.

**A requirement stated as a unification.** From `n00b.bnf`:

```
<for-in-target-list> @infer("$0 unify list[`t]|array[`t]") ::= <expression>
```

That one line says "whatever is iterated must be a list or an array, and `t`
is its element type". Three things make it work with no code:

- A type variable is shared by name across the expression, so `` `t ``
  appearing in both arms binds them together and the element type falls out —
  no `element_of()` needed.
- A union as the unification target says "one of these shapes", which covers
  much of what a `one_of` constraint would.
- Unification failure *is* the type error, so no separate assertion or
  reporting mechanism is required.

**Saying what a production is, rather than what to do about it.** `@branch`,
`@loop`, `@switch`, `@jump`, `@call`, `@assigns`, `@varref` label a production
as a kind of construct and the lowering does the rest. Combined with
production-scoped annotations, one production per kind of construct covers a
language's control flow declaratively.

**Composition with repetition.** Because annotations accumulate against
`last_sym`, a repeated sub-production annotates the enclosing declaration
however many times it matches — modifiers, interfaces, attributes.

## What is actually missing

Only two things resist that decomposition, because in both the value is
arbitrary source rather than a fixed keyword.

**1. `@type` cannot take a type from the parse.** Its type argument is a
literal, so a declared type as the source writes it cannot be named. You
cannot write a production per possible type. `@type($1, $2)` — name child,
type child — is the shape, and the whole `translate_type_spec` callback exists
to work around not having it.

**2. Nothing extracts a value from a referenced child.** `ref_name` pulls an
identifier out of a node; there is no ref-to-integer, so `@size($2)` cannot
read the `3` out of `x : 3`. The annotation shape is right and the extraction
does not exist.

## Common semantics still unstateable

- **Mutability** — proposed below; the IR carries it now and no grammar can
  say it.
- **Field and parameter defaults** — upstream's record type carries
  `has_default` per field. Nothing states it, so a language with default
  arguments cannot express them.
- **Rest / variadic parameters** — `typelang.bnf` has `<rest-param>` and no
  annotation marks a production as one.
- **Parameter direction** — proposed as `@direction` below.
- **Coercion policy** — `th_coerce` takes a policy and no annotation supplies
  one, so a language cannot state which implicit conversions it permits.

# Part 2 — Proposals

Not decided. Nothing here should be built until it is.

## `@mutability` — because the IR now has it and nothing states it

`ir_local_t` and `ir_global_t` carry `ir_mutability_t` and print it, and
`ncc_sym_entry_t.mutability` exists, but the only thing that ever sets the
latter is `kind == PARAM ? IMMUTABLE : MUTABLE`. No grammar can say it.

```
@mutability ( <ref> , "mutable" | "immutable" | "const" )
```

Not a type property — `gates.sh:112` rejects access modifiers in the type
model, and `is_static`/`is_extern` set the precedent that declaration
properties live on the node.

## `@namespace` — which namespace a declaration belongs to

The symbol table has namespaces; only `""` and `"tag"` are ever used, both
hardcoded in `annot_walk.c`. A language with separate type and value namespaces
cannot express that.

```
@namespace ( <tag> )
```

Named `@namespace`, not `@ns`: an abbreviation that has to be looked up is a
worse name than the word.

## `@direction` — a parameter's direction

Only for a signature assembled from the tree, where a parameter node has no
written-out type to carry a direction. A written type already carries it.

```
@direction ( $ref , "in" | "out" | "inout" )
```

Named `@direction`, not `@mode`: "mode" says nothing about what varies.

## `@constrain` — the constraint shapes `th` already supports

Syntax deliberately the one `th_print` already emits for a `where` clause, so a
constrained variable round-trips between printer and grammar rather than having
two spellings. `@implements` stays the declaration-site form; this is how a
*use* site states a bound.

```
@constrain ( <ref> , <constraint-list> )

<constraint-list> ::= <constraint> ( "+" <constraint> )*

<constraint> ::= "==" <type>                                # unifies with
               | "!=" <type>                                # never this type
               | "one_of" "(" <type-list> ")"
               | "promotes_to" <type>                        # safe widening
               | "has_field" "(" <name> ":" <type> ")"
               | "has_param" "(" <int> "," <type> ")"
               | <iface-name>                                # implements
```

## Pinning representation — one annotation per property

An annotation states a property of the node its production yields, so there is
no `<ref>` and no bundle. `@size` goes on the production that yields the
aggregate; the member bit range goes on the production that yields the
*member*.

```
@size      ( <val> )    # this node's result is this big
@alignment ( <val> )    # aligned to this
@offset    ( <val> )    # this member starts here, in bits, within its aggregate
@width     ( <val> )    # this member occupies this many bits

<val> ::= <ref>                                  # read it out of the parse
        | <int>
        | "sizeof"   "(" <type> ")"
        | "alignof"  "(" <type> ")"
        | "offsetof" "(" <type> "," <name> ")"
```

**`<ref>` is the important case.** The value usually comes from the source, not
from a constant the grammar author wrote. A C-style bitfield is just a
production whose width is a child:

```
<bitfield> @size($2) @infer($2, uint32_t) ::= %IDENTIFIER ":" <simple-int-literal>
```

**Why `@alignment` and not `@align`.** `@align` was already taken, by the
Wadler pretty-printing family in "Pretty-printing" above, where it means
"indent continuation lines to the current column". The two have nothing to do
with each other and an annotation kind is a single flat namespace, so the
layout one is spelled `@alignment`. The printing one keeps `@align` because it
shipped first and because that is what every Wadler-derived printer calls it.

so no bitfield-specific syntax is needed — `@size` pointing at the width child
is the whole mechanism. This also means the value is known per *parse*, so it
is not target-dependent, which is a different situation from a grammar author
pinning a constant for an ABI.

`@offset` and `@width` are the two facts `ir_member_layout_t` actually stores.
The IR text form renders them as a range — `ir_print.c:359` prints
`name @ bits offset..(offset+width)` — but the end of that range is derived, so
an annotation must not ask for it.

`sizeof` / `alignof` / `offsetof` as operators are required, not decoration:
§12 of `~/th/spec.md` says a descriptor must be emittable symbolically so a
cross-compile can defer it to the target compiler, and `ir_layout_val_t`
already carries `IR_SYM_SIZEOF`/`ALIGNOF`/`OFFSETOF`.

**No `@stride`.** Stride is the distance from one array element to the next.
`ir_layout.c` defaults it to `round_up(size, align)` and uses it as
`stride * n` to size an array, so it differs from size only by tail padding
and is derivable. Only an ABI that disagrees with the default would need to
pin it, and no such case is in front of us.

### Built as far as the two open questions allow

All four parse, decode, and land on the node as `ncc_repr_pin_t` — the value
plus a `has_` flag per property, filled by the `annot_repr` consumer in
`PHASE_REFINE`. `<ref>` resolves against the child it names, so the bitfield
case above works.

What is deliberately *not* built is the bridge from that pin to
`ir_layout_pin_size` / `_align` / `_member`, because two questions have to be
answered first and neither can be guessed:

**Units.** `ir_layout_pin_size` / `_align` / `_stride` take a parameter named
`bytes`; `ir_layout_pin_member` takes `bit_offset` / `bit_width`. §12 requires
member positions in bits for every member, not only bitfields. If `@size` is
in bits then the aggregate pins move to bits as well, otherwise the family
mixes units. The pin records the number the grammar stated and does not tag it
with a unit, so calling `ir_layout_pin_size` with it would be inventing one.

**Target.** Descriptors are per-target and a grammar production cannot know
the target. Either these pins are target-agnostic — assertions that must hold
everywhere — or there has to be a way to scope one to a target. Until that is
settled there is no correct descriptor to pin *into*.

## Constraint shapes `th` does not have

- **`can_cast_to(<type>)`** — an explicit, possibly lossy conversion, against
  `promotes_to`'s safe widening. Needs a cast relation beside
  `th_register_promotion`.
- **`has_trait(<name>)`** — *declared* satisfaction. Duck-typed interfaces need
  only `methods(T) ⊇ methods(I)`; a language requiring the type to say so needs
  **the declaration and `methods(T) ⊇ methods(I)`** — declaring it does not
  excuse satisfying it. So it is `implements` plus a check that the declaration
  exists, which means declarations must be recorded against the type.
- **`is_kind(record | function | sum | indirection | sequence | ...)`** —
  constrain a variable to a `th_kind` without naming a type. "Must be callable"
  is not expressible.
- **`sized`** — must have a known representation. The first constraint that
  reaches past the type into the descriptor.
- **`arity(<int>)`** — parameter count, for forwarding and partial application.

## Not a proposal: type structure needs no annotations

Parameter modes, indirection attribute bits, sequence extents and `bitint`
width and signedness are **part of the type** — `th` encodes all of them and
`typelang.bnf` already spells every one. They need `@type` to take a real type
instead of a quoted string: `@type($n, *owning int)`, `@type($n, [10]i32)`.

## Gaps in the IR text form, not in any annotation

- `ir_descriptor_t` holds variant tag placement, `niche`, and an indirection's
  `fat`/`words`; the `layout` block cannot spell any of them.
- `put_layouts` prints only the descriptor matching the module's own target,
  and the printed block carries no target — a module with descriptors for two
  targets prints one and loses the other on reparse.
