# ~/ir — migration plan

Read before doing anything. Authoritative over anything inferred from the code.

This file says what **is** and what is **next**. It is not a changelog.

---

## 1. What this project is

> This project is pulling together the last 10 years of my compiler work into a
> single C api that is generic and reusable with simple clearly defined APIs.
>
> Ultimately my front-end makes it easy to start with a spec for any language
> and then build tooling, from compilers to linters. You keep trying to pull
> everything towards any single language and that's the worst thing that can
> possibly happen. The second that happens all your work goes bad immediately.

**This is one library — `libir`.** One `library()` target, one archive. The
three source trees are **modules**, not layers and not link units:

| module | path | holds |
|---|---|---|
| types | `th/c` | the type descriptor, the unifier, typelang |
| front end | `fe/src`, `fe/include` | scanning, parsing, annotations, symbols, trees |
| IR | `src`, `include/ir` | the IR, the module, passes, text |

`fe/include/lib` (alloc, arena, buffer, list, dict, array, string) is the
shared foundation. Annotations are their own module inside the front end
(`annotation.c`, `annot_args.c`, `annot_grammar.c`, `annot_walk.c`).

**Any module may call any other module. Link order constrains nothing.**

**Do not invent an abstraction to avoid an include.** Every callback, forward
declaration and `void *ctx` added to keep these apart has been a mistake.
`ncc_type_binder_t` and `ncc_type_syntax_t` were both that mistake; both are
gone. Annotation code calls `ir_type_from_text` and `ir_type_from_node`
directly.

`ncc_bnf_load` is reentrant — it saves and restores its per-load statics —
because loading a grammar can now trigger loading typelang's grammar.

---

## 2. Upstream

Port source is **`~/n00b`**: `src/slay`, `src/typecheck`, `src/parsers`.

**`~/n00b` is a partial checkout.** Do not conclude a feature is unnecessary
because you cannot see its callers. If it is there, assume it is useful.

Upstream is a source, not an oracle. It has real defects and one hard
structural difference: it is one binary, so `infer_expr.c` calls the type
checker directly. Here the libraries are separate, which changes *where* code
lives, never *what* it does.

Design docs: `~/th/spec.md` (typehash, source of the 66 conformance vectors),
`~/n00b/docs/slay_type_system.md`, `~/n00b/docs/annotation_design_review.md`.

---

## 3. The ask

1. Make the passes separately callable.
2. Let people add their own walk callbacks to a pass.
3. Build a separate tokenization library and abstraction.

Not a wholesale port. Earley stays behind.

**All three are done.** See §11.

---

## 4. Architecture (settled — not negotiable)

1. **One type descriptor: `th`.** It grows spans, row polymorphism,
   constraints, named type vars.
2. **A single implementation descriptor**, separate from the type, mapping type
   info to implementation info: layout, defaults, representation, ABI. Keyed by
   the type hash.
3. **Interfaces are a higher-level concept** with a mapping down to the type
   hash. Not a constraint shape that mentions a name.
4. **The deterministic type hash is critical.**
5. **Coercion is not part of the type descriptor.** It is an operation applied
   during typing: `coerce(type, policy) -> type`, possibly bottom.
6. **Spans are critical**, in the type layer.
7. **The constraint engine comes across**, to make high-level type constraints
   easy to express.
8. Anything layout- or implementation-specific — **including field defaults** —
   goes in the implementation descriptor.

### 4.1 The reserved vocabulary

Three distinct names under spec §4.1's `__ty.` prefix. None is a top type.

- `__ty.untyped` — spec §4.6: the placeholder a rest parameter takes so every
  C variadic agrees with every other. Not erased, not bottom.
- `__ty.erased` — the C-like "we don't know or care, there is one type and it
  is memory." Inhabited, has a representation.
- `__ty.bottom` — no value. What a failed coercion yields. Uninhabited.

All three are nullary `Named`, so they encode like any named type.

### 4.2 What the hash does not carry

Spec §17.7: row polymorphism and open records are **not supported** in the
hash — *"making `{x:int | ρ}` equal the right closed record is unification, not
hashing."* That line separates every inference concern from identity.

Row-openness lives in the unifier. Named vars and constraints live on the
in-memory node, unencoded. Field order is a layout fact (§7, §12) and belongs
to the implementation descriptor — spec §14 deleted `ORDERED` for this reason
and it stays deleted.

Tuples stay `Named`: structural is `__ty.tuple[...]`, nominal is
`MyName[...]`. Order is intrinsic to an argument list. Records have no order.

---

## 5. Ownership and lifetime

**This section exists because getting it wrong produced an invented
`ncc_type_binder_t` and a plan to key per-node state off node addresses.**

Three lifetimes. Never merge them into one struct.

### 5.1 The module owns the durable state

One owner per translation unit, alive as long as the module is:

- the **parse tree**
- the **symbol table** — one tiered table: namespaces → scopes
  (`parent`, `depth`, `id`) → entries (`next_in_scope`, `shadowed`)
- the **type universe** (`th_ctx`) — interns types, holds the error list, the
  promotion relation, the interface registry
- the grammar it was parsed with

