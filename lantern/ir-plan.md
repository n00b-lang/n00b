# IR Execution Plan

Covers `ir-spec.md` and nothing else. Back ends (spec §14), slay integration
(spec §15) and the open questions (spec §16) are **out of scope**; the end state
is an IR that slay can be moved onto without rework.

The conformance table in §6 is the point of this document. Every normative
statement in the spec has a row, an owning work package, and a named automated
test. A work package is not done when the code exists; it is done when its rows
are green.

---

## §1 End state

- `libir`: build, inspect, transform, parse, print, verify.
- Two profiles, exercised side by side, so language-neutrality is tested rather
  than asserted.
- The passes the spec names: `promote`, `structure`, `layout`, `split-edges`.
- slay's CFG, DFG, CDG and six diagnostics running on the IR instead of on parse
  trees.
- `ir` CLI: parse, print, verify, cfg, pass, explain.

Not built: C emitter, MIR emitter, slay's annotation-to-IR lowering. The seams
those need are built and tested; the consumers are not.

---

## §2 The rule against corner cutting

These are the gates. They exist because the failure mode is not "we forgot to
implement it", it is "we implemented the happy path, wrote a test that takes it,
and moved on."

1. **Every row names a test that exists and runs in CI.** "Verified by: review"
   is allowed only for the rows in §7, which are listed exhaustively and capped.
2. **Every prohibition gets a negative test.** A row whose requirement is a
   *must not* or a *never* is not green until a fixture violates it and the
   implementation rejects it with the specific diagnostic code. A passing
   happy-path test does not discharge a prohibition.
3. **Every diagnostic code has a fixture that produces it.** A code with no test
   is an unreachable code, and the build fails on one.
4. **Two profiles, from WP1.** A C-shaped profile and a deliberately different
   synthetic one. Any behavior that works for only one of them is hardcoding,
   and this is what catches it before slay does.
5. **The verifier runs between every pass in tests**, not only at the end. Spec
   §3 requires it to be runnable there; if it is not exercised there, it is not.
6. **Round trip is a property test over generated modules**, not a fixture
   count. Fixtures test what someone thought of.
7. **A pass may not be merged without its invalidation set.** A pass that edits
   and declares nothing is caught by the pass manager (PROP-03), and that check
   is itself tested.

---

## §3 Prerequisites in `~/th`

Spec §4.2. These block WP4 and WP6 and nothing earlier, so they run in parallel
with WP1 through WP3.

| ID | Work | Blocks | Status |
|---|---|---|---|
| TH-1 | Export a read API: accessors for every `th_kind`, so `th_type` can be walked from C. | TYPE-05 | **done** — `c/th_access.c`, 26 accessors |
| TH-2 | Member lookup by name on `Record`, returning the member type. | TYPE-06, OP-11 | **done** — `th_record_lookup`, `th_func_kw_lookup` |
| TH-3 | Confirm `th_hash` is stable across a build and usable as an interning key. | TYPE-02 | **done** — `c-vs-python` over the §13 vectors |
| TH-4 | Bring the C implementation to parity with the Python reference. | — | **done** — `c/th_infer.c`; `c-vs-python-inference`, 527 checks |

Parity work went beyond what the IR needs. `th_unify`, `th_instantiate` and
`th_generalize` now exist in C, but spec §4.2 still stands: **the IR does not
unify.** It checks types it is given, and TYPE-07 is a grep gate asserting no
unifier is reached from IR code. The inference API is there for front ends.

---

## §4 Work packages

Dependencies are hard unless noted. Sizes are relative, not calendar.

