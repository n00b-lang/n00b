# N00b Remaining Work Roadmap

## Completed

### Pipeline Integration, Graph Analysis, and End-to-End Testing

| Phase | What | Files | Status |
|-------|------|-------|--------|
| 1 | Unified diagnostic accumulator | `include/slay/diagnostic.h`, `src/slay/diagnostic.c` | In progress |
| 2 | Graph analysis module | `include/slay/analyze.h`, `src/slay/analyze.c` | In progress |
| 3 | n00b_dev pipeline enhancement | `src/tools/n00b_dev.c` | In progress |
| 4 | End-to-end tests | `test/unit/test_analyze.c` | In progress |

**Phase 1: Unified Diagnostic Accumulator**
- Single collection point for all stages (parse, annotation, typecheck, CFG, analysis)
- Severity levels: error, warning, note
- Source spans reusing `n00b_tc_span_t`
- Renderer with ANSI colors and source context
- Import bridge from `n00b_tc_error_t` into unified diagnostics

**Phase 2: Graph Analysis Module**
- W001: Dead code detection (CFG reachability via BFS from entry)
- W002: Use-before-def (DFG reaching definitions empty for a use)
- W003: Unused variables (DFG def with no reached uses)
- E001: Undefined variables (use with no symtab entry)
- W004: Unreachable-after-jump (statements after break/continue/return in same block)
- W005: Shadowed variables (symtab entries with non-NULL `shadowed` pointer)

**Phase 3: n00b_dev Pipeline Enhancement**
- New CLI flags: `--analyze`, `--typecheck`, `--all`, `--quiet`
- Diagnostic accumulation across all stages
- Continue-on-error: type errors don't prevent graph analysis
- Summary printing with exit code

**Phase 4: End-to-End Tests**
- `test_analyze.c` with helper functions for pipeline + diagnostic inspection
- Tests for: clean code, use-before-def, unused vars, dead code, shadowed vars, multiple diagnostics

### 5. N00b-Specific Checking and Transformations

Semantic rules specific to n00b that go beyond generic C-like analysis:

- **Confspec validation** — verify that `confspec` blocks conform to the expected schema, required fields present, types correct
- **Use resolution** — resolve `use` declarations against available modules/packages
- **Exhaustive match** — check that `match`/`case` statements cover all variants of an enum or tagged union
- **Enum checking** — verify enum values are valid, detect duplicate enum members, check exhaustive switch coverage
- **N00b-specific type rules** — any typing rules that differ from standard C semantics

### 6. MLIR Generation API

Walk annotated, type-checked parse trees to emit MLIR operations:

- Design the mapping from n00b AST nodes to MLIR operations
- Build MLIR C API bindings layer (MLIR already vendored in tree)
- Implement expression lowering (arithmetic, comparison, logical, string ops)
- Implement control flow lowering (if/else, loops, match, break/continue/return)
- Implement function definition and call lowering
- Implement variable declaration and assignment lowering

### 7. N00b-Specific MLIR Targeting

Runtime calls for n00b-specific operations:

- **GC allocation** — lower `new`/allocations to `n00b_alloc()` calls
- **String operations** — lower string concat, formatting, interpolation to runtime calls
- **Collections** — lower list/dict/set operations to runtime library calls
- **Conduit I/O** — lower conduit read/write/pub/sub to runtime calls
- **`!` operator** — lower the n00b bang operator to appropriate runtime semantics
- **Type metadata** — emit type descriptors for runtime type checking / GC tracing

### 8. Full Native MLIR Compilation

Complete the compilation pipeline from MLIR to native code:

- **MLIR pass pipeline** — optimization passes (CSE, DCE, loop opts, inlining)
- **LLVM dialect lowering** — convert from high-level MLIR dialects to LLVM dialect
- **Native codegen** — LLVM dialect → machine code via MLIR's JIT or AOT paths
- **Build system integration** — wire native compilation into `build.sh` / meson
- **Object file emission** — produce `.o` files compatible with system linker
- **Linking** — link against libn00b and system libraries

### 9. REPL

Interactive n00b REPL with modern developer experience:

- **JIT compilation** — use MLIR JIT to compile and execute n00b expressions interactively
- **Inline diagnostics** — show warnings/errors immediately as user types
- **Tab completion** — complete variable names, function names, types from symtab
- **Multi-line editing** — handle multi-line expressions and blocks
- **History** — persistent command history across sessions
- **Pretty printing** — formatted output for values, types, and data structures
