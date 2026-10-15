# n00b FFI — Work Checklist

The work in `n00b-integration.md`, phases W0–W1 and N1–N8, as items small
enough to check off one at a time. The specs are `ffi-spec.md`,
`dsl.md` §3.2 and §9.3, `implementation.md` §4 and Slice 3, and
`n00b-integration.md`.

- **Phase 1:** P, W0, W1, N1–N6, C1, B1, and Phase 1 close.
- **Phase 2:** N7 and N8, dynamic loading. Not started until the user
  starts it.

## Rules

- Items are only checked off (`[ ]` → `[x]`). Adding, removing, rewording,
  splitting or reordering an item needs the user's approval first.
- Stop and ask the user when:
  - an item can't be done as written;
  - a spec is wrong, contradicts another, or leaves a choice open;
  - n00b needs a language decision the specs don't make.
- n00bb bugs found along the way, including a red baseline, are fixed and
  committed separately, each with a regression test.
- A phase is done only when every gate item in it is checked.
- Standing rules: no comments in code, generated code, grammar, build
  files or scripts; jj only; parsers come from lantern grammars; one
  implementation of each piece of infrastructure; in-place edits through
  the Edit tool; docs state the present only.
- n00bb work happens in its main workspace, `~/n00bb`. No other agent is
  changing n00bb.
- The podman VM's unused images and build cache may be pruned, and its
  disk grown with `podman machine set --disk-size`.

---

## P. Preparation

- [x] Baseline `./build.sh test` in `~/n00bb` is green; record the base commit
- [x] Baseline `./build.sh test` in `~/writ` is green
- [x] Baseline sanitizer build of writ is green
- [x] Baseline Linux container run of writ is green
- [x] Re-read `abi/n00b-abi-v1.h` module, scope, symbol and native-binding rows on the new base; stop if they differ from the checklist's assumptions
- [x] n00bb `tools/linux.Dockerfile`: LLVM, lantern, writ and n00bb built inside, sources mounted from the host
- [x] n00bb `build.sh linux test` runs the suite in that image, with the engine chosen as writ's `build.sh` does (`podman`, else `docker`)
- [ ] Baseline n00bb Linux run green on the new base, before any FFI work
- [x] Committed in n00bb: "Linux builds in a container"

---

## W0. Embedded units, `use <…>`, file list (writ)

### Grammar

- [x] `grammar/writ.bnf`: `<use-directive>` takes `%STRING` or `<header-angle>`
- [x] `grammar/writ.bnf`: `<embedded> ::= <item>* <embedded-binding>?`
- [x] `grammar/writ.bnf`: `<embedded-binding> ::= "binding" "export"? "{" <binding-item>* "}"`
- [x] Keyword corpus test still passes (`keywords-are-identifiers`)

### Parsing

- [x] `include/writ/writ.h`: `WRIT_SOURCE_EMBEDDED`
- [x] `src/core/syntax/parse.c`: `grammar_specs[WRIT_GRAMMAR_WRIT]` gains the `embedded` start symbol
- [x] `parse_source` picks the `embedded` start for `WRIT_SOURCE_EMBEDDED`
- [x] `writ_unit_add_embedded(unit, language, origin, text, source_id)` in `parse.c`, declared in `writ.h`
- [x] The unit records the embedded language
- [x] Embedded spans are in host coordinates (origin line, column and byte)
- [x] The embedded binding parses to `WRIT_N_BINDING` with no name and no language node
- [x] `src/core/syntax/fmt.c` formats an embedded unit and round-trips it
- [x] `fmt_roundtrip.py` covers an embedded unit

### `use`

- [x] `gather_use` in `src/core/model/resolve.c`: angle form searches the contract path
- [x] Session stores contract directories in insertion order
- [x] `writ_session_add_contract_dir` in `src/core/session.c`, declared in `writ.h`
- [x] `WRIT_CONTRACT_PATH` read once per session, split on `:`
- [x] Installed contract directory from a meson-configured path
- [x] Search order: host dirs, then `WRIT_CONTRACT_PATH`, then installed
- [x] Quoted `use` from an embedded source resolves against `origin.host_path`
- [x] Missing file is E0003 at the `use`, for both forms
- [x] Diagnostics in a used file report in that file
- [x] `meson.build` installs `contracts/` to the installed contract directory
- [x] `contracts/zlib.writ` holds the zlib contract; `tests/zlib/zlib.writ` uses it instead of a copy