| WP | Name | Depends on | Delivers |
|---|---|---|---|
| WP1 | Core structures | — | Module, function, block, instruction, value, local, global; type table; profile registry; spans; origins; annotations. Two profiles. |
| WP2 | Diagnostics | WP1 | Severity, codes, labels, suggestions, policy, accumulation, both renderings. |
| WP3 | Opcode table + generator | WP1 | One table; generates the enum, operand-shape checks, parser dispatch, printer dispatch. |
| WP4 | Text format | WP2, WP3, TH-1 | Parser, canonical printer, round-trip property harness. |
| WP5 | Rule dispatch | WP3, WP2 | Ladder resolution, decline-and-fall-through, profile-supplied rules, extension opcodes, `ir explain`. |
| WP6 | Default typing rules | WP5, TH-1, TH-2 | Every §7 rule. Annotating results. |
| WP7 | Verifier | WP6, WP4 | Graded properties; all §12 rules; location monotonicity. |
| WP8 | Layout | WP7 | Descriptors, four states, pin API, `layout` pass, symbolic resolution. |
| WP9 | `promote` | WP7 | Dominators, phi placement, renaming discipline, override hook. |
| WP10 | `structure` | WP7 | Region overlay, relooper, dispatch-loop fallback and its record. |
| WP11 | Analyses | WP9 | CFG view, DFG, CDG, W001–W005, E001. |
| WP12 | CLI | WP7 | parse, print, verify, cfg, pass, explain. |

`split-edges` lands with WP9, since `promote` is where critical edges start to
matter.

**Ordering note.** WP2 before WP4 is deliberate: a parser that reports errors by
returning null has to be rewritten once diagnostics exist, and the rewrite is
where error paths get skipped.

---

## §5 Test strategy

| Kind | What it covers | Where it applies |
|---|---|---|
| **Property** | `parse(print(m)) == m`; `print` idempotent; locations never degrade; annotations survive unknown-namespace passes. Over generated modules, not fixtures. | WP4, WP7, WP9, WP10 |
| **Negative** | One fixture per prohibition and per diagnostic code, asserting the code, the primary span, and the secondary spans. | WP2 onward |
| **Golden** | Canonical printing; diagnostic rendering, both human and machine. | WP4, WP2 |
| **Cross-profile** | The same module under both profiles; and a profile-specific construct under the profile that lacks it. | WP1 onward |
| **Pass-pair** | Verify between every ordered pair of passes; assert declared invalidation matches observed. | WP7 onward |
| **Fuzz** | Parser must not crash or hang on arbitrary input; generated modules must survive print/parse. | WP4 |

A generator producing random well-formed modules is part of WP4, not an
afterthought — the property tests are worth what the generator covers, so its
coverage of the opcode table is itself measured.

---

## §6 Conformance table

Status: `[ ]` open, `[x]` green. Green requires the named test to exist and pass.

### 6.1 Forms and properties — spec §3

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| PROP-01 | §3 | A module carries a property set; passes declare requires/establishes/invalidates. | unit | WP1 |
| PROP-02 | §3 | The verifier is runnable between any two passes. | pass-pair | WP7 |
| PROP-03 | §3 | A pass that edits without declaring invalidation is caught. | negative | WP7 |
| PROP-04 | §3 | Nothing requires `ssa` for well-formedness; non-SSA is not degraded. | property | WP7 |
| PROP-05 | §3 | Each named pass declares exactly the requires/establishes the spec lists. | unit | WP8–WP10 |

