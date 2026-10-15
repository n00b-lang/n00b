# writ in n00b — Integration Plan

This document covers how n00b declares a C ABI with writ, declares the n00b
bindings it wants, and has the compiler generate and link those bindings at
compile time. It depends on the libwrit API in `implementation.md`.

Work lands in two repositories:

- **writ**: an n00b language profile, an n00b generator, and the extra C shim
  output n00b needs.
- **n00bb**: the `'ffi` literal, compile-time binding, package export,
  extern functions, linking, and runtime support.

---

## 1. What a user writes

The contract lives in its own file, `zlib.writ`:

```writ
library zlib
link    "z"
header  <zlib.h>
platforms aarch64-apple-darwin, aarch64-linux-gnu, x86_64-linux-gnu, x86_64-pc-windows-msvc

const Z_OK: c_int = 0

fn compress2(
    dest:      out *[u8; cap destLen.in, len destLen.out],
    destLen:   inout *c_ulong,
    source:    *[u8; sourceLen],
    sourceLen: c_ulong,
    level:     c_int,
) -> c_int
  error ret != Z_OK
```

n00b source binds it with one `'ffi` literal:

```
const zlib = [-
    use <zlib.writ>

    binding {
        name compress2 "compress"
    }
-]'ffi

let packed = zlib.compress(data, 6)
```

- An `'ffi` literal is an embedded writ unit (`dsl.md` §9.3): the
  contract, then n00b's choices for it. Its language is defined in
  `ffi-spec.md`.
- `use <zlib.writ>` brings in the contract from the contract search path,
  so it can be updated without touching the n00b source. A contract can
  also be written inline, or `use`d by a path relative to the n00b file.
- `binding { … }` imports; `binding export { … }` makes n00b functions
  available as the contract's C API. The contract and the language are
  implied.
- Binding happens when the preprocessor evaluates the literal. Its value
  is a `Module`, a builtin type. Written as a `const` initializer or as
  `import zlib from [- … -]'ffi`, `zlib` is a module known at compile
  time, and the bound API's namespace: `zlib.compress`, `zlib.Error`.

`zlib.compress(data, 6)` returns the compressed bytes or fails with a
status. Everything the call needs (the extern declaration, the
slice-to-pointer lowering, buffer sizing, error mapping, the C shim,
linking `-lz`) is generated and linked by the compiler.

**Only the bound API survives into the program.** The libwrit state and
the literal's text exist only during compilation, so none of it is
emitted. What remains is the generated module (with its reflection entry,
like any module), the shims, and the link requirements.

**Packages export it like any module.** `export zlib` in a `package`
statement puts the bound names into the package's logical view, so a
package `zlib` whose `__init.n` holds the literal gives consumers
`import zlib` and `zlib.compress(...)` (`ffi-spec.md` §9).

---

## 2. Where it runs in the compiler

n00b has two compile-time evaluators. Only one of them can create
declarations:

| Evaluator | Where | Can create declarations? |
|---|---|---|
| Preprocessor (`preprocessor.c:eval_expr`, `eval_builtin`) | In the compiler process, on the parse tree, before semantic analysis | Yes: it is the stage that decides what the program contains |
| `comptime` (`n00bb_execute_comptime`) | A separate native process, linked and spawned per compilation | No: results come back as values only |

writ therefore runs **in the preprocessor**:

- `'ffi` is a **builtin literal modifier whose behaviour the compiler
  supplies** (§4.2). Its function is never called, and the literal is
  never sent to the comptime process. The preprocessor hands the
  literal's contents straight to libwrit.
- Evaluating an `'ffi` literal binds it. The preprocessor resolves the
  contract for the target platform, checks the binding against it,
  generates n00b declarations as source, and parses that source with the
  n00b grammar as a module of its own. The const or import that the
  literal initializes names that module, before typing.
- The comptime process never needs libwrit, so nothing writ-related is
  added to its link line.

---

## 3. writ-side work

### 3.1 The n00b language profile (`profiles/n00b.writ`)

| Contract construct | n00b representation |
|---|---|
| Fixed-width integers, `c_*` types | `i8`…`i64`, `u8`…`u64`, with `c_*` resolved per platform |
| `bool` | n00b `bool` |
| `layout c` records | `type X layout(c) { ... }` |
| Records n00b's `layout(c)` can't express (bitfields, `layout explicit`) | Opaque storage with generated accessors |
| `handle` | A generated wrapper type holding the C value (`uptr` or integer), with `close()` |
| `bytes[n]`, `utf8[n]`, `cstr` | n00b `string` / byte buffers in, copied across the boundary by the shim |
| `out` and `inout` promoted to results | Multiple return values or a result record |
| `error ...` | n00b `fail:` results (`-> T fail: E`) |
| Rest packs | n00b lists |
| Keyword packs | n00b keyword arguments, where n00b has them; otherwise a generated options record |
| Callbacks with `userdata` | n00b `callback[T]` values, through generated trampolines (§4.6) |
| `unknown` | No safe binding is generated, and the compile fails unless the binding says `raw` |
| Memory model | Tracing GC. Pinning and rooting use `n00b_gc_root_push` / `n00b_gc_root_pop`. |