The tree and the symbol table are already cross-linked both ways:
`ncc_sym_entry_t` has `decl_node`, `ncc_scope_t` has `node`.

`ir_module_t` is that owner. It holds `tree`, `tree_grammar` and `symtab`
alongside types, globals, funcs, rules and files, and reaches the `th_ctx`
through `ir_ctx_t`:

```c
void ir_module_set_tree(ir_module_t *, ncc_grammar_t *, ncc_parse_tree_t *);

ncc_parse_tree_t *ir_module_tree(ir_module_t *);
ncc_grammar_t    *ir_module_grammar(ir_module_t *);
ncc_symtab_t     *ir_module_symtab(ir_module_t *);
th_ctx           *ir_module_types(ir_module_t *);
```

The symbol table is created with the module and freed with the context. There
is no second owner and no `fe`-level module: it is one library, so a linter
uses `ir_module_t` too and simply never builds any IR.

### 5.2 The node owns its own annotation state

Per-node analysis results live **on the node**, not in a side table:

```c
typedef struct ncc_nt_node_t {
    ...
    ncc_scope_t     *scope;   /* already there */
    ncc_sym_entry_t *sym;
    th_type         *type;
} ncc_nt_node_t;
```

104 → 120 bytes per node, against a dict entry plus a hash per lookup.
`scope` already set this precedent.

**This is what makes application order flexible.** A table keyed by node
address cannot be filled while the tree is being built — the node is not in
the tree yet. With the state on the node, applying an annotation during
construction and applying it in a later walk write to the same slot, and
either is allowed.

Every node and every symbol entry gets a type variable when it is created, not
lazily.

### 5.3 A pass owns only its own temporaries