### 6.2 Types — spec §4

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| TYPE-01 | §4.1 | A type in the IR is a `th_type *`. No parallel type model exists. | review + grep gate | WP1 |
| TYPE-02 | §4.1 | Type equality is `th_hash` equality, never structural or by name. | unit | WP1 |
| TYPE-03 | §4.1 | Type syntax is typelang; the IR adds no type syntax. | golden | WP4 |
| TYPE-04 | §4.2 | Type structure is never re-derived from printed text. | review + grep gate | WP6 |
| TYPE-05 | §4.2 | Terms are walked through the typehash read API. | unit | WP6 |
| TYPE-06 | §4.2 | Member resolution is a typehash call. | unit | WP6 |
| TYPE-07 | §4.2 | No unification is used anywhere in the IR. | review + grep gate | WP6 |
| TYPE-08 | §4.3 | The profile maps `Named` only; other kinds get machine class structurally, per the table. | unit, all 9 kinds | WP1 |
| TYPE-09 | §4.3 | `BitInt` takes width and signedness from the node, not the profile. | unit | WP1 |
| TYPE-10 | §4.3 | A `Named` with no profile entry is `opaque`: storable, passable, pointable, not operable. | unit + negative | WP6 |
| TYPE-11 | §4.3 | A module names its profile. | unit | WP1 |
| TYPE-12 | §4.4 | Generic types pass through; no monomorphization; no ground requirement. | property | WP6 |
| TYPE-13 | §4.4 | A non-ground operand is marked boxed via `ir.boxed`. | unit | WP6 |
| TYPE-14 | §4.4 | No `dynamic_*` opcode exists. | grep gate | WP3 |

### 6.3 Layout — spec §5

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| LAY-01 | §5.1 | Descriptors are keyed by `th_hash` and scoped to a target; one type may have several. | unit | WP8 |
| LAY-02 | §5.2 | Bit offset and bit width are recorded for every member, not only bitfields. | unit | WP8 |
| LAY-03 | §5.3 | Four states: unset, pinned, symbolic, resolved. | unit | WP8 |
| LAY-04 | §5.3 | The `layout` pass fills unset values around pins. | unit | WP8 |
| LAY-05 | §5.3 | An unsatisfiable pin fails with a diagnostic and never silently overrides. | **negative** | WP8 |
| LAY-06 | §5.3 | Symbolic values are target-independent and survive a target change. | unit | WP8 |
| LAY-07 | §5.3 | `laid-out` for a target means every reachable value is pinned or resolved for it. | unit | WP8 |
| LAY-08 | §5.3 | The four pin API entry points exist and are honored. | unit | WP8 |
| LAY-09 | §5.2 | Variant tag placement, niche use, and indirection representation are recorded. | unit | WP8 |
| LAY-10 | §5.1 | Layout is absent from the type. Changing a descriptor does not change `th_hash`. | unit | WP8 |

### 6.4 Module structure — spec §6.1, §6.2

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| MOD-01 | §6.1 | A module owns type table, profile, target, globals, functions, properties. | unit | WP1 |
| MOD-02 | §6.1 | A function owns type, linkage, params, locals, blocks with entry first, optional region overlay, properties. | unit | WP1 |
| MOD-03 | §6.1 | A block has exactly one terminator, and it is last. | negative | WP7 |
| MOD-04 | §6.1 | A block has no parameters. | grep gate | WP1 |
| MOD-05 | §6.1 | Every value has a type and is defined exactly once. | negative | WP7 |
| MOD-06 | §6.2 | A local has no address; taking one is impossible by construction. | negative | WP7 |
| MOD-07 | §6.2 | `promote` does not touch memory. | unit | WP9 |