### 3.2 `writ_gen_n00b`

A new generator in libwrit's gen component. Given a model and an n00b import or export
binding, it produces:

- **n00b source text**: extern declarations (§4.3), wrapper functions,
  handle types and records. Each declaration has a span mapping back to the
  writ declaration that caused it, so errors in the generated code report
  at the `'ffi` literal, or in the contract file it `use`s.
- **C shim source** (through `writ_gen_c`): one C function per imported
  function, with an n00b-friendly signature (§4.4).
- **Link requirements**: the libraries and frameworks from the contract's
  `link`, resolved per platform.

### 3.3 Shims that normalize the calling convention

`writ_gen_c` gains a **pointer-normalized shim** mode. Each shim takes only
scalars and pointers. Aggregates passed or returned by value in C are
passed through pointers in the shim, and the shim makes the real C call.
n00b's backend then never has to classify C aggregates (§4.4).

Shims, callback trampolines and export entry points are emitted as C, and
compiled by clang to **LLVM bitcode** for the target. They are not
compiled to object files.

---

## 4. n00bb-side work

### 4.1 Linking libwrit into the compiler

- `meson.build` adds `dependency('writ-core')` and `dependency('writ-gen')` to
  `n00bb_exe`. `writ-clang` is linked only when a binding asks for
  verification against headers.
- The compiler already links a static lantern (`libir.a`). libwrit carries
  its own pinned lantern with every non-`writ_*` symbol localized
  (`implementation.md` §4.2), so the two never collide. A n00bb test links
  both and parses an `'ffi` literal and an n00b file in one process.

### 4.2 The `'ffi` literal modifier

- `Module` is a builtin type. At run time a `Module` value is a reference
  to the module's reflection entry.
  - A `const` of type `Module` is a module: member access, `export`, and
    package flattening treat it exactly as an imported module.
  - `import NAME from EXPR` accepts a `Module` expression known at compile
    time, as well as a string.
  - Static member access, `export` and `import … from` on a `Module` not
    known at compile time are compile errors; such a value is used
    through reflection.
- A user literal modifier may not shadow a builtin one.
- `'ffi` is declared in the builtin library as
  `litmod<fence> ffi(source: string) -> Module`. The preprocessor
  recognizes that builtin declaration and binds the literal itself; the
  modifier's function is never called.
- The preprocessor passes the literal's bytes, from `content_start_byte`
  to `content_end_byte`, to `writ_unit_add_embedded` with the language
  `n00b`:
  - The origin is the n00b file. Line and column are computed from
    `content_start_byte`, because tokens only store line and column for the
    `[-`.
  - Quoted `use` paths resolve relative to the n00b file.
  - Before the first `'ffi` literal, the compiler adds its contract
    directories to the search path with `writ_session_add_contract_dir`:
    each `--contract-dir` from the command line, then `contracts/` in the
    package being compiled.
- After binding, every file in `writ_unit_file_at` is added to the
  compilation's dependencies, so editing a used contract rebuilds the
  modules that bind it.
- At compile time the `Module` value also holds a handle to the writ
  unit, kept only for run-time binding (§4.9).
- The unit and its libwrit session are discarded after compilation.
  Nothing from them is emitted unless run-time binding asks for the spec
  text (§4.9).
- `n00bb --show-ffi MODULE.NAME` prints the generated n00b declarations
  for one `'ffi` module, and go-to-definition on a bound name lands in
  them.
- libwrit diagnostics are forwarded through `core_diagnostics_report_at`
  with the writ code in the message and the mapped line and column.
  Diagnostics in a used contract report in that file.

### 4.3 Extern functions

n00b calls C today only through the hardcoded builtin table
(`register_builtin_native_function`, binding `n00bb.abi.native`). Generated
bindings need the same thing for arbitrary symbols:

- Generated declarations carry an extern annotation naming the C symbol.
  They lower through the existing `ir_func_add(..., IR_LINK_EXTERN)` path
  with that annotation.
- Whether this is a **user-visible syntax** or an annotation only generated
  code can produce is a language decision (§6).

### 4.4 C calling convention

