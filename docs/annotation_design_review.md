# Remaining Annotation Categories — Design Review

Status: design notes for next planning phase.
Covers: pretty-printing, data flow, type system, ADT, and TOKENIZER annotations.

## Current State

The annotation walk (`annot_walk.c`) handles 9 of 35 annotation kinds:

| Handled | Category |
|---------|----------|
| SCOPE_OPEN, DECLARES, TYPE_DECL | Symbol table (original) |
| BRANCH, LOOP, SWITCH, JUMP, CAPTURE, ASSIGNS | Control flow (new) |

The remaining 26 fall into five categories.

---

## 1. Pretty-Printing (11 kinds)

**Kinds:** INDENT, GROUP, SOFTLINE, HARDLINE, ALIGN, CONCAT, BLANKLINE, NEWLINE, SPACE, NOSPACE, PENALTY

**Slop pattern:** These are *not* processed by the symtab/annotation walk. The pretty-printer (`pprint.c`) does its own tree walk, reading annotations directly off the grammar's NT structs via `read_annotations()`. This is correct — pretty-print annotations are layout hints consumed at output time, not semantic labels that need to be resolved during a parse-tree walk.

**Data:** Mostly boolean flags (INDENT, GROUP, CONCAT, BLANKLINE) or a child ref indicating *where* to insert whitespace (SOFTLINE, HARDLINE, NEWLINE, SPACE, NOSPACE, ALIGN). PENALTY is rule-level (cost hint for ambiguity resolution), consumed during grammar construction.

**Recommendation:** Do NOT route these through the annotation walk. The pretty-printer should read them directly from the grammar, as slop did. This means:

1. Port `pprint.c`'s Wadler/Lindig formatter as a separate module.
2. It walks the parse tree, and for each NT node, reads the rule's annotation list (now per-rule thanks to Task 0).
3. No changes to `annot_walk.c` needed for this category.
4. PENALTY goes in the grammar/BNF layer — it's already a grammar-construction concern, not a walk concern.

**Priority:** Medium. Pretty-printing is useful but not blocking other analysis.

---

## 2. Type System (2 kinds)

**Kinds:** TYPE, INFER

**Slop pattern:**
- **TYPE** — processed by `slay_build_symtab()` in a second pass after DECLARES/ADT/FIELD create symbols. It stamps a `type_spec` string on the symbol.
- **INFER** — NOT processed by the symtab walk. The `infer_expr` string is parsed by a separate inference module (`infer.c` -> `infer_solve.c`) during a dedicated type-inference tree walk. The constraint DSL supports operations like `$0 unify $1`, `$self has_type int`, etc.

**Recommendation:**

- **TYPE** should go through the annotation walk alongside DECLARES/TYPE_DECL. When the walk encounters TYPE on a node that already has a symbol, it stamps the type. Add this to `annot_walk.c` as a straightforward case — it's just `sym->type_spec = a->type_ref_string`.

- **INFER** should NOT go through the annotation walk. It needs its own pass that runs after the symtab is built (because constraint refs like `$scope.X` need resolved symbols). Design a separate `n00b_infer_walk()` that:
  1. Takes a parse tree + symtab + grammar
  2. Finds nodes with INFER annotations
  3. Parses the `infer_expr` into constraint structs
  4. Feeds them to a solver

This is a substantial module. Defer until the object/type system is more mature.

**Priority:** TYPE is low-effort, add it soon. INFER is high-effort, defer.

---

## 3. ADT (8 kinds)

**Kinds:** ADT, FIELD, METHOD, INHERITS, IMPLEMENTS, VISIBILITY, STATIC, ABSTRACT

**Slop pattern:** All processed by `slay_build_symtab()` / `walk_node()`:
- **ADT** — creates a type symbol + opens a child scope (like SCOPE_OPEN but typed)
- **FIELD, METHOD** — create symbols in the ADT's scope with `is_field`/`is_method` flags
- **VISIBILITY, STATIC, ABSTRACT** — stamp flags on existing symbols (second pass)
- **INHERITS, IMPLEMENTS** — store parent/interface names on the scope, resolved in a post-walk pass

**Recommendation:** Extend the annotation walk to handle ADT annotations. They integrate naturally with the existing symtab infrastructure:

1. **ADT** -> `symtab_push(ctx, adt_kind_tag)` + register type symbol in parent scope
2. **FIELD, METHOD** -> register symbol in current scope with appropriate kind
3. **VISIBILITY, STATIC, ABSTRACT** -> flag-stamp on most recent symbol
4. **INHERITS, IMPLEMENTS** -> store names on scope for post-walk resolution

This is a natural extension of SCOPE_OPEN + DECLARES. The data structures (`n00b_sym_entry_t`, `n00b_symtab_t`) already have the fields needed (or can be extended with a few flags).

**Dependency:** Needs the object system / type representation to be more developed before INHERITS/IMPLEMENTS resolution makes sense. FIELD/METHOD/ADT can proceed independently.

**Priority:** Medium-high for ADT/FIELD/METHOD (unlocks struct/class analysis). Low for INHERITS/IMPLEMENTS (needs type system).

---

## 4. Data Flow (4 kinds)

**Kinds:** OPERATOR, LITERAL, CALL, VARREF

**Slop pattern:** These are NOT consumed by the symtab walk. They serve two purposes:

1. **Auto-inference** (`semantic_infer.c`) — walks the *grammar* (not parse tree) and heuristically attaches these annotations to NTs that lack them. E.g., `NT -> INTEGER` gets LITERAL("int"), `NT -> IDENTIFIER` gets VARREF($0).

2. **Codegen/semantic analysis** — intended to feed a code generation or semantic analysis pass. Slop's `analysis_typed.c` was a stub.

**Data:** `op_kind` string + child refs for name/args.

**Recommendation:** These are AST role labels, not control flow or scoping. They should NOT go through the annotation walk. Instead:

1. **Auto-inference** — port `semantic_infer.c` as a grammar utility. It runs once after grammar construction and annotates NTs. This is independent of the parse-tree walk.

2. **Consumption** — a future DFG (data flow graph) module can read these annotations from the grammar when walking a parse tree, similar to how the CFG builder reads CF labels. The DFG module would produce def-use chains.

3. **No changes to `annot_walk.c`** — these annotations are grammar-level metadata, not parse-tree-level labels.

**Priority:** Low. The auto-inference is a nice convenience but not blocking. DFG analysis depends on having a working CFG first (done) and a type system (not done).

---

## 5. TOKENIZER (1 kind)

**Slop pattern:** Intercepted by the BNF parser during grammar loading. Stored directly on `grammar_t::tokenizer_name`. Never attached to any NT.

**Recommendation:** Already handled correctly — `n00b_bnf_load()` should parse `@tokenizer("name")` and store it on the grammar. This is a grammar-construction concern. No walk needed.

**Priority:** Low. The current C grammar hardcodes its tokenizer via the test infrastructure.

---

## Suggested Implementation Order

| Phase | Items | Effort | Depends on |
|-------|-------|--------|------------|
| Next | TYPE annotation in walk | Small | Nothing |
| Next | ADT, FIELD, METHOD in walk | Medium | Symtab extensions |
| Later | Pretty-print module (own walk) | Medium | Nothing |
| Later | VISIBILITY, STATIC, ABSTRACT | Small | ADT phase |
| Later | Auto-inference (grammar utility) | Medium | Nothing |
| Deferred | INFER (type inference) | Large | Object/type system |
| Deferred | INHERITS, IMPLEMENTS | Medium | Type system |
| Deferred | DFG module | Large | CFG, type system |
| Deferred | TOKENIZER in BNF parser | Small | Nothing |

## Key Design Decisions

1. **Walk vs. own walk vs. grammar-level:** The annotation walk should only handle annotations that produce per-node semantic labels (symtab entries, CF labels). Pretty-printing and data flow annotations are consumed by their own modules reading directly from the grammar.

2. **No monolithic walk:** Slop's mistake was putting everything in one walk function. Keep the walk focused on symtab + CF labels. Add TYPE and ADT to it because they're symtab-adjacent. Everything else gets its own consumer.

3. **Flat structs:** Continue the pattern from `n00b_cf_label_t` — flat structs with unused fields zeroed, keyed by parse tree node pointer in an `n00b_dict_untyped_t`. Don't use unions or linked lists.

4. **Rule-level annotations work:** Task 0's migration to per-rule annotation lists is paying off. The pretty-printer and any future consumer can read annotations from the specific rule that matched, not all rules for the NT.