### 6.5 Locations — spec §6.3

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| LOC-01 | §6.3.1 | A span carries byte offsets and line/column, and both describe the same range. | property | WP1 |
| LOC-02 | §6.3.1 | `file` indexes a module file table; `-1` is unknown and legal. | unit | WP1 |
| LOC-03 | §6.3.1 | Lines and columns are 1-based; columns count Unicode scalar values. | unit, non-ASCII fixture | WP1 |
| LOC-04 | §6.3.2 | `agent` is present for every origin kind but `SOURCE`. | negative | WP7 |
| LOC-05 | §6.3.2 | An origin chain is finite and acyclic. | negative | WP7 |
| LOC-06 | §6.3.2 | Rendering shows the innermost span as primary and each parent as a note. | golden | WP2 |
| LOC-07 | §6.3.3 | Instructions, blocks, locals, globals and functions carry spans; types do not. | grep gate | WP1 |
| LOC-08 | §6.3.3 | Locations are never compared for equality and never affect identity. | grep gate | WP1 |
| LOC-09 | §6.3.4 | Replacement inherits span and origin unchanged. | unit | WP9 |
| LOC-10 | §6.3.4 | A fold takes the covering span; across files, the primary operand's. | unit | WP9 |
| LOC-11 | §6.3.4 | An expansion gives every result the original's span; none unknown. | unit | WP9 |
| LOC-12 | §6.3.4 | Pass-created nodes take the nearest enclosing span with `GENERATED_BY`. | unit | WP9 |
| LOC-13 | §6.3.4 | A created block takes the span of the construct it implements. | unit | WP10 |
| LOC-14 | §6.3.4 | A `promote` phi takes the local's declaration span with `GENERATED_BY "promote"`. | unit | WP9 |
| LOC-15 | §6.3.4 | A moved instruction keeps its own span. | unit | WP9 |
| LOC-16 | §6.3.4 | Inlined instructions keep their span and gain `INLINED_FROM`. | unit | WP9 |
| LOC-17 | §6.3.4 | **Monotonicity**: no pass turns a known location into an unknown one. The verifier checks it. | **property, every pass** | WP7 |
| LOC-18 | §6.3.4 | Spans and origins survive the round trip. | property | WP4 |

### 6.6 Annotations — spec §6.4

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| ANN-01 | §6.4 | Every node kind carries an annotation map. | unit, all 6 kinds | WP1 |
| ANN-02 | §6.4 | Keys are namespaced; `ir.` is reserved to the spec. | negative | WP7 |
| ANN-03 | §6.4 | A pass preserves annotations in namespaces it does not understand. | **property, every pass** | WP7 |
| ANN-04 | §6.4 | Annotations round-trip. | property | WP4 |
| ANN-05 | §6.4.1 | Each annotation is authored or derived, with `derived_from` naming a property. | unit | WP1 |
| ANN-06 | §6.4.1 | Invalidating a property invalidates every annotation derived from it. | unit | WP7 |
| ANN-07 | §6.4.1 | An authored annotation is never invalidated or overwritten; a contradicting pass diagnoses instead. | **negative** | WP7 |
| ANN-08 | §6.4.2 | A typing rule may return annotations, and they are attached. | unit | WP6 |
| ANN-09 | §6.4.2 | `ir.rule` and `ir.boxed` are populated as specified. | unit | WP6 |

### 6.7 Rules and dispatch — spec §7.1, §7.2

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| RULE-01 | §7.1 | One opcode table generates the enum, parser, printer and operand checks. | build gate | WP3 |
| RULE-02 | §7.1 | No opcode exists without a typing rule. | build gate | WP3 |
| RULE-03 | §7.2 | Dispatch follows the five-rung ladder, most specific first. | unit, one per rung | WP5 |
| RULE-04 | §7.2 | A rule may decline; resolution continues down the ladder. | unit | WP5 |
| RULE-05 | §7.2 | Rules live in the profile. | cross-profile | WP5 |
| RULE-06 | §7.2 | A hook cannot change arity, operand kinds or spelling. | negative | WP5 |
| RULE-07 | §7.2 | Parse and print are identical with and without a profile loaded. | **property** | WP5 |
| RULE-08 | §7.2 | Extension opcodes are namespaced and cannot collide. | negative | WP5 |
| RULE-09 | §7.2 | An extension opcode without its profile reports a missing profile, not an unknown opcode. | **negative** | WP5 |
| RULE-10 | §7.2 | Which rule fires is queryable. | unit + CLI | WP5, WP12 |
| RULE-11 | §7.2 | Typing and lowering are separate hook points. | unit | WP5 |

### 6.8 Instructions — spec §7.3–§7.16