`llvm_backend.c:native_function_type` maps parameters directly to LLVM types
and passes structs as LLVM aggregates. It does no `byval`/`sret` handling and
no SysV, AAPCS or Win64 classification. That works for scalars and
pointers, but not for C structs passed by value.

n00b's backend never classifies C arguments. Instead:

1. Every C call goes through a pointer-normalized shim (§3.3), so n00b
   only ever calls functions whose parameters are scalars and pointers.
2. The shims are clang-compiled bitcode. n00bb already links LLVM's
   bitreader and linker, so it links that bitcode into the n00b module
   (`LLVMLinkModules2`) before optimization, and the optimizer inlines
   the shims.

clang therefore owns C argument passing for every platform, permanently,
and the shim costs nothing after inlining. The backend needs no SysV,
AAPCS or Win64 rules. When writ adds a platform, n00b gets it for free.

The run-time `ffi` module (§4.9) can't use clang at run time. It uses
writ's `abi_class` facts, which writ derives from the start
(`implementation.md` slice 1).

### 4.5 Memory, strings and handles

- **Strings and buffers.** n00b strings are 16-byte tagged values, not
  `char *`. Shims receive a pointer and length and copy in or out:
  - A `cstr` parameter gets a NUL-terminated copy.
  - `out` buffers are allocated by the shim at the declared capacity and
    copied into a new n00b string or buffer at the reported length.
- **Borrowed n00b memory goes through one runtime API.** Generated code
  never touches the GC directly. It calls:

  ```c
  void *n00b_ffi_borrow(void *object, size_t *length, uint32_t access);
  void  n00b_ffi_release(void *object);
  ```

  - `for call` spans wrap the call in a borrow and a release.
  - `keep` spans hold the borrow in the handle wrapper until the event.

  Whether the GC moves objects, and therefore whether a borrow roots,
  pins or copies, is a decision inside these two functions (§6). Changing
  that decision changes no generated code.
- **Handles.** Generated wrapper types hold the C value and expose
  `close()`. Release at collection time needs finalizer support from the
  GC (§6). Until that exists, handles must be closed explicitly, and
  leaked handles are reported in debug builds.

### 4.6 Callbacks

An n00b `callback` value is a function index plus an environment
(`n00b_abi_callback_value_v1`). It is not a C function pointer. Two
choices are fixed up front, so callbacks never need pinning and the
runtime grows exactly one entry point:

- **`userdata` carries a handle, never a GC pointer.** Registering a
  callback stores the n00b callback value in a runtime **handle table** and
  passes the table index to C as the `userdata`. C can hold the index for
  any span (`for call`, `keep until ...`, `once`) without affecting the
  GC. The handle is freed when the span ends, at the event the contract
  names.
- **One dispatch entry point.** For each callback type in a contract,
  `writ_gen_c` emits a typed C trampoline (as bitcode, §3.3). The
  trampoline packs the C arguments into a struct whose layout writ
  computes, and calls:

  ```c
  int32_t n00b_ffi_dispatch(uint64_t handle, const void *arguments, void *result);
  ```

  The runtime looks up the callback, unpacks the arguments with the same
  layout, and calls it.

The handle table and `n00b_ffi_dispatch` are new runtime work in
`builtin_abi.c`. Callbacks without a `userdata` slot can't carry a handle.
For those, the binding requires a `static` n00b function, and the
trampoline calls its symbol directly.

### 4.7 Linking

The final link has `--link-object` but no library flags. Linking gains:

- the shim bitcode, linked into the module before optimization (§4.4);
- libraries from the contract's `link`, as `-l`, `-framework`, or
  `.lib`, chosen per platform;
- library search paths from the binding or the command line.

These come from `writ_gen_n00b`'s link requirements, not from user flags.

### 4.8 Export direction

An `'ffi` literal with a `binding export { … }` part:

- generates C-ABI entry points, through `writ_gen_c`, that convert
  arguments, root GC values, and call the n00b function by its symbol
  (`module.function`);
- names the exported symbols as the contract says, generalizing today's
  static `c_alias` table in `runtime_native_roots.c`;
- emits a C header for the contract through `writ_gen_c`, so C callers get
  a header that matches by construction;
- keeps every exported function reachable (`--native-root` behavior).

### 4.9 Binding at run time

Binding at compile time is the default, but programs can also bind at
run time. There are two levels.

**Late loading: the contract is known at compile time, the library is
loaded at run time.** The binding asks for it:

```
const resharp = [-
    use <resharp.writ>

    binding {
        load dynamic
    }
-]'ffi
```