### File list

- [x] Unit records every file it reads, in read order, once each
- [x] `writ_unit_file_count` and `writ_unit_file_at` in `ast.c`, declared in `writ.h`
- [x] Files read through `use` inside `use` are listed

### Binding an embedded unit

- [x] `find_block` in `src/gen/binding.c`: an embedded binding's contract is the unit's `library`, its language the unit's language
- [x] An embedded unit with no binding part binds as an import with defaults
- [x] `binding export { }` sets the export side
- [x] `module` in an embedded binding is E0514
- [x] Zero or several `library` directives in an embedded unit is E0514
- [x] Two binding parts is E0510
- [x] `defs/diag_codes.def`: `EMBEDDED_SHAPE` as E0514
- [x] `writ_bind` takes the binding from the embedded unit when `request->binding` is that unit

### CLI and tests

- [x] `writ bind --embedded LANGUAGE FILE` reads `FILE` as an embedded unit
- [x] `writ --contract-dir DIR` for every command that resolves
- [x] `tests/embedded/zlib_path.writ`: quoted `use`, golden binding
- [x] `tests/embedded/zlib_search.writ`: angle `use` through `--contract-dir`, golden binding
- [x] `tests/embedded/zlib_env.writ`: angle `use` through `WRIT_CONTRACT_PATH`
- [x] `tests/embedded/defaults.writ`: no binding part, golden binding
- [x] `tests/embedded/export.writ`: `binding export` over `kv`, golden binding
- [x] `tests/embedded/refine.writ`: `use` plus `refine`, golden model
- [x] Golden tests for each E0510 and E0514 case
- [x] Golden test for E0003 on both `use` forms
- [x] `tests/host/embedded.c`: `writ_unit_add_embedded` with a host origin; spans in host coordinates; `writ_unit_file_at` lists the used files
- [x] `tests/spec_examples.py` parses `writ-embedded` blocks as embedded units
- [x] `dsl.md` §9.3 and `ffi-spec.md` embedded examples tagged `writ-embedded`
- [x] Spec-example goldens regenerated and reviewed
- [x] libFuzzer target covers `writ_unit_add_embedded`

### Gate

- [x] `./build.sh test` green
- [x] Sanitizer build green
- [x] Linux container green
- [x] Committed: "Embedded units, use <...>, unit file list"

---

## W1. n00b generator and shims (writ)

### Profile and operators

- [x] `profiles/n00b.writ` matches `ffi-spec.md` §3
- [x] `src/core/model/expr.c`: the n00b row of the operator table

### `writ_gen_n00b`

- [x] `include/writ/gen.h`: `writ_gen_n00b_request_t` (binding, module name, shim prefix)
- [x] `include/writ/gen.h`: `writ_gen_n00b` output: n00b source, C shim source, link requirements
- [x] Link requirements as a public struct: libraries, frameworks, search paths, load mode
- [x] `src/gen/n00b.c`: module skeleton from a `writ_binding_t`
- [x] Functions: `func` declarations from plans, names from binding decisions
- [x] Extern declarations returned as data with the generated source
- [x] Scalars and `c_*` types per platform
- [x] `layout c` records as `type X layout(c)`
- [x] Records `layout(c)` can't express: opaque storage with accessors
- [x] Enums and flags
- [x] Constants
- [x] Results: direct, or `F_Result` records (`ffi-spec.md` §3.2)
- [x] Consumed counts and lengths dropped from results
- [x] `fail: Error` from `error` and `failure` outcomes
- [x] Generated `type Error` with `code`, `text`, `fn`
- [x] `error PATTERN -> "Type"` calls `Type.from_ffi`
- [x] Several success outcomes: outcome enum as first result
- [x] Handles: wrapper types with `close()`
- [x] `method` and `static` grouping; Rust impl grouping by default
- [x] `string`, `buffer`, `borrow` representations per slot
- [x] Rest packs as `List[T]` or `Array[T]`
- [ ] Keyword packs as keyword arguments or `F_Options`
- [ ] Policy parameters: fixed arm, or an enum parameter
- [x] Reserved words get a trailing `_`
- [x] `raw` declarations with `ptr` slots and a `_raw` suffix
- [x] Each generated declaration carries a span to its writ declaration