Each pass carries a small private struct: `last_sym`, pending visibility,
pending mutability, an anonymous-scope counter, the `` `x ``→var memo inside
one `@infer` expression. Nothing else.

`annot_walk.c`'s `actx_t` is the model. **One uber context shared by every
pass is wrong** — upstream's `n00b_annot_walk_ctx_t` has eleven fields
spanning all three lifetimes; do not copy its shape.

### 5.4 typelang is the target, so there is no hook

**typelang is not one language's type syntax. It is *our* syntax, the one a
source language maps its types to** — the surface form of the `th` descriptor,
and what the 66 conformance vectors are written in. So there is nothing to
plug in and no per-language callback:

```c
th_type *ir_type_from_text(th_ctx *, ncc_string_t);                       /* @type's argument   */
th_type *ir_type_from_node(th_ctx *, ncc_grammar_t *, ncc_parse_tree_t *); /* a type subtree     */
```

Annotation code calls these directly. `from_node` is needed by `@infer` too,
for the `...$M` spread, which turns each type-relevant child of NT-child M
into a type.

A source language expresses its types **in typelang**, in the annotation
(`@type($1, dict[int,string])`, `@infer(list[$0])`), or by reducing its own
type subtrees through typelang's reducers (`ir_typelang_install`). Both go
through one implementation.

`ncc_annot_walk_tree(g, tree, types)` and `ncc_annot_register(w, out, types)`
— a null `th_ctx` means the typing work does not run, which is how typing
stays opt-in.

`ncc_type_binder_t` and `ncc_type_syntax_t` are both deleted, and so is the
separate `bool`-returning `ncc_annot_type_checker`: grammar-load validation of
a `@type` argument calls `ir_type_from_text` against a scratch `th_ctx`.
**One implementation of text→type, not three.**

Everything else in the evaluator calls `th_*` directly, as `infer_expr.c`
calls `n00b_tc_*`: `th_name`, `th_apply`, `th_sum`, `th_var_named`,
`th_unify`, `th_func_ret`, `th_seq_elem`, `th_named_info`.

---

## 6. The pass model

A pass is a set of callbacks attachable to **any** tree walk, not a fixed
pipeline stage.

- A pass declares which node fields it **requires populated** and which it
  **populates**. Checked when the pass is attached, not documented in prose.
- Symbols are populated before types, because a type propagates to symbol
  entries as well as to nodes.
- Users add their own callbacks to a pass. This is ask #2, and it is why every
  transformation must assume annotations are **not** enough on their own.
- Passes are individually callable. This is ask #1.

**Lowering is deferred.** An annotation's effect is carried on the node and
applied when the tree is converted to IR, not at annotation time.

**Syntactic sugar is a pass.** If a construct is well-formed in the grammar,
rewrite rules restate it in terms of other constructs —
`with (x) { }` → `context_init(x); … context_end();`.

**Typing is opt-in**, through annotations, on one tree — never a forest. If the
type hook and `th_ctx` are absent, the typing pass does not run and nothing
else changes.

---

## 7. Annotations

`ANNOTATIONS.md` is the reference for all 39. Summary of the mechanism:

- The BNF bootstrap knows only that an annotation's arguments are a balanced
  `( … )`. It slices that **byte span** and hands it to
  `ncc_annot_parse_args`, which re-lexes and parses it with **that
  annotation's own start symbol** from `fe/annot.bnf`.
- Arity and argument kind are grammar facts, so a malformed annotation fails
  the **grammar load**, naming the offending text.
- A child reference is always `$`-sigilled: `$2` by index, `$name` by
  non-terminal name. A bare identifier is not a reference; a name-valued
  argument is a quoted string.
- `@infer` arguments are the infer grammar (`fe/infer.bnf`), parsed once at
  load into `ncc_infer_expr_t` on the annotation — never re-parsed per node.
  `$$` is the production's own type; `$return` is the enclosing function's.
- `@type`'s argument is a balanced span, validated at load through the
  registered type hook. The type *grammar* belongs to the embedder.
- Annotations scope to the **production**, not the non-terminal.
- Arguments print verbatim from `args_text`. There is no per-kind argument
  printer to keep in step with the decode.

---

## 8. Printers

Every printer builds its string through `ncc_buffer_t` and returns `char *`.
Three in `fe`, and they do three different jobs:

| printer | input | driven by |
|---|---|---|
| `ncc_tree_dump_str` | parse tree | nothing — structural |
| `ncc_tree_print` | parse tree | annotations where present, one space where not |
| `ncc_bnf_print` | `ncc_grammar_t` | the grammar itself |

`ncc_bnf_print` is not tree printing: a composed or C-built grammar has no
parse tree behind it. `fe/test/test_bnf_print.c` round-trips `toy.bnf`,
`n00b.bnf` and `c_ncc.bnf` to a fixpoint and requires annotation and rule
counts to survive.

`ncc_trivia_t` carries a `kind`. The formatter renders comment trivia and
ignores whitespace trivia — **layout is the formatter's decision, not the
input's.**

---

## 9. Discipline

1. **Never invent a type at a seam.** Use the real one or port it. If a seam
   seems to need a new struct, the lifetimes are probably confused — reread §5.
2. **State lives with the thing it describes.** Per-node state on the node,
   per-pass state in the pass, durable state on the module.
3. **Never port by rewriting from scratch.** Read the upstream file, port it,
   then diff the API surface against it. Divergences are written down as
   deliberate, or fixed.
4. **No annotation may be parsed without a consumer.** Enforced by
   `test/gates.sh`, which fails in both directions. `PENALTY` and `DISAMBIG`
   are consumed at grammar-build time in `bnf.c` and read by `pwz.c`.
5. **One implementation of anything.** Containers are `ncc_list_t` /
   `ncc_dict_t`; strings are built with `ncc_buffer_t`. Both gated.
6. **Look at the target before deleting or overwriting it.**
7. Comments and docs in existing code are not a spec. **The grammars are.**
   `fe/test/n00b.bnf` is a specification, not a fixture.
8. `~/CLAUDE.md` applies: no comments in code; VCS is the user's; never invent
   numeric values; use existing typed machinery.

### Verification standard

Both trees green — `build` and `build-asan` — plus
`ir_typelang_test th/vectors.tsv` reporting 66. Benchmarks are not run by
default.

### Portability constraint

`n00b_variant_t` is built on ncc's compile-time `typehash(T)`, `typeid`,
`_generic_struct`. `~/ir` is plain clang `-std=c2x` with no ncc. Anything
using variants — the 7 constraint shapes, the 7 type kinds — needs a
kind-enum + union encoding here.

---

## 10. What is missing

| upstream | lines | what it is |
|---|---|---|
| `slay/transform.c` | 764 | the pass framework — §3 asks 1 and 2 |
| `slay/infer_expr.c` | 1213 | the `@infer` interpreter |
| `slay/annot_symtab.c` | 765 | walk phases 2–3, partially ported |
| `slay/annot_types.c` | 292 | the inference phase |
| `slay/rewrite.c` | 816 | BNF rewrite blocks with captures — §6 sugar |
| `slay/cfg.c` `cdg.c` `dfg.c` | 1565 | **do not port** — §12 |
| `slay/analyze.c` | 366 | all six checks accounted for — §12 |
| `parsers/tokenizer_registry.c` | 79 | the tokenizer abstraction — §3 ask 3 |
| `parsers/{json,lisp,shell,text}_tokenizer` + `slay/c_tokenizer` | 1057 | concrete tokenizers |
| `typecheck/unify.c` | 730 | ported |
| `typecheck/print.c` | 533 | ported |
| `typecheck/construct.c` | 451 | ported |
| `typecheck/context.c` | 422 | ported |
| `typecheck/hash.c` | 187 | ported |
| `slay/diagnostic.c` | 397 | superseded by `ir_diags_t`; only the bridge is wanted |
| `slay/pprint.c` | 876 | `tree_print.c` written fresh instead |
| `slay/grammar_image.c` | 701 | a real capability we do not have — needs a yes/no |
| `slay/debug.c` | 1150 | **do not port** — printers, all covered or do-not-port |

Out of scope: `earley.c`, `earley_forest.c`, `codegen*`, `n00b_parse.c`.

### `slay/debug.c`, triaged

Every one of its ten entry points is a printer, and each is already covered or
already refused:

| upstream | here |
|---|---|
| `n00b_grammar_print` | `ncc_bnf_print` |
| `n00b_tree_print` | `ncc_tree_print` / `ncc_tree_dump_str` |
| `n00b_parse_node_repr` | `ncc_tree_dump_str` |
| `n00b_forest_print` | enumerable: `ncc_parse_forest_count` + `_tree` + `ncc_tree_dump_str` |
| `n00b_parser_print_states` | Earley-specific; `earley.c` is out of scope |
| `n00b_cfg_print` `n00b_cf_labels_print` `n00b_cdg_print` `n00b_dfg_print` | printers for the tree-level graphs §12 refuses; the IR-level equivalents already print |

So: nothing to port.

### `slay/grammar_image.c` — the one real capability gap

It bakes a finalized grammar into an offset-relocatable image, wraps it in a
section record, emits linkable object bytes, and repairs the image at runtime
(re-hashing dicts, re-binding action hooks, which are process-local and cannot
be baked). The point is that a compiled tool does not re-parse its own BNF at
startup.

We do not have this, and it is not duplication of anything we do have. It
depends on upstream's `util/comptime_image.h`, which is not here either. Two
questions before any of it is worth writing:

1. Is startup grammar-parsing cost actually a problem for the tools this
   library is for? `ncc_bnf_load` + `ncc_grammar_finalize` on a real grammar
   has not been measured against a startup budget.
2. Emitting ELF object bytes puts an object-file writer in a front-end
   library. A plain serialize/deserialize to a blob would get the startup win
   without that, and is a fraction of the code.

### Do not port these

- `n00b_tc_translate_type_spec` — hardcodes slay's `<tspec-*>` NT names, baking
  one language's type grammar into the library. §1 forbids it.
- `n00b_diag_ctx_t` — `ir_diags_t` is strictly richer. Port only
  `n00b_diag_import_tc_errors`, the bridge, when something drives inference.
- `th_clone` — was a hand-rolled `instantiate`. Deleted; non-binding
  unification comes from universal quantification.

---

## 11. Phases

### Done

- **Type descriptor.** `__ty.erased` / `__ty.bottom`; `th_var_named`; path
  compression; the 7 constraint shapes checked at bind time and unencoded; the
  interface registry with `th_implements` / `th_infer_ifaces`; `th_coerce`;
  row-openness as `HAS_FIELD`/`HAS_PARAM` accumulation that merges on
  var-to-var binding. `TH_BINDER` carries generalized vars so a quantified
  variable keeps its name and constraints. Zero hash impact.
- **Implementation descriptor.** Already existed as `ir_descriptor_t` in
  `include/ir/layout.h` — size, align, stride, ordered members with per-member
  bit offset and width, indirection repr, variant/niche, target-scoped,
  emittable symbolically. It carries `type` and `target` back-refs, so §1.3's
  decision is a lookup from either end. **Do not write a second one.**
- **Diagnostics with spans.** `th_error_t` with kind, message, name,
  expected/got, primary + related span; 14 kinds; every `uni` failure names
  what went wrong. `where`-clause rendering of constrained vars.
- **Type-spec bridge**, as an integration: `ir_typelang_install` +
  `ncc_tree_walk_reduce` lowers a parse subtree to a `th_type`, and another
  language installs its own reducers the same way.
- **Annotation argument grammars.** All 39, per §7.
- **`@infer` parse side.** `fe/infer.bnf` + `ncc_infer_expr_t`, read off
  `infer_expr.c` rather than its header doc. 28 corpus shapes covered.
- **Printers**, per §8.

### The three asks — done

**1 and 2: passes separately callable, with user callbacks.** `ir_pass_t`
already had the shape (`name`, `requires`, `establishes`, `invalidates`,
`run`) and `ir_pass_run` already returned `IR_PASS_UNMET`. Since the module
owns the tree, **a tree pass is just an `ir_pass_t`** — no second pass type.
`transform.c` was **not** ported: `ncc_tree_walk_t` is already its registry
(`ncc_walk_on_nt` per-NT pre/post, `ncc_walk_on_node` wildcard,
`ncc_walk_on_annot_phase`) and `lib/tree.h` already had its mutation
primitives. Building a second registry beside a working one is the
duplication.

`ir_tree_pass.c` publishes five, each declaring its prereqs as node-state
properties:

| pass | requires | establishes |
|---|---|---|
| `annot.scope` | TREE | SCOPED |
| `annot.symbols` | TREE SCOPED | SYMBOLS |
| `annot.refine` | TREE SCOPED SYMBOLS | TREE_TYPED |
| `annot.cflow` | TREE SCOPED SYMBOLS | CFLOW |
| `annot.infer` | TREE SCOPED SYMBOLS | TREE_TYPED |

plus `annot.symcheck`, which requires CFLOW. `ir_tree_pass_run(p, m, hook,
state, d)` takes a hook that adds the caller's own callbacks to that pass's
walk. A later pass **re-enters** the scopes recorded on the nodes rather than
recreating them, which is only possible because §5.2 put them there.

**3: tokenizer.** `ncc_tokenizer_register` / `_for`, `ncc_grammar_tokenizer`,
`ncc_tokenize`, default `"text"`. `@tokenizer` sets it at grammar load.
`ir_grammar.c`'s inline tokenizer is registered as `"ir"` and `src/ir.bnf`
names it, so `ir_lex` goes through the registry like anything else.

### Also done

- **Rewriting.** A visitor sets `v->replace`; `ncc_tree_walk_rewrite` splices
  it in and counts replacements, and `ncc_tree_rewrite` iterates to a fixpoint.
  `ncc_tree_clone`, `ncc_tree_make_nt`, `ncc_tree_make_token`.
- **Templates.** `ncc_template_parse(g, start_nt, source, tokenize, err)`
  parses a rewrite target **as source** at a chosen start symbol;
  `ncc_template_instance` clones it. The sugar case works end to end:
  `with (r) { a; b; }` → `context_init ( r ) ; a ; b ; context_end ( r ) ;`.
- **`@infer` evaluated.** `ncc_infer_eval` walks the AST calling `th_*`
  directly. Node types live on the node, auto-propagate from a sole
  non-terminal child, and unify with the node's symbol.
- **The three symbol-table checks** from §12: `IR-W0032` unused, `IR-W0033`
  shadowed, `IR-E0034` undefined, from a use count on `ncc_sym_entry_t` that
  `@varref` increments. No flow graph involved.
- **Every annotation has a consumer.** `expected_unconsumed` in `gates.sh` is
  empty; `TOKENIZER` joined `PENALTY`/`DISAMBIG` as consumed at build time.

### Also done — the four that were "Next"

- **`fe` public headers.** The installed surface is `ncc.h` and all of `lib/`,
  `parse/`, `scanner/`, `unicode/`. `internal/` is not
  installed, which is what that directory name declares. Verified by
  installing to a staging root, compiling a translation unit that includes
  only installed paths, and linking it against `libir.a`.
- **The concrete tokenizers.** `json`, `lisp`, `shell`, `c`, registered by
  name alongside the `text` default; `ncc_tokenizers_register_builtin`.
- **`ir_import_type_errors`** — the bridge from `th_error_t` to `ir_diags_t`,
  mapping `th_err_kind` to `ir_diagcode_t`, rendering types through
  `ir_type_text` (never `th_print`, per the gate), attaching the related span
  as a secondary, and clearing the context's errors so nothing reports twice.
  Also exposed as the `annot.type-diags` pass.
- **`ANNOTATIONS.md` Part 2** — `@mutability`, `@namespace`, `@direction`,
  `@constrain`, `@size`, `@alignment`, `@offset`, `@width`, each with an
  argument grammar in `annot.bnf`, a decoder, a consumer, and a rejection
  test. `@align` was already the Wadler printing annotation, so the layout one
  is `@alignment`. The four representation pins land on the node as
  `ncc_repr_pin_t`; the bridge from there into `ir_layout_pin_*` is
  deliberately absent — see the two open questions below.
- **`IR-F0008 OUT_OF_MEMORY` and the whole `FATAL` tier** — *deleted*. No
  library code could emit it: the one allocator `abort()`s when `calloc`
  returns null, so there is no failable path to report. And since diagnostic
  codes are fixed in `ir_diags.def`, no consumer could emit one either, and
  policy escalates only as far as `ERROR` — so `ir_diags_fatal()` could only
  ever return false, which is a lie in the public API. Gone: the code, the
  `IR_SEV_FATAL` value, the `fatal` flag and its merge, `ir_diags_fatal()`,
  and the naming and policy branches. Severity is still ordered most- to
  least-severe, so sorting is unaffected. The surviving pipeline test asserts
  what actually holds — errors accumulate rather than halt.

  Two gates now keep this shut: a declared code that no library code can emit
  **fails** instead of being reported as "pending" (the old loophole where
  this sat), and a severity nothing can produce fails too — where "produce"
  means a code defaults to it or `is_attachment` accepts it, so `NOTE` and
  `HELP` stay legitimate.

### Also done — the styled-string layer and the hash

- **The styled-string layer — *deleted*.** `lib/style.h` in its entirety
  (`ncc_text_style_t`, `ncc_style_record_t`, `ncc_string_style_info_t`,
  `ncc_text_case_t`, `ncc_tristate_t`, `ncc_color_t`, `NCC_COLOR_VALID_BIT`,
  `ncc_option_size_t`), plus `ncc_string_to_ansi` and `emit_sgr`. Every one of
  those symbols had zero users outside `style.h` and `string.c` themselves,
  and `ncc_string_t.styling` was only ever assigned `nullptr` — so the ~90-line
  styled render path was unreachable, which is also why nobody noticed it
  uppercases byte-at-a-time and would corrupt any non-ASCII text. Nothing in
  the tree does ANSI rendering through this; the diagnostic renderer has its
  own. `ncc_color_t` was additionally declared a second time, identically, in
  `ncc.h`; both are gone.

  `ncc_string_t` is now `{char *data; size_t u8_bytes;}` — 16 bytes, down from
  32. `codepoints` went with it: five writes, zero reads, and every write
  stored the *byte* length, so the field was a lie as well as dead.

  `ncc_string_borrow(src, len)` is now declared where it belongs.
  `annot_walk.c` had hand-rolled it as a private `borrow()` plus a `LIT()`
  macro duplicating `NCC_STRING_STATIC`, because `string.h` offered no
  non-copying constructor. Folding that in exposed a live bug: `adt_scope_open`
  borrowed a `char buf[32]` scoped to an inner `if` block and then used the
  string after the block closed, so `push_scope_in`'s `dup_str` was copying
  out of a dead stack slot. `buf` is hoisted to the function.

- **`ncc_hash_raw` is XXH3 again.** `fe/include/lib/hash.h` had a hand-rolled
  `static inline` hash: two independent 8-round FNV-1a passes over the same
  bytes, composed into the 128-bit value. That came in verbatim from the
  standalone `ncc` extraction (`~/n00b/subprojects/ncc/include/lib/hash.h`),
  whose own header said it was substituting FNV for the runtime's real hash.
  Meanwhile this library *already* links libxxhash — `th/meson.build` makes it
  a hard dependency and `th/c/typehash.c` calls `XXH3_128bits` — so `libir.a`
  carried two hash implementations and the parser dictionary used the
  hand-rolled one.

  Now `fe/src/lib/hash.c` implements the whole family over XXH3, in the shape
  both `n00b` and `bCe` use: `extern` declarations in `lib/hash.h`,
  `XXH_INLINE_ALL` in the one `.c`, `ncc_xxh3_64bits_raw` and
  `ncc_xxh3_128bits_raw` exposed separately, and a
  `static_assert(XXH_VERSION_NUMBER >= 800)` because XXH3 is only value-stable
  from 0.8.0.

  `ncc_hash_value_t` is `uint64_t` and `ncc_hash_raw` delegates to the 64-bit
  variant, which is the split `n00b`'s own `core/hash.h` documents —
  `n00b_xxh3_64bits_raw` is "used by parser/string caches", and everything in
  `fe` is a parser cache. The 128-bit entry point stays available and returns
  `ncc_uint128_t` for anything that wants a wide stable id. This does not touch
  type identity: `th_typehash` and `th_path_digest` call `XXH3_128bits`
  directly and return `uint64_t`, independent of this typedef, and `th_arm_tag`
  was already using `XXH3_64bits`.

  Measured, 5 invocations of a min-of-9 20000-instruction parse: 64-bit min
  245.4 ms / median 245.7, against 128-bit min 247.3 / median 248.3. About 1%,
  and it halves `ncc_dict_bucket_t` from 32 bytes to 16.

- **The dictionary has n00b's bucket layout and caches the hash.** Buckets are
  metadata only — `{ncc_hash_value_t hv; uint32_t state;}` — with keys and
  values in separate parallel arrays, which is `n00b_dict_bucket_t`'s shape
  (`~/n00b/include/adt/dict.h`) minus the parts that need the n00b runtime:
  no `_Atomic` flags word, no futex-gated migration protocol, no rocs layout
  guarantee, no GC scan kinds. `insert_order` is also left out — nothing in
  this tree reads dictionary iteration order, and an unmaintained field is
  worse than no field.

  `_ncc_dict_resize` now rehomes entries with the stored `hv` instead of
  calling `hash_fn` on every key at every growth, and the probe loop compares
  `hv` before `key_eq`. `_ncc_dict_add` used to run a full `_get` and then a
  full `_put`, hashing the same key twice; it now hashes once and shares the
  result through `_ncc_dict_put_hashed`. Four hand-rolled copies of the
  bucket-scan loop — in `annot_walk.c`, `grammar.c`, `symtab.c` and
  `tree_visit.c` — reached past the API into `buckets[i].key/.value` and are
  now `ncc_dict_foreach`.

  Measured, min of 9 runs, 20000-instruction parse, release build:

  | dictionary | min | µs/instr |
  | --- | --- | --- |
  | interleaved 24 B bucket, no `hv`, rehash on resize | 251.2 ms | 12.56 |
  | parallel arrays, no `hv` | 370.7 ms | 18.53 |
  | parallel arrays + 128-bit cached `hv`, 32 B bucket | 247.3 ms | 12.37 |
  | parallel arrays + 64-bit cached `hv`, 16 B bucket | 245.4 ms | 12.27 |

  Two things follow. The layout and the cached hash are a **package**: the
  point of moving keys out is that probing does not touch `keys[]`, and only
  the `hv` compare makes that possible — the layout without the cache is 49%
  *worse*, because every probe then reads two arrays and runs `key_eq`. And
  the pair is only break-even against the interleaved original here (251 →
  249): these dictionaries are small and pointer-keyed, so `key_eq` is a free
  pointer compare and the `hv` filter has nothing to save. It earns its keep on
  string keys, where `ncc_dict_cstr_eq` runs `strcmp`, and on resize.

- **Caching hashes inside objects is not worth it — measured, not assumed.**
  A census of one 20000-instruction parse, counting every entry point:

  | entry point | calls | share |
  | --- | --- | --- |
  | `ncc_hash_word` | 5,657,562 | 90.3% |
  | `ncc_hash_cstring` | 609,252 | 9.7% |
  | `ncc_string_hash` | 0 | — |
  | `ncc_buffer_hash` | 0 | — |

  90% of hashing is of an 8-byte *immediate* — a pointer or a small integer
  key — where there is no object to hold a cached hash. The C-string 10% is
  568,301 distinct `(pointer, length)` pairs against only 40,951 repeats, so a
  per-object cache would serve 6.7% of 9.7%, or 0.65% of all hashing.
  `ncc_string_t` also could not memoize on read even if it were worth it: it is
  a by-value struct, so a cache written during `ncc_string_hash(s)` dies with
  the parameter. It would have to hash eagerly at construction, paying for
  every string whether or not it is ever a key.

  The number worth remembering from that census is a different one: **313 hash
  calls per instruction parsed**, 6.27 million for a 560 KB input, 49 MB
  hashed. The cost is the *count* of dictionary operations, which is a property
  of the pwz memoization, not of the hash function.

- **Timings in this file are release-build numbers now.** The `build`
  directory is `buildtype=debug, optimization=0`, and an earlier round of
  measurement was taken against it, which overstated every cost by roughly 3x
  (a 20000-instruction parse reads as 934 ms at `-O0` and 261 ms at `-O2`).
  Use a `--buildtype=release` tree for any performance claim.

- **`ir_scale_test`'s parse guard no longer measures wall-clock against a
  fixed budget.** It asserted `< 1000 ms` for a 20000-instruction parse that
  costs ~934 in the `-O0` tree the benchmark actually runs in, so it went red
  whenever the machine was busy — including merely because the three
  benchmarks run concurrently. It now uses the ratio idiom the rest of that
  file already uses: parse at 10000 and 20000 and assert the time does not
  more than triple, which is what "not quadratic" actually means and does not
  depend on machine load.

### Next

Every item this plan called for is built. What is left is the "Open" list —
each needs a decision from the language designer, not more code.

### Open — needs a decision, not a guess

- **Grammar images** — see §10. Needs a measurement and a scope call, not code.
- **Representation pins: units.** `ir_layout_pin_size`/`_align`/`_stride` take
  `bytes`; `ir_layout_pin_member` takes `bit_offset`/`bit_width`. §12 wants
  member positions in bits for every member. `ncc_repr_pin_t` records the
  number the grammar stated without a unit tag, so nothing can pin it into a
  descriptor without inventing the unit.
- **Representation pins: target.** Descriptors are per-target; a production
  cannot know the target. Either these pins are target-agnostic assertions or
  there must be a way to scope one to a target.
- **Constraint attachment surface**: in `~/n00b` `constraints` is only ever set
  to `nullptr` — no adder, no caller. `th_var_constrain` taking the struct is a
  guess at what "easy to express high level type constraints" means.
- **`th_coerce`** implements identity, `DEREF`, `PROMOTE`. Upstream also has
  `OPTION_WRAP` and `CUSTOM`; semantics were not inferable, so they are absent
  rather than guessed.
- **`PROMOTES`** upstream runs on a Datalog engine and hardcodes n00b's numeric
  tower. Here it is a plain relation with BFS closure and no built-in
  promotions — the front end registers its own.
- **LR0 builder and left-corner sets** — *deleted*, 534 lines. They were
  leftovers from Earley optimization, and Earley is out of scope (§10). The
  whole family was write-only: `ncc_grammar_finalize` computed left-corner
  sets, LR0 items, states, gotos and predict states on every grammar, and
  nothing anywhere read any of it. Removed the functions, the fields
  (`left_corner_sets`, `lc_words_per_nt`, the eleven `lr0_*`, and the equally
  write-only `has_groups`), the `ncc_lr0_*` types, and the free paths.
  `first_set`, `nullable` and `start_nt` stay — those have real consumers.

  This also removed a cubic and two quadratics. `compute_left_corners` was an
  iterate-to-fixpoint transitive closure, O(iters × nts² × words). The goto
  construction rescanned every item once per symbol, O(Σ items²) — 4,172,902
  item-visits on `c_ncc.bnf`. `lr0_compute_predict_states` scanned all states
  per nonterminal, 393,145 `lr0_items_eq` calls on `c_ncc.bnf`, each a memcmp
  over up to 307 ints.

- **FIRST sets** — the same iterate-to-fixpoint shape, but `first_set` is
  consumed (`pwz.c` reads both the nonterminal and the rule sets), so it was
  fixed rather than deleted. The old loop swept every rule and every
  nonterminal until nothing changed: 21 sweeps on `c_ncc.bnf`, 28 on
  `n00b.bnf`, because visiting rules in index order moves information one
  level per sweep. Now `first_sets_by_scc` builds the nonterminal dependency
  graph (who appears in a leading, first-nullable-reachable position),
  condenses it with an iterative Tarjan, and settles each component in
  reverse topological order — successors are already final, so a component is
  visited once and only genuine cycles iterate, over the component alone.

  Verified equivalent, not just green: running the old sweep over the new
  result reports zero growth on every grammar in the suite, so the result is a
  fixpoint of the same update function; and since that function only ever
  merges derivable sets, it cannot have overshot. Same least fixpoint.

  `ncc_grammar_finalize`, best of 5, `-O0` library:

  | grammar | before | leftovers deleted | FIRST via SCC |
  |---|---|---|---|
  | `toy.bnf` | 0.03 ms | 0.008 ms | 0.008 ms |
  | `n00b.bnf` | 6.82 ms | 3.01 ms | 0.58 ms |
  | `c_ncc.bnf` | 24.73 ms | 14.39 ms | 3.17 ms |

  The "before" column already had the LR0 quadratic fixes in it, so the true
  starting point was higher; it was never measured.

- **`first_nullable`** — the same sweep-to-fixpoint, one layer earlier. Now
  `compute_first_nullable`, a worklist over the reverse dependency graph
  (`B → A` when a rule of `A` mentions `B`, the only dependency
  `match_is_first_nullable` has), built as a CSR by counting pass and prefix
  sum. Each nonterminal flips at most once and wakes only its waiters.
  Verified the same way: after it runs, the old sweep finds nothing to add on
  any grammar in the suite.

  `ncc_grammar_finalize` has no sweep-to-fixpoint left. Every remaining nested
  loop in `grammar.c` is outer × its own children, so linear in grammar size,
  except `rule_exists`, which is quadratic in one nonterminal's alternatives
  and bounded by grammar authoring rather than input.

---

## 12. Flow analysis: gap analysis, not a port

Upstream's `cfg.c` / `cdg.c` / `dfg.c` (1,565 lines) build a control-flow
graph, a control-dependence graph and a data-flow graph **over the parse
tree**. `src/` already has all three over the IR:

| upstream, tree-level | here, IR-level |
|---|---|
| `n00b_build_cfg` | blocks and terminators; `ir_doms_of`, `ir_rpo_index`, `ir_pred_at` |
| `n00b_build_cdg` | `ir_pdg_of` + `ir_controllers_of` / `ir_controller_at` |
| `n00b_build_dfg` | `src/ir_rd.c`, reaching definitions with bitsets |

**Do not port them.** Building a second set of graphs over the tree is the
duplication, and the IR ones are stronger: dominators, RPO, exact-vs-conservative
PDG, and bitset reaching-definitions.

Its six checks, against what exists:

| upstream | here |
|---|---|
| W001 dead code | `IR-W0030 DEAD_CODE`, `src/ir_analyze.c` |
| W002 use before def | `IR-W0027 UNSET_LOCAL`, from reaching definitions |
| W004 unreachable after jump | `ir_verify.c` `parsed.terminator` — an instruction after a terminator is structurally invalid, not a warning |
| W003 unused variable | `IR-W0032`, `src/ir_symcheck.c` |
| W005 shadowed variable | `IR-W0033`, `src/ir_symcheck.c` |
| E001 undefined variable | `IR-E0034`, `src/ir_symcheck.c` |

Those three were **symbol-table** properties, not flow properties. They needed
`ncc_sym_entry_t` (`shadowed`, `decl_node`, and a use count) and no graph at
all, so they landed as `ir_pass_symcheck` — an annotation-walk pass over the
symbol table. The use count is incremented by `@varref`. This is why porting
`cfg`/`cdg`/`dfg` would have bought nothing even for these.

All six checks are now accounted for and nothing from `analyze.c` is
outstanding.

---

## 13. Settled

### `$N` is 0-based over all children, everywhere

Terminals included. This was settled once; upstream's `@infer` using
`get_nth_nt_child` for plain `$N` is an **upstream defect**, not a second
convention to preserve. There is one indexing rule and the evaluator uses it.

### Trivia attachment — settled and built

**Default is attach-right**: trivia becomes the *leading* trivia of the token
that follows. A comment on its own line introduces what comes after it.
`ncc_scan_set_trivia_attach(s, NCC_TRIVIA_ATTACH_LEFT)` switches a scanner to
attach it to the token before instead — where it appends to that token's
`trailing_trivia` directly, not to a pending slot that would land it on the
*next* token.

**Trivia never attaches across a `@notrivia` boundary.** `@notrivia($N)` says
that child is adjacent, so `no_trivia` is set on its token and both trivia
lists are cleared. It used to print a warning to stderr and change nothing.

`ncc_scan_skip_line_comment` routes through `ncc_scan_add_trivia`, so the
direction is honoured in one place; `ir_scan`'s second, contradictory
recording of the same comment is gone.

### `@literal` — the language's literal syntax

`@literal(<kind>, $ref)` says "this production is my literal syntax for
`<kind>`". Its job is to **pass the data down** so the IR can load a literal:
the kind, and the text the literal was written with. The consumer is the
lowering, which emits an IR literal of that type.

### `@record` — bind a type name and set up layout

`@record($ref)` declares a structural record. It **binds a type name** and
**sets up the layout information**, i.e. it produces the `th` record type *and*
its `ir_descriptor_t` entry. Field order is a layout fact, so it belongs to the
descriptor, not the type (§4.2).

### Naming: two proposals had names that say nothing

`@ns` and `@mode` are rejected **as names**, not as ideas:

| was | is | means |
|---|---|---|
| `@ns` | `@namespace` | the namespace a declaration belongs to |
| `@mode` | `@direction` | a parameter's direction: `in`, `out`, `inout` |
| `@align` (layout) | `@alignment` | `@align` is the Wadler printing one |

The rest of `ANNOTATIONS.md` Part 2 — `@mutability`, `@constrain`, `@size` /
`@alignment` / `@offset` / `@width` — is built.