- The literal generates the same declarations and shims. The only
  difference is that symbols are resolved through
  `dlopen`/`dlsym` (`LoadLibraryW`/`GetProcAddress` on Windows), not by
  the linker.
- The namespace gets an `open(path)` that loads the library and
  reports any missing symbols. Functions marked `weak` or `since` that
  aren't present fail when called, not at load time.
- libwrit is not needed at run time.

**Fully dynamic: the contract itself arrives at run time.** A program that
reads a contract from a file, or builds one, uses a run-time FFI module:

```
import ffi

func main() {
    spec   := ffi.load_spec(read_file("plugin.writ"))?
    plugin := ffi.bind(spec, "./libplugin.so")?
    result := plugin.call("plugin_process", buffer, len(buffer))?
}
```

- `ffi.bind` returns a `Module` whose reflection entry is built at run
  time from the contract. It is used through reflection like any `Module`
  not known at compile time; `call` is reflection's dynamic call.
- Using `ffi` links `libwrit` into the program. It is the only case
  where libwrit is present at run time.
- Calls go through a **generic call path** driven by the model: argument
  conversion follows the contract's shapes, and the call itself is a
  dynamic call built from the target's calling convention (variadic
  included), using the contract's `abi_class` facts.
- Values cross as n00b dynamic values, and every contract check
  (nullability, extents, ownership on return) runs at the call.
- An `'ffi` module can also be handed to `ffi.bind` at run time. Doing so
  keeps the spec's text in the program, which is the one case where
  compile-time spec state is emitted.

### 4.10 Package export

An `'ffi` module is exported from a `package` statement like any module
(`ffi-spec.md` §9):

- `export zlib` puts every bound name into the package's logical view,
  flattened.
- `export zlib as name` is rejected, as any renamed module export is.
- A subpackage that exports the module keeps the bound names under the
  subpackage's name.

Package export resolution reaches a `const` of type `Module` through the
same path as an imported module. The bound names are ordinary
declarations by then, so nothing else in package linking changes.

### 4.11 Baking C objects built at comptime

`'ffi` functions can be called from comptime code. A C object graph they
return can be the value of a `const`, and is then **baked**: copied into
the program's data and used at run time without being rebuilt.

```
const config = settings.load("app.conf")
```

- **Description.** Every struct reachable from the value must be fully
  described by its contract. writ's layout facts become n00b record
  types with descriptors: counted pointers carry their count field,
  zero-terminated arrays their sentinel, unions their discriminant, and
  function pointers are marked as such. A pointer the contract doesn't
  describe is a compile error at the `const`, naming the field path.
- **Copy.** The comptime copier follows foreign pointers using those
  descriptors. Shared targets are copied once, and pointers into the
  middle of a block stay pointers into the copy. Function pointers are
  recorded by symbol and relocated.
- **Placement.** A baked foreign graph is emitted once into a writable
  section and used in place. It is not cloned per evaluation.
- **Ownership.** Baked objects are never released. A release or `close()`
  on one fails with `Error`.
- **Hashing.** A table keyed by object address stays valid when its keys
  cache their hash; the key type implements `Hash` by returning the
  cached value.
- **Fixups.** State that can't survive baking (file descriptors, sockets,
  caches, locks) is restored by the builtin trait

  ```
  trait Fixup {
      func fixup(modify self: `T) -> nil fail: string
  }
  ```

  A baked object whose type implements `Fixup` runs `fixup` once, before
  its first use. If it fails, that use fails with the fixup's error and
  the object stays unfixed, so a later use tries again.

---

## 5. Phases

| Phase | Lands | Done when |
|---|---|---|
| S1 | **Spec pause: the `'ffi` literal.** Work stops here until the spec is written and signed off. It is the same pause as the start of `implementation.md` slice 2. | `ffi-spec.md` is approved (§5.1) |
| W0 | Embedded units (`writ_unit_add_embedded`, E0514), `use <name.writ>` and the contract search path, `writ_unit_file_count` / `writ_unit_file_at` | Golden tests: an embedded unit that `use`s a contract by path and by search path, binds it with and without a binding part, and lists every file it read; each E0510 and E0514 case |
| W1 | `profiles/n00b.writ`, `writ_gen_n00b`, pointer-normalized shims and trampolines as bitcode | Golden n00b and C output for the zlib and resharp literals |
| N1 | libwrit linked into n00bb; symbol coexistence test | A test compiles an n00b program in a compiler that parsed an `'ffi` literal, with no symbol clash |
| N2 | The `'ffi` literal, compile-time const values, contract directories, dependency recording, diagnostic forwarding, `--show-ffi` | A malformed literal reports at the right n00b line and column, an error in a used contract reports in that file, editing a used contract rebuilds the module, and no spec state appears in the output binary |
| N3 | `Module` type, `import … from` a `Module`, extern declarations from generated code, the generated module, package export, shim bitcode linking, library linking | An n00b test calls zlib `compress2` / `uncompress` through a `zlib` package and round-trips data on Mach-O and Linux |
| N4 | Strings and buffers through `n00b_ffi_borrow`, out-params, handles with `close()`, error channels | An n00b program compiles resharp patterns, matches, finds all spans, and reports a compile error from `resharp_last_error` |
| N5 | Handle table, callback trampolines, `n00b_ffi_dispatch` | `qsort` with an n00b comparator; a registered callback kept until unregistered |
| N6 | `binding export`: entry shims, symbol naming, generated header | A C program includes the generated header and calls an n00b implementation of the `kv` contract |
| N7 | Late loading (`load dynamic`) | The resharp test passes with the library loaded through `open()`; a missing feature-gated symbol fails only when called |
| N8 | Run-time `ffi` module: `load_spec`, `bind` returning a `Module` with a reflection entry built at run time, generic calls | A program loads `plugin.writ` at run time and calls into a plugin library, including a variadic call |