### Shims

- [x] `src/gen/c.c`: pointer-normalized shim mode on `writ_gen_c_request_t`
- [x] Shims take only scalars and pointers
- [x] By-value aggregates passed and returned through pointers
- [x] Shims convert strings and buffers per binding decision
- [x] Shims call `n00b_ffi_borrow` and `n00b_ffi_release` around borrowed spans
- [x] Shim symbol names from the shim prefix
- [x] Callback trampolines calling `n00b_ffi_dispatch` with a writ-laid-out argument struct
- [x] Export entry points calling n00b symbols, with `on_panic`
- [x] Export header generation

### CLI and tests

- [x] `writ gen n00b --embedded FILE -o DIR`
- [x] Golden n00b and C output: zlib
- [x] Golden n00b and C output: resharp, linked statically
- [x] Golden n00b and C output: kv import
- [x] Golden n00b and C output: kv export
- [x] Generated C shims compile with clang to bitcode for the host triple
- [x] Generated C shims compile for every platform in the zlib contract (`-fsyntax-only` per triple)

### Gate

- [x] `./build.sh test` green
- [x] Sanitizer build green
- [ ] Linux container green
- [ ] Committed: "writ_gen_n00b and pointer-normalized shims"

---

## N1. libwrit in n00bb

- [x] writ produces uninstalled pkg-config files `n00bb` can find
- [x] n00bb `build.sh` adds writ's `meson-uninstalled` directory to `PKG_CONFIG_PATH`
- [x] n00bb `build.sh` builds writ when it is stale
- [x] n00bb `meson.build`: `dependency('writ')` on `n00bb_exe`
- [x] Coexistence test: one process parses an `'ffi` literal through libwrit and an n00b file through lantern
- [x] Gate: n00bb `./build.sh test` green
- [x] Committed in n00bb: "Link libwrit"

---

## N2. The `'ffi` literal

### The declaration

- [x] `lib/builtin/ffi.n`: `litmod<fence> ffi(source: string)`
- [x] Implicit import of `builtin.ffi`
- [x] Two declarations of one literal modifier for one literal kind anywhere in the program are an error, builtins included
- [x] Test: a local `litmod<fence> ffi` is rejected

### Evaluation

- [x] Preprocessor recognizes an `'ffi` literal as a module-level const initializer and replaces the const with an import of the generated module
- [x] An `'ffi` literal elsewhere is a compile error at the literal
- [x] One writ session per compilation, created on first use
- [x] Contract directories added before the first literal: each `--contract-dir`, then the package's `contracts/`
- [x] Literal content bytes from `content_start_byte` to `content_end_byte`
- [x] Content line and column computed by scanning the source from `start_byte`
- [x] `writ_unit_add_embedded` with language `n00b` and the n00b file as origin
- [x] `writ_resolve` for the target triple
- [x] `writ_bind` with the n00b profile
- [x] libwrit diagnostics forwarded through `core_diagnostics_report_at` with the writ code and mapped location
- [x] Diagnostics in a used contract report in that file
- [x] `n00bb_lower_user_literal` never lowers an `'ffi` literal

### Dependencies and flags

- [x] `compiler_cli.c`: `--contract-dir DIR`, repeatable
- [x] Editing a used contract changes the next build

### Tests