Every row is a typing rule. Each needs a positive case and, where the
requirement is a constraint, a rejection case.

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| OP-01 | §7.3 | `const`: the literal is representable in `T`. | unit + negative | WP6 |
| OP-02 | §7.3 | `aggregate`: operand types match `T`'s members in typehash order. | unit + negative | WP6 |
| OP-03 | §7.3 | `const` with an `opaque` `Named` is an error. | negative | WP6 |
| OP-04 | §7.3 | `undef` is legal to read. | unit | WP6 |
| OP-05 | §7.4 | `local` is a function-scoped declaration, not an instruction. | unit | WP1 |
| OP-06 | §7.4 | A local name as an operand is the read, and has the local's type. | unit | WP6 |
| OP-07 | §7.4 | A destination is a fresh value or a local, and its type equals the result type. | unit + negative | WP6 |
| OP-08 | §7.4 | `copy` is the only instruction whose job is moving a value. | unit | WP6 |
| OP-09 | §7.5 | `alloca`, `load`, `store` type as specified. | unit + negative | WP6 |
| OP-10 | §7.5 | `volatile` and `atomic` are access modifiers, not type properties. | unit + grep gate | WP6 |
| OP-11 | §7.5 | `field` resolves the member type through typehash; `T` must be a record or a `Named` with a record definition. | unit + negative | WP6 |
| OP-12 | §7.5 | `index` requires a `Sequence` or an `INDEXABLE` indirection. | unit + negative | WP6 |
| OP-13 | §7.5 | `offset` requires `INDEXABLE`. | negative | WP6 |
| OP-14 | §7.5 | `diff` yields a signed integer in units of `T`. | unit | WP6 |
| OP-15 | §7.5 | `memcpy`, `memmove`, `memset`, `memcmp` type as specified. | unit | WP6 |
| OP-16 | §7.6 | `extract` and `insert` type as specified; `insert` does not mutate. | unit | WP6 |
| OP-17 | §7.6 | Member access is by name through typehash, never by offset. | grep gate | WP6 |
| OP-18 | §7.7 | `variant.make` requires a `variant Named[...]`; the tag is `th_arm_tag`. | unit + negative | WP6 |
| OP-19 | §7.7 | `variant.tag` and `variant.payload` type as specified. | unit | WP6 |
| OP-20 | §7.8 | Arithmetic requires `class(T)` in {`sint`, `uint`, `float`}. | negative | WP6 |
| OP-21 | §7.8 | Signedness is not in any opcode mnemonic. | grep gate | WP3 |
| OP-22 | §7.8 | The overflow modifier is required on integer arithmetic; omission is rejected. | **negative** | WP6 |
| OP-23 | §7.8 | `wrap`, `trap`, `sat`, `undef` behave per the worked table, including the `u8` and `i8` vectors. | unit, all 4 × both vectors | WP6 |
| OP-24 | §7.8 | `div` and `rem` take a zero modifier; signed forms also take a fit modifier for `INT_MIN / -1`. | unit + negative | WP6 |
| OP-25 | §7.9 | Bitwise requires `class(T)` in {`sint`, `uint`}. | negative | WP6 |
| OP-26 | §7.9 | `shr` is arithmetic or logical from the type; there is one opcode. | unit, both | WP6 |
| OP-27 | §7.9 | A shift at or above the width is undefined unless `mask`, which takes it modulo the width. | unit | WP6 |
| OP-28 | §7.10 | Comparisons yield `bool`; signedness comes from `T`. | unit, both | WP6 |
| OP-29 | §7.10 | Float comparison takes `ordered` or `unordered`. | unit + negative | WP6 |
| OP-30 | §7.10 | No `min`/`max` opcode exists. | grep gate | WP3 |
| OP-31 | §7.11 | The cast kind is determined by `class(T)` and `class(U)` and is not spelled. | unit, all 8 rows | WP6 |
| OP-32 | §7.11 | Fit and rounding modifiers are required exactly per the conversion table. | **negative, all 8 rows** | WP6 |
| OP-33 | §7.11 | Widening sign vs zero extension comes from the source type, not a modifier. | unit, both | WP6 |
| OP-34 | §7.11 | `bitcast` operand and result sizes agree once `laid-out`. | negative | WP8 |
| OP-35 | §7.12 | `select` evaluates both arms and does not short-circuit. | unit | WP6 |
| OP-36 | §7.12 | No short-circuit opcode exists; an unevaluated operand is impossible. | grep gate | WP3 |
| OP-37 | §7.13 | One `call`; direct and indirect are distinguished by the callee's type. | unit, both | WP6 |
| OP-38 | §7.13 | Call arguments are checked against the callee's typehash `Function`: positional arity, rest element type, keyword names, parameter modes. | unit + negative, all 4 | WP6 |
| OP-39 | §7.13 | Keyword arguments are argument syntax, not a second opcode. | grep gate | WP3 |
| OP-40 | §7.14 | `sizeof`, `alignof`, `offsetof` are symbolic until `laid-out`. | unit | WP8 |
| OP-41 | §7.14 | `typehash` and `typename` are constants under any target. | unit, two targets | WP6 |
| OP-42 | §7.14 | `armtag` yields `th_arm_tag` of the name. | unit | WP6 |
| OP-43 | §7.15 | Every block ends in exactly one terminator, and terminators appear nowhere else. | negative | WP7 |
| OP-44 | §7.15 | `switch` requires an integer or `bool` scrutinee and distinct case constants. | negative, both | WP6 |
| OP-45 | §7.15 | `return` matches the function's return type, and is absent iff the return type is `void`. | negative, both | WP6 |
| OP-46 | §7.15 | No `goto`, `break` or `continue` opcode exists. | grep gate | WP3 |
| OP-47 | §7.16 | Every opcode in the deleted list is absent. | grep gate, one per row | WP3 |