The work runs in two phases. Phase 1 is S1, W0, W1 and N1–N6: binding at
compile time and linking statically. It also makes `'ffi` modules
callable from comptime code, and lets a C object graph built at comptime
be baked into the program and used at run time (§4.11). Phase 2 is N7
and N8: dynamic loading. Until Phase 2, `load dynamic` is rejected as not yet supported.

N1 can run before S1, since it only links libwrit and has no binding
content. Everything else waits for S1. W0 comes first after S1, and W1
runs in parallel with N2. N3 needs `implementation.md` slice 1. N4 needs
slice 2.

### 5.1 The `'ffi` spec pause

The `'ffi` examples in this document only illustrate. The language itself
is defined in `~/writ/ffi-spec.md`, and nothing that consumes it is built
until that document is approved. It covers:

- **Shape.** The `'ffi` literal as an embedded writ unit (`dsl.md` §9.3),
  `use` and the contract search path (`dsl.md` §3.2), and which parts are
  n00b-specific.
- **Surface mapping.**
  - Names: how C declarations become functions, methods and types in the
    literal's module.
  - Grouping functions into methods on handle types.
  - Hiding and renaming.
- **Type choices.** For each contract construct, the n00b types a binding
  may choose, with a default from `profiles/n00b.writ`:
  - strings vs. byte buffers;
  - lists vs. slices for rest packs;
  - keyword arguments vs. options records;
  - label mappings (`path`, `secret`).
- **Errors.** How `error` clauses become `fail:` types, how error detail
  (errno, status codes, error strings) is carried, and per-function
  overrides.
- **Ownership choices.**
  - Copy vs. borrow where the contract allows both.
  - `until` spans: copy out vs. invalidating handles.
  - Policy-arm selection (`dsl.md` §5.6).
  - How handles close.
- **Callbacks.** How n00b callbacks are named and typed, and what
  `static`-only callbacks look like when there is no `userdata`.
- **Loading.** `load dynamic`, library search paths, and per-platform
  library names.
- **Export.** Mapping contract declarations to the n00b functions that
  provide them, symbol naming, the GC and thread rules for incoming calls,
  and the generated header's name.
- **Diagnostics.** What a binding that contradicts its contract reports,
  and where.
- **Packages.** How an `'ffi` module is exported from a `package`
  statement.
- **Grammar and examples.** The embedded-unit grammar, plus complete
  `'ffi` literals for the zlib, resharp and kv contracts. These become the
  golden inputs for W1 and N3–N6.

The decisions in §6 are settled during this pause, because several of
them (keyword arguments, extern visibility, finalizers) decide what a
binding spec can express.

---

## 6. Decisions for n00b

1. **Extern syntax.** Should n00b get a user-visible `extern` declaration
   that generated bindings also use, or should extern stay an annotation
   only generated code produces?
2. **GC movement.** Does the n00b collector move objects? The answer
   decides what `n00b_ffi_borrow` does: root, pin or copy (§4.5).
   Generated code is the same either way.
3. **Finalizers.** Will the GC support finalizers for handle wrappers, or
   is explicit `close()` the model?
4. **Keyword arguments.** Do keyword packs map to n00b keyword arguments, or
   to generated options records?
5. **Dynamic calls.** Should the run-time `ffi` module's generic call path
   use libffi, or call thunks that writ generates from `abi_class` facts?
   libffi is ready now. writ's own thunks avoid an extra dependency, and
   are checked by the same conformance tests as the static path.