- [x] `tests/programs/ffi/` directory and `tests/ffi.sh`, registered in `meson.build`
- [x] Malformed literal: error at the right n00b line and column
- [x] Error in a used contract: reported in the contract file
- [x] Binding error (E0501): reported at the binding item in the literal
- [x] `'ffi` literal outside a module-level const: rejected
- [x] Edit a used contract between builds: the module rebuilds
- [x] Built binary contains no literal text and no contract text

### Gate

- [x] n00bb `./build.sh test` green
- [x] Committed: "'ffi module literals"

---

## N3. zlib end to end

### Generated modules

- [x] Generated n00b source parsed as its own module unit (the `n00bb_format_parse` sequence on a synthetic source)
- [x] Synthetic module name unique per literal and stable across builds
- [x] `resolve_one_package_export` (`compiler_link_packages.c`) treats an `'ffi` const as a module export
- [x] `export zlib as x` on an `'ffi` const is N00BB-M0007
- [x] `compiler_cli.c`: `--show-ffi MODULE.NAME` prints the generated source
- [x] Go-to-definition data maps bound names to the generated source

### Externs

- [x] Generated externs registered as native functions in the generated module
- [x] Extern declaration in user source is a compile error
- [x] Extern declarations lower through `ir_func_add(…, IR_LINK_EXTERN)` with the symbol annotation
- [x] `llvm_backend.c` types extern functions as their scalar-and-pointer signatures

### Shims and linking

- [x] Shim C written to the build's scratch directory
- [x] clang compiles shims to bitcode for the target triple
- [x] Shim bitcode cached by content hash in the native cache
- [x] With `--lto`: shim bitcode linked through `n00bb_llvm_backend_link_bitcode` and left inlinable
- [x] Without `--lto`: shims compiled to an object and passed as a link object
- [x] `n00bb_build_executable` (`compiler_abi_driver.c`): `-l`, `-framework` and `-L` from link requirements
- [x] Link requirements collected across every `'ffi` module in the program, deduplicated

### Values

- [x] `Array[u8]` in: copied to a C buffer by the shim
- [x] `out` buffers: allocated by the shim at capacity, returned as `Array[u8]` at length
- [x] Status errors: `fail: zlib.Error` with `code` and `fn`

### Tests

- [x] `tests/programs/ffi/zlib/__init.n`: `const zlib = [- use <zlib.writ> … -]'ffi` and `package zlib { export zlib }`
- [x] Consumer: `import zlib`; `compress2` and `uncompress` round-trip data
- [x] Subpackage form: `archive.zlib.compress`
- [x] `--show-ffi` output golden
- [x] Reflection: the `'ffi` module has a module row, a `module` scope, and symbol rows for every bound name
- [x] Reflection: extern functions appear in the native-binding table with their C symbols
- [x] Passes with and without `--lto`
- [x] Mach-O run green
- [ ] Linux run green

### Gate

- [x] n00bb `./build.sh test` green
- [ ] Committed: "zlib through 'ffi"

---

## N4. Strings, buffers, handles, errors: resharp

### Runtime

- [ ] `runtime/runtime_ffi.c` added to `runtime_abi_sources`
- [ ] `n00b_ffi_borrow(object, length, access)` and `n00b_ffi_release(object)`
- [ ] Borrow roots through `n00b_gc_root_push` / `n00b_gc_root_pop`
- [ ] Native registration for the borrow and release functions

### Values

- [ ] `cstr` in: NUL-terminated copy
- [ ] `utf8[n]` in: counted copy
- [ ] `string` on a byte slot: shim passes the string's UTF-8 bytes
- [ ] Bytes out as `string`: UTF-8 decode; invalid text fails with `Error`
- [ ] `borrow` on a byte slot: the array's storage for the span
- [ ] `keep until EVENT` borrows held in the handle wrapper
- [ ] `with x` results keep `x`'s wrapper alive
- [ ] Handles: `close()` calls the release function
- [ ] Debug builds report handles collected while open, then release them
- [ ] `consume` marks the wrapper closed; reuse fails with `Error`
- [ ] Text error channels fill `Error.text`
- [ ] Keyword arguments for keyword packs
- [ ] `F_Options` records when the binding chooses `record`