### 6.9 SSA — spec §8

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| SSA-01 | §8.1 | Both forms are legal IR and both round-trip. | property | WP4 |
| SSA-02 | §8.1 | The instruction set is identical in both forms. | grep gate | WP3 |
| SSA-03 | §8.2 | `promote` requires `parsed` and `typed`; establishes `ssa`. | unit | WP9 |
| SSA-04 | §8.2 | Renaming is `%x_0`, `%x_1`, … in dominance order, derived from the source name. | golden | WP9 |
| SSA-05 | §8.2 | A colliding name gets a disambiguating suffix. | unit | WP9 |
| SSA-06 | §8.2 | Naming is overridable by a supplied function. | unit | WP9 |
| SSA-07 | §8.2 | Created blocks are named by role — `loop.header`, `if.join` — not numbered. | golden | WP9 |
| SSA-08 | §8.2 | Address-taken locals are not promoted. | unit | WP9 |
| SSA-09 | §8.3 | A `phi` appears only in the leading run of its block. | negative | WP7 |
| SSA-10 | §8.3 | A `phi` names every predecessor exactly once and no other block. | negative, both halves | WP7 |
| SSA-11 | §8.3 | Every incoming value has the `phi`'s type. | negative | WP7 |
| SSA-12 | §8.3 | An incoming value dominates the end of its named predecessor, not the phi. | negative | WP7 |
| SSA-13 | §8.3 | Joins are `phi`; block parameters do not exist. | grep gate | WP1 |

### 6.10 Regions — spec §9

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| REG-01 | §9 | The overlay is metadata and changes no instruction. | property | WP10 |
| REG-02 | §9 | `structure` requires `parsed`; establishes `structured`. | unit | WP10 |
| REG-03 | §9 | Reducible control flow yields the corresponding regions. | golden, if/while/for/switch | WP10 |
| REG-04 | §9 | Irreducible control flow still succeeds, via a dispatch loop. | unit, irreducible fixture | WP10 |
| REG-05 | §9 | The dispatch-loop fallback is recorded where it is used. | **unit** | WP10 |
| REG-06 | §9 | The overlay covers every block when `structured` holds. | property | WP10 |
| REG-07 | §9 | The round trip holds in nested and flat printing modes alike. | property, both modes | WP4 |