### Rust transport

- [ ] resharp's transport crate generated by `writ gen rust` and built as a staticlib
- [ ] The transport staticlib reaches the link through link requirements

### Tests

- [ ] resharp program compiles patterns
- [ ] `is_match` with a `string` argument
- [ ] `find_all` returns spans
- [ ] An invalid pattern fails with the text from `resharp_last_error`
- [ ] Options through keyword arguments
- [ ] Handle closed explicitly; leaked handle reported in a debug build
- [ ] Mach-O run green
- [ ] Linux run green

### Gate

- [ ] n00bb `./build.sh test` green
- [ ] Committed: "resharp through 'ffi"

---

## N5. Callbacks

- [ ] Runtime handle table: register, look up, free
- [ ] `n00b_ffi_dispatch(handle, arguments, result)`
- [ ] Dispatch unpacks arguments with the writ layout and calls the callback value
- [ ] Callback with `userdata`: handle registered, index passed, freed at the span's event
- [ ] Callback without `userdata`: trampoline calls the `provide`d function's symbol
- [ ] Unnamed callback without `userdata` is E0509
- [ ] Test C library with a register/unregister callback API in `tests/programs/ffi/callbacks/`
- [ ] Test: `qsort` with an n00b comparator
- [ ] Test: registered callback kept until unregistered, then freed
- [ ] Mach-O run green
- [ ] Linux run green
- [ ] Gate: n00bb `./build.sh test` green
- [ ] Committed: "Callbacks through 'ffi"

---

## N6. Export

- [ ] `binding export` literal produces entry shims
- [ ] Entry shims attach the calling thread to the runtime when needed
- [ ] Entry shims root GC values until return
- [ ] Contract `thread` constraints checked in debug builds
- [ ] `on_panic abort` and `on_panic error EXPR`
- [ ] `own` results allocated from the named allocator
- [ ] Symbols from contract names and `symbol` clauses, through the `c_alias` path in `runtime_native_roots.c`
- [ ] Exported functions kept reachable (`--native-root` behaviour)
- [ ] Generated header written next to the build output
- [ ] Signature mismatch reported at the `provide` item
- [ ] Test: C program includes the header and calls an n00b implementation of `kv`
- [ ] Mach-O run green
- [ ] Linux run green
- [ ] Gate: n00bb `./build.sh test` green
- [ ] Committed: "Export through 'ffi"

---

## C1. FFI calls at comptime

- [ ] Comptime helper link (`compiler_phases.c`) gains the shim objects of every `'ffi` module the comptime code reaches
- [ ] Comptime helper link gains those modules' link requirements
- [ ] Comptime helper argument array sized for the extra objects and flags
- [ ] Test: a `comptime` block calls zlib `compress2` and the result is baked into the program
- [ ] Test: a user litmod whose function calls an `'ffi` module runs at comptime
- [ ] Test: the final binary links zlib only when run-time code uses it
- [ ] Gate: n00bb `./build.sh test` green
- [ ] Committed: "'ffi calls at comptime"

---

## B1. Baking C objects built at comptime

A C object graph returned to comptime code is copied into the program's
data and used at run time without rebuilding it. The test target is a
small C library written for this, `libbake`, whose graph has every shape
lantern's grammar has.

### Prerequisites