### 6.11 Textual format — spec §10

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| TXT-01 | §10.2 | Lexis as specified: `%`, `$`, `@`, bare block names, typelang types, `#` comments, NFC identifiers. | unit, one per element | WP4 |
| TXT-02 | §10.3 | Canonical form: one instruction per line, blocks in layout order with entry first, types via `th_print`. | golden | WP4 |
| TXT-03 | §10.3 | `parse(print(m)) == m`. | **property, generated** | WP4 |
| TXT-04 | §10.3 | `print` is idempotent. | **property, generated** | WP4 |
| TXT-05 | §10.3 | Comments survive the round trip. | property | WP4 |
| TXT-06 | §10.3 | Whitespace does not survive. | unit | WP4 |
| TXT-07 | §10.7 | Every grammar production is implemented as written. | unit, one per production | WP4 |
| TXT-08 | §10.7 | Mnemonics and modifiers validate against the opcode table, not a grammar enumeration. | build gate | WP4 |
| TXT-09 | §10.1 | Nothing in the IR's meaning depends on the surface spelling; a spelling change is a table edit. | **unit: rename an opcode in the table, rebuild, tests pass** | WP3 |

### 6.12 Diagnostics — spec §11

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| DIAG-01 | §11.1 | Five severities with the specified semantics. | unit | WP2 |
| DIAG-02 | §11.1 | `FATAL` stops the pipeline; `ERROR` continues so one run reports many. | unit, both | WP2 |
| DIAG-03 | §11.1 | `NOTE` and `HELP` never stand alone. | negative | WP2 |
| DIAG-04 | §11.1 | A primary label is required. | negative | WP2 |
| DIAG-05 | §11.1 | Several labelled secondary spans are supported and rendered. | golden | WP2 |
| DIAG-06 | §11.1 | Suggestions carry a span, replacement and description. | golden | WP2 |
| DIAG-07 | §11.1 | The origin chain is attached and rendered. | golden | WP2 |
| DIAG-08 | §11.2 | Every diagnostic carries a code. | **build gate** | WP2 |
| DIAG-09 | §11.2 | Codes are namespaced by `ir` or the profile name. | unit | WP2 |
| DIAG-10 | §11.2 | Every code has a fixture that produces it. | **build gate** | WP2 onward |
| DIAG-11 | §11.3 | A primary span is always present; fallback is instruction, then block, then function. | unit, all 3 | WP2 |
| DIAG-12 | §11.3 | An unknown primary span is itself reported as a bug. | negative | WP7 |
| DIAG-13 | §11.4 | Policy maps codes to `default`, `allow`, `warn`, `deny`, `forbid`. | unit, all 5 | WP2 |
| DIAG-14 | §11.4 | Policy applies by code and by prefix. | unit, both | WP2 |
| DIAG-15 | §11.4 | `FATAL` is not subject to policy. | negative | WP2 |
| DIAG-16 | §11.5 | Diagnostics accumulate rather than abort, except on `FATAL`. | unit | WP2 |
| DIAG-17 | §11.5 | Ordering is file, then start offset, then severity, and is deterministic. | **property** | WP2 |
| DIAG-18 | §11.5 | A human rendering with source context and carets exists. | golden | WP2 |
| DIAG-19 | §11.5 | A machine rendering carries every field, with byte-offset spans. | golden | WP2 |

### 6.13 Verification — spec §12

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| VER-01 | §12.1 | The verifier reports which properties hold, not a boolean. | unit | WP7 |
| VER-02 | §12.1 | Every diagnostic carries a location and names the rule violated. | golden | WP7 |
| VER-03 | §12.2 | All seven `parsed` rules, each with a violating fixture. | negative × 7 | WP7 |
| VER-04 | §12.3 | All eight `typed` rules, each with a violating fixture. | negative × 8 | WP7 |
| VER-05 | §12.4 | All three `ssa` rules, each with a violating fixture. | negative × 3 | WP7 |
| VER-06 | §12.5 | All four `laid-out` rules, each with a violating fixture. | negative × 4 | WP8 |
| VER-07 | §12.3 | An unresolved extension opcode reports a missing profile. | negative | WP7 |

### 6.14 Analyses — spec §13

| ID | Spec | Requirement | Verified by | WP |
|---|---|---|---|---|
| ANA-01 | §13 | The CFG is the block list plus terminators; there is no construction pass. | grep gate | WP11 |
| ANA-02 | §13 | Edge kinds are derived from the dominator tree and the region overlay. | unit, all 7 kinds | WP11 |
| ANA-03 | §13 | Def/use is keyed by value and local, not by name string. | unit | WP11 |
| ANA-04 | §13 | Reaching definitions produce data-dependence edges. | golden | WP11 |
| ANA-05 | §13 | Under `ssa`, def/use is immediate. | unit | WP11 |
| ANA-06 | §13 | The CDG is built over IR blocks. | golden | WP11 |
| ANA-07 | §13 | W001–W005 and E001 run on the IR and reproduce slay's verdicts. | differential vs slay | WP11 |
| ANA-08 | §13 | Every analysis diagnostic is attributable to a span. | property | WP11 |

---

## §7 Rows discharged by review, not by test

These are structural absences and negative-space properties. Each is checked by
a grep gate in CI where possible, and by review where not. This list is
**closed** — a row may not be moved into it without a note saying why it is not
testable.

| ID | Why it is not a runtime test | Gate |
|---|---|---|
| TYPE-01 | An absence: no second type model. | grep for a parallel type struct |
| TYPE-04 | An absence: no type structure derived from text. | grep for type parsing outside typehash |
| TYPE-07 | An absence: no unifier. | grep for unify entry points |
| TYPE-14, OP-21, OP-30, OP-36, OP-39, OP-46, OP-47 | Absences from the opcode table. | assert against the generated enum |
| LOC-07, LOC-08 | Absences: no span on types, no span comparison. | grep |
| MOD-04, SSA-13 | An absence: no block parameters. | grep |
| OP-10, OP-17 | Absences: volatility not in types, no offset-based member access. | grep |
| SSA-02 | An identity between two forms. | assert the opcode table is shared |
| ANA-01 | An absence: no CFG construction pass. | grep |

---

## §8 Definition of done

Per work package:

1. Every conformance row assigned to it is `[x]`.
2. Every row's named test exists, runs in CI, and fails when the behavior is
   reverted — mutation-checked at least once per row marked **bold** in §6.
3. Every diagnostic code the WP introduces has a fixture (DIAG-10).
4. The WP's passes declare requires/establishes/invalidates, and the pass-pair
   harness covers them.
5. Both profiles exercise the WP.

For the plan as a whole:

- Every row in §6 is green, or moved to §7 with a written reason.
- The round-trip and location-monotonicity properties hold over the generated
  corpus, and the generator's opcode coverage is reported.
- `ir verify` on every fixture reports exactly the properties the fixture
  claims.
- Renaming an opcode in the table and rebuilding changes only printed output
  (TXT-09) — the check that §2's "syntax is not load-bearing" claim is true
  rather than aspirational.

---

## §9 Out of scope

| | Where it goes |
|---|---|
| C emitter, MIR emitter | Spec §14. After this plan. |
| slay annotation-to-IR lowering, and the four annotation gaps | Spec §15. After this plan. |
| Atomics vocabulary, unwinding, debugger info, vector types, profile location, descriptor convergence with typehash | Spec §16, open. |
| Interpreter | Rides on the MIR back end, so it follows §14. |

What this plan does build for slay's benefit: the profile mechanism, extension
opcodes, annotation namespaces, diagnostic policy, and the analyses. Those are
the seams slay plugs into, and each is exercised here by a second profile so
that slay is a new profile rather than a fork.