- [ ] `trait Fixup { func fixup(modify self: `T) -> nil fail: string }` in n00bb's builtin library (`n00b-integration.md` §4.11)

### Test library

- [ ] `tests/programs/ffi/bake/bake.c` and `bake.h`, built as a static library by the test harness
- [ ] Graph root struct with a string, a counted array `{data, len, cap}` of records, and a zero-terminated array
- [ ] A tagged union selected by a `kind` field
- [ ] Two records sharing one string, and a pointer into the middle of an array
- [ ] A function pointer field chosen from a named set
- [ ] A pointer-keyed hash table whose keys cache their hash
- [ ] A mutable cache field reset at load
- [ ] A file descriptor field opened at build time and reopened by its fixup
- [ ] `bake_build(spec)` allocates the graph with `calloc`; `bake_query(root, key)` reads it
- [ ] `bake_free(root)` for graphs built at run time
- [ ] `tests/programs/ffi/bake/bake.writ`: every reachable struct described, with extents, the union's discriminant and ownership

### writ

- [ ] `writ_gen_n00b` emits n00b record types for contract structs reachable from a bound result, not opaque handles
- [ ] Counted pointer fields carry their count field in the generated type
- [ ] Zero-terminated pointer fields carry their sentinel in the generated type
- [ ] Unions carry their discriminant field and arm values
- [ ] Function pointer fields are typed as foreign function pointers
- [ ] Golden n00b output for `bake.writ`

### Descriptors

- [ ] A foreign pointer type kind (C memory, not GC memory) in `abi/n00b-abi-v1.h`
- [ ] Trace child form: element count read from a sibling field
- [ ] Trace child form: element count up to a zero sentinel
- [ ] Union arms selected through the existing trace-arm and tag-extractor rows
- [ ] Foreign function pointer fields marked in the descriptor
- [ ] Layout and trace rows emitted for generated C record types (`abi_type_layout_rows.c`)
- [ ] `tests/abi_*` unit tests for each new descriptor form

### Comptime copier

- [ ] `scan_value` (`comptime_graph.c`) follows foreign pointers, sizing each block from its type
- [ ] Counted arrays sized from the count field
- [ ] Zero-terminated arrays sized up to and including the sentinel
- [ ] Unions followed through their selected arm
- [ ] Shared targets copied once
- [ ] A pointer into an already-copied block becomes an edge to that block at an offset
- [ ] A pointer into a block found later is reconciled when the containing block is found
- [ ] Function pointer fields recorded as symbolic references, not addresses
- [ ] An undescribed pointer fails the compile at the const, naming the field path
- [ ] `tests/comptime_graph.c` unit test over each shape

### Placement

- [ ] A `const` whose value is a foreign graph is emitted once, in a writable section, and used in place, not cloned per evaluation
- [ ] Edges emitted as relocations, as for existing graph nodes
- [ ] Function pointer references emitted as relocations to the named symbols
- [ ] Baked foreign objects are never released: generated `close()` and release calls on a baked object fail with `Error`

### Fixups

- [ ] A baked object whose type implements `Fixup` runs `fixup` once, before its first use
- [ ] A failed fixup fails that use with the fixup's error
- [ ] A failed fixup leaves the object unfixed, so a later use retries it
- [ ] Fixups reset mutable state (the test library's cache field)
- [ ] Fixups re-establish OS resources (the test library's file descriptor)
- [ ] Hash tables keyed by pointer stay valid through cached hashes; the key type implements `Hash` by returning the cached value

### Tests

- [ ] `const graph = bake.build(spec)` at comptime; run-time `bake_query` answers match a graph built at run time
- [ ] Every shape checked at run time: string, counted array, zero-terminated array, union arm, shared string, interior pointer, function pointer, hash lookup, reset cache
- [ ] The baked graph is not rebuilt at run time (`bake_build` never called in the final binary)
- [ ] Releasing a baked object fails with `Error`
- [ ] A baked file descriptor whose file still exists is reopened and readable
- [ ] A baked file descriptor whose file was deleted after the build fails its first use with the fixup's error
- [ ] Recreating that file lets a later use succeed
- [ ] An undescribed pointer is a compile error naming the field
- [ ] Mach-O run green
- [ ] Linux run green
- [ ] Gate: n00bb `./build.sh test` green
- [ ] Committed: "Bake C objects built at comptime"

---

## IC. Incremental build cache (n00bb)

### Separate compilation per SCC

- [ ] An SCC's native code emits every function it defines, not only the program's reachable set
- [ ] Type, function, global and dispatch indices in code are read from a per-SCC index table in data
- [ ] Every index table entry has an index-patch row (section, offset, table kind, SCC-local index)
- [ ] Instantiations an SCC needs are recorded as specialization requests instead of cloned into its module
- [ ] Test: a builtin SCC compiles to an identical object in two different programs

### SCC objects

- [ ] One object per SCC holds native code, ABI metadata, the semantic interface, runtime and comptime IR, and patch rows
- [ ] Key: SCC freshness identity, imported SCC interface identities, imported freshness identities for inlined callees, compiler, flags and target
- [ ] Objects live in the root's `.n00b-cache/obj/` and persist across builds

### Build from cached SCCs

- [ ] A cached SCC skips lexing, parsing, preprocessing, annotation, link stages, lowering and codegen
- [ ] Cached SCCs feed the link stages through their interfaces
- [ ] Inlining, specialization and comptime execution read callee bodies from cached IR

### Incremental link

- [ ] The ABI link rewrites each patched index from SCC-local to global
- [ ] Specialization requests across all SCCs are collected; missing instantiations are generated from the generic's cached IR
- [ ] Generated instantiations are cached by generic SCC identity and type arguments, with their own patch rows

### Tests

- [ ] An unchanged rebuild compiles only the root SCC
- [ ] A source edit rebuilds exactly that SCC and the SCCs whose imported interface changed
- [ ] An inline-body change rebuilds the SCCs that inline it
- [ ] A new instantiation generates one specialization object and reuses cached ones
- [ ] A cycle of modules is cached and invalidated as one SCC
- [ ] Gate: n00bb `./build.sh test` green, Mach-O and Linux
- [ ] Committed: "Incremental build cache"

---

## Phase 1 close

- [ ] `load dynamic` in a binding is rejected with a clear "not yet supported" error, in writ and in n00bb
- [ ] `n00b-integration.md` and `implementation.md` Slice 3 describe what was built
- [ ] writ full suite, sanitizer build and Linux container green on the final state
- [ ] n00bb full suite green on the final state
- [ ] Stop: report Phase 1 done to the user

---

# Phase 2: dynamic loading

Not started until the user starts it.

## N7. Late loading

- [ ] writ: `writ_gen_n00b` emits `open(path)` and symbol slots for `load dynamic`
- [ ] Remove the Phase 1 "not yet supported" rejection of `load dynamic`
- [ ] `load dynamic`: symbols through `dlopen` / `dlsym`
- [ ] Generated `open(path)`; with no path, search paths then the platform default
- [ ] `open` reports missing symbols
- [ ] `weak`, `since` and feature-gated symbols fail only when called
- [ ] `library … when` per-platform names
- [ ] Test: resharp loaded through `open()`
- [ ] Test: a missing feature-gated symbol fails only when called
- [ ] Mach-O run green
- [ ] Linux run green
- [ ] Gate: n00bb `./build.sh test` green
- [ ] Committed: "Late loading"

---

## N8. Run-time `ffi` module

- [ ] Stop: `n00b_abi_v1_register_image` accepts one image per process; confirm with the user how a run-time-built module is registered (a second image as a new generation, or another mechanism)
- [ ] Stop: confirm with the user the reflection API for listing members and calling by name
- [ ] writ: call thunks per `abi_class` signature shape (`ffi-spec.md` §12, decision 5)
- [ ] writ: thunks checked by the conformance tests used for the static path
- [ ] `lib/ffi.n`: `load_spec`, `bind`
- [ ] `bind` builds a reflection entry from the contract and returns a `Module`
- [ ] Reflection `call` routes through the generic call path
- [ ] Contract checks at each call: nullability, extents, ownership
- [ ] Variadic calls
- [ ] A compile-time `'ffi` module handed to `ffi.bind` keeps its spec text
- [ ] Programs that import `ffi` link libwrit; others don't
- [ ] Test: load `plugin.writ`, bind a plugin library, call it, including a variadic call
- [ ] Mach-O run green
- [ ] Linux run green
- [ ] Gate: n00bb `./build.sh test` green
- [ ] Committed: "Run-time ffi module"

---

## Phase 2 close

- [ ] `n00b-integration.md` describes what was built
- [ ] writ full suite, sanitizer build and Linux container green on the final state
- [ ] n00bb full suite green on the final state
