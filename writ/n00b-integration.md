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
- Binding happens when the preprocessor evaluates the literal. The
  `const` it initializes becomes an import of the generated module, so
  `zlib` is the bound API's namespace: `zlib.compress`, `zlib.Error`.

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

A `build` runs two compiles of the same program:

| Compile | What it does |
|---|---|
| Build-time (`--build-time`) | `comptime()` is true and `const` is writable. The program is linked and run on the real runtime and GC, with the same shims and native inputs as the shipped build. A capture of every `const` value is written back to the compiler. |
| Shipped | `comptime()` is false. `const` values come from the capture; nothing else from the build-time run reaches the program. |

writ runs **in the preprocessor** of each compile:

- `'ffi` is a **builtin literal modifier whose behaviour the compiler
  supplies** (§4.2). Its function is never called. The preprocessor
  hands the literal's contents straight to libwrit.
- Evaluating an `'ffi` literal binds it. The preprocessor resolves the
  contract for the host platform, checks the binding against it,
  generates n00b declarations as source, and loads that source as a
  module of its own. The const that the literal initializes becomes an
  import of that module, before typing.
- Both compiles bind the literal the same way. Shim bitcode and Rust
  transport builds are cached on disk, so the second binding reuses the
  first one's native work.
- libwrit is linked only into the compiler. Programs never link it.

---

## 3. writ-side work

### 3.1 The n00b language profile (`profiles/n00b.writ`)

| Contract construct | n00b representation |
|---|---|
| Fixed-width integers, `c_*` types | `i8`…`i64`, `u8`…`u64`, with `c_*` resolved per platform |
| `bool` | n00b `bool` |
| `layout c` records | `type X layout(c) { ... }` |
| Records n00b's `layout(c)` can't express (bitfields, `layout explicit`) | Opaque storage with generated accessors |
| `handle` | A generated `X_Cell` record holding the C value, used through `alias X = Ref[X_Cell]`, with `close()` and a finalizer (§4.5) |
| `bytes[n]`, `utf8[n]`, `cstr` | n00b `string` or `Array[u8]`, copied across the boundary by the shim; a `borrow` slot passes the array's storage directly |
| `out` and `inout` promoted to results | Multiple return values or a result record |
| `error ...` | n00b `fail:` results (`-> T fail: E`) |
| Rest packs | `List[T]` by default, or `Array[T]` |
| Keyword packs | n00b keyword arguments by default; a binding that chooses `record` gets a generated `<Fn>_Options` record |
| Callbacks | n00b callback values, through generated trampolines (§4.6) |
| `unknown` | No safe binding is generated, and the compile fails unless the binding says `raw` |
| Memory model | Non-moving tracing GC (`memory gc nonmoving`); borrows go through `n00b_ffi_borrow` / `n00b_ffi_release` (§4.5) |

### 3.2 `writ_gen_n00b`

A generator in libwrit's gen component (`include/writ/gen.h`). Its request
is `writ_gen_n00b_request_t`: the binding, the generated module's name, a
shim prefix, and a type namespace. For an import binding it produces:

- **n00b source text** (`<module>.n`): wrapper functions, handle types,
  records, enums, constants and the generated `Error` type, with a line
  map (`<module>.n.map`) back to the writ declarations, so errors in
  generated code report at the `'ffi` literal or in the contract file it
  `use`s.
- **Extern declarations as data** (`writ_n00b_extern_t`: n00b name, C
  symbol, scalar-and-pointer signature). They are not part of the n00b
  text (§4.3).
- **C shim source** (`<prefix>_n00b.c`, through `writ_gen_c`): one C
  function per imported function, plus callback trampolines and their
  argument structs (§4.4, §4.6).
- **Link requirements** (`writ_link_requirements_t`): libraries,
  frameworks and search paths from the contract and binding, resolved
  per platform, and the load mode.

For an export binding it produces the entry shims
(`<prefix>_n00b_export.c`), the exported symbols as data
(`writ_n00b_export_t`), and the C header for the contract (§4.8).

`writ gen n00b --embedded FILE -o DIR` writes the same outputs, and
`writ --embedded NAME bind FILE --profile FILE` prints the binding's
decisions.

### 3.3 Shims that normalize the calling convention

`writ_gen_c` has a **pointer-normalized shim** mode
(`writ_gen_c_request_t.pointer_normalized`). Each shim takes only scalars
and pointers. Aggregates passed or returned by value in C are passed
through pointers in the shim, and the shim makes the real C call. n00b's
backend then never has to classify C aggregates (§4.4).

Shims, callback trampolines and export entry points are emitted as C and
compiled by clang to **LLVM bitcode** for the target.

---

## 4. n00bb-side work

### 4.1 Linking libwrit into the compiler

- `meson.build` adds `dependency('writ')` to `n00bb_exe`, found through
  writ's uninstalled pkg-config files. Its `profiledir` variable locates
  `profiles/n00b.writ`.
- The compiler already links a static lantern (`libir.a`). libwrit carries
  its own lantern copy with every non-`writ_*` symbol localized
  (`implementation.md` §4.2), so the two never collide.
  `tests/writ_coexistence.c` resolves an `'ffi` literal through libwrit and
  parses an n00b file through lantern in one process.

### 4.2 The `'ffi` literal modifier

- `'ffi` is declared in the builtin library (`lib/builtin/ffi.n`) as a
  `litmod<fence>` whose body fails if it is ever called. The preprocessor
  recognizes the modifier and binds the literal itself.
- An `'ffi` literal is the initializer of a module-level `const`:

  ```
  const zlib = [- use <zlib.writ> -]'ffi
  ```

  The preprocessor replaces the const with `import zlib`, aliased to the
  generated module `<parent>.__ffi_<leaf>_<name>`, named from the
  importing module and the const. An `'ffi` literal anywhere else is an
  error at the literal.
- The preprocessor passes the literal's bytes, from `content_start_byte`
  to `content_end_byte`, to `writ_unit_add_embedded` with the language
  `n00b`, then calls `writ_resolve`, `writ_bind` with the n00b profile,
  and `writ_gen_n00b`:
  - The origin is the n00b file. Line and column are computed by scanning
    the source up to `content_start_byte`, because tokens only store line
    and column for the `[-`.
  - Quoted `use` paths resolve relative to the n00b file.
  - The compiler opens one writ session per process. When the session
    opens, it adds each `--contract-dir` to the contract search path;
    each binding then adds `contracts/` in the package being compiled.
- The generated module is never cached, so every build binds the literal
  again, and editing a used contract takes effect on the next build.
- `n00bb --show-ffi MODULE.NAME` prints the generated n00b source for one
  `'ffi` module.
- libwrit diagnostics are reported through `core_diagnostics_report_at`
  with the writ code as the diagnostic code and the mapped line and
  column. Diagnostics in a used contract report in that file.
- The writ session is freed when the compiler exits. Nothing from it is
  emitted into the program.

### 4.3 Extern functions

There is no `extern` syntax. Extern declarations exist only in generated
modules:

- `writ_gen_n00b` returns them as data (§3.2). n00bb registers each one as
  a native function of the generated module, with the binding
  `n00bb.generated-extern`.
- They lower through `ir_func_add(..., IR_LINK_EXTERN)` with the symbol
  annotation, and `llvm_backend.c` types them as their scalar-and-pointer
  signatures.
- User code can't name them: the generated wrappers are the only callers.

### 4.4 C calling convention

n00b's backend never classifies C arguments:

1. Every C call goes through a pointer-normalized shim (§3.3), so n00b
   only calls functions whose parameters are scalars and pointers.
2. Shims are clang-compiled bitcode, cached in the native cache by
   content, contract directories and headers. Only the shims of literals
   whose externs are reachable are compiled; export shims are always
   compiled.
   - With `--lto`, the shim bitcode is linked into the program's bitcode
     with `LLVMLinkModules2` before optimization, and the optimizer
     inlines the shims.
   - Without `--lto`, the shims are linked together into one native
     object of their own.

clang therefore owns C argument passing for every platform, and the
backend needs no SysV, AAPCS or Win64 rules. When writ adds a platform,
n00b gets it for free.

The run-time `ffi` module (§4.9) can't use clang at run time. It uses
writ's `abi_class` facts, which writ derives from the start
(`implementation.md` slice 1).

### 4.5 Memory, strings and handles

- **Strings and buffers.** n00b strings are 16-byte tagged values, not
  `char *`. Shims receive a pointer and length and copy in or out:
  - A `cstr` parameter gets a NUL-terminated copy.
  - `out` buffers are allocated by the shim at the declared capacity and
    copied into a new n00b string or buffer at the reported length.
    Bytes returned as `string` are decoded as UTF-8; invalid text fails
    with `Error`.
- **Borrowed n00b memory goes through one runtime API**
  (`runtime/runtime_ffi.c`):

  ```c
  void *n00b_ffi_borrow(void *object, size_t *length, uint32_t access);
  void  n00b_ffi_release(void *object);
  ```

  The collector does not move objects, so `n00b_ffi_borrow` returns the
  array's storage and length, and `n00b_ffi_release` does nothing. The
  borrowed object stays live through the caller's frame across the call.
  `for call` spans wrap the call in a borrow and a release; `keep until
  EVENT` spans hold the borrow in the handle wrapper until the event.
- **Handles.** A handle is a generated `X_Cell` record holding the C
  value, used through `alias X = Ref[X_Cell]`:
  - `close()` calls the release function and marks the handle closed. A
    call on a closed handle fails with `Error`; `consume` closes it too.
  - `with x` results keep `x`'s wrapper alive.
  - The cell has a `__finalize`. In debug builds, a handle collected
    while open is reported ("collected while open; releasing it") and
    released.

### 4.6 Callbacks

An n00b callback value is a function index, a callback type index and an
environment (`sys.internal.abi.Callback_Value`). It is not a C function
pointer, and C never holds a GC pointer:

- **`userdata` carries a handle.** For each callback type, the generated
  module has a handle table (`builtin.ffi.Ffi_Table`). Registering a
  callback stores the callback value in the table and passes the table
  index to C as the `userdata`. The handle is freed when the span ends,
  at the event the contract names, or after the call for `once`.
- **One dispatch function per callback type.** The generated module
  defines `__<type>_dispatch(handle, arguments, result)` and exports it
  as `<prefix>___<type>_dispatch`. The C trampoline packs the C arguments
  into a struct whose layout writ computes and calls that function; the
  dispatcher unpacks them with the same layout and calls the callback.
- **Callbacks without `userdata`** call the `provide`d n00b function by
  its symbol, through a generated trampoline.

### 4.7 Linking

The final link takes, from every `'ffi` module in the program,
deduplicated:

- the shims (§4.4);
- libraries from the contract's `link` as `-l` or `-framework`, and
  archives, chosen per platform;
- library search paths from the binding.

Each required library is probed with a test link before the program is
built; a missing one is N00BB-F0001.

A Rust contract's transport crate is generated with `writ_gen_rust` and
built with cargo by n00bb, cached in the native cache; its static library
and the native libraries cargo reports join the link.

### 4.8 Export direction

An `'ffi` literal with a `binding export { … }` part:

- generates C-ABI entry shims that convert arguments and call an n00b
  adapter for the `provide`d function. An entry shim saves an unwind
  point: a panic inside the call returns to the shim, which applies the
  binding's `on_panic` (`abort` traps; `error EXPR` returns `EXPR`).
- A foreign thread becomes a runtime mutator the first time it touches
  the runtime, so entry shims need no explicit attach. Values handed to C
  stay live because the generated module holds them in its handle tables
  and export holds.
- Contract `thread` constraints (main-thread functions, thread-affine
  handles) are checked in debug builds.
- `own` results are copied into the binding's named allocator.
- Symbols follow the contract names and `symbol` clauses, and every
  exported function is a native root.
- The C header for the contract is written next to the build output, so C
  callers get a header that matches by construction. A `provide` whose
  signature doesn't match its contract function is reported at the
  `provide` item.

### 4.9 Binding at run time

Binding at run time is Phase 2 (§5). Until then a binding that asks for
`load dynamic` is rejected with E0901 ("not supported yet").

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

Package export resolution reaches an `'ffi` const through its import
alias, the same path as an imported module. The bound names are ordinary
declarations by then, so nothing else in package linking changes.

### 4.11 Baking C objects built at comptime

`'ffi` functions can be called from comptime code. A C object graph they
return can be the value of a `const`, and is then **baked**: copied into
the program's data and used at run time without being rebuilt.

```
const config = settings.load("app.conf")
```

- **Description.** Every struct reachable from the value must be fully
  described by its contract. writ's layout facts become `layout(c)`
  n00b records whose pointer fields are `Foreign[T]` or `Foreign_Fn[...]`,
  with field clauses: `count FIELD` for a counted pointer, `until EXPR`
  for a sentinel-terminated array, and `union by FIELD { … }` for a
  union and its discriminant (N00BB-T0037 rejects malformed clauses).
  A pointer the contract doesn't describe fails the capture, naming the
  field path.
- **Copy.** The copier (`sys.internal.marshal`) runs in the build-time
  program and follows foreign pointers using those descriptors.
  Overlapping blocks are merged, so shared targets are copied once and
  pointers into the middle of a block stay pointers into the copy.
  Function pointers are recorded by their native symbol and relocated; a
  function without a symbol fails the capture.
- **Placement.** A baked foreign graph is emitted once into a writable
  section and used in place. It is not cloned per evaluation.
- **Ownership.** Baked objects are never released. A release or `close()`
  on one fails with `Error` ("a baked object cannot be released"), and
  handle finalizers skip baked objects.
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
  the first foreign call that takes it. If it fails, that call fails with
  the fixup's error and the object stays unfixed, so a later call tries
  again.

### 4.12 Incremental builds

`n00bb build` with one input caches each strongly connected component of
the module graph (an SCC) as one object in `<root>/.n00b-cache/obj/`. An
object holds the SCC's native code, ABI metadata, semantic interface,
runtime and comptime IR, and recorded specialization requests.

- **Keys.** An SCC's key covers its members' sources, the interface
  identities of the SCCs it imports, the freshness of the SCCs whose
  inline bodies it uses, the compiler, the flags that change code, and
  the target.
- **Reuse.** A cached SCC skips every stage up to codegen. It feeds the
  link stages through its interface, and its cached IR supplies the
  bodies that inlining, specialization and comptime execution need. A
  compile only sees the cached SCCs of modules it imports, so programs
  can share a cache directory.
- **Body edits.** When an import is rebuilt, a dependent SCC stays cached
  as long as the import's interface identity is unchanged. The compiler
  checks that once the rebuilt interfaces are known, and reruns the build
  without the assumption (`N00BB_CACHE_STRICT`) when it fails.
- **Instantiations** are cached separately, keyed by the generic's SCC
  and the type arguments.
- `--lto` and `N00BB_NO_OBJECT_CACHE` turn the cache off.
  `N00BB_CACHE_LOG=FILE` records each reuse and store.
- The generated module of an `'ffi` literal and the root SCC are never
  cached.

---

## 5. Phases

| Phase | Lands | Done when |
|---|---|---|
| S1 | **Spec pause: the `'ffi` literal.** Work stops here until the spec is written and signed off. It is the same pause as the start of `implementation.md` slice 2. | `ffi-spec.md` is approved (§5.1) |
| W0 | Embedded units (`writ_unit_add_embedded`, E0514), `use <name.writ>` and the contract search path, `writ_unit_file_count` / `writ_unit_file_at` | Golden tests: an embedded unit that `use`s a contract by path and by search path, binds it with and without a binding part, and lists every file it read; each E0510 and E0514 case |
| W1 | `profiles/n00b.writ`, `writ_gen_n00b`, pointer-normalized shims and trampolines as bitcode | Golden n00b and C output for the zlib and resharp literals |
| N1 | libwrit linked into n00bb; symbol coexistence test | A test compiles an n00b program in a compiler that parsed an `'ffi` literal, with no symbol clash |
| N2 | The `'ffi` literal, compile-time const values, contract directories, dependency recording, diagnostic forwarding, `--show-ffi` | A malformed literal reports at the right n00b line and column, an error in a used contract reports in that file, editing a used contract rebuilds the module, and no spec state appears in the output binary |
| N3 | The generated module and its import alias, extern declarations from generated code, package export, shim linking, library linking | An n00b test calls zlib `compress2` / `uncompress` through a `zlib` package and round-trips data on Mach-O and Linux |
| N4 | Strings and buffers through `n00b_ffi_borrow`, out-params, handles with `close()`, error channels | An n00b program compiles resharp patterns, matches, finds all spans, and reports a compile error from `resharp_last_error` |
| N5 | Handle tables, callback trampolines, per-type dispatch functions | `qsort` with an n00b comparator; a registered callback kept until unregistered |
| N6 | `binding export`: entry shims, symbol naming, generated header | A C program includes the generated header and calls an n00b implementation of the `kv` contract |
| N7 | Late loading (`load dynamic`) | The resharp test passes with the library loaded through `open()`; a missing feature-gated symbol fails only when called |
| N8 | Run-time `ffi` module: `load_spec`, `bind` returning a `Module` with a reflection entry built at run time, generic calls | A program loads `plugin.writ` at run time and calls into a plugin library, including a variadic call |

The work runs in two phases. Phase 1 is S1, W0, W1 and N1–N6: binding at
compile time and linking statically. It also makes `'ffi` modules
callable from comptime code, and lets a C object graph built at comptime
be baked into the program and used at run time (§4.11), and adds the
incremental build cache (§4.12). Phase 2 is N7 and N8: dynamic
loading. Until Phase 2, `load dynamic` is rejected as not yet supported.

N1 can run before S1, since it only links libwrit and has no binding
content. Everything else waits for S1. W0 comes first after S1, and W1
runs in parallel with N2. N3 needs `implementation.md` slice 1. N4 needs
slice 2.

### 5.1 The `'ffi` spec pause

The `'ffi` examples in this document only illustrate. The language itself
is defined in `writ/ffi-spec.md`, and nothing that consumes it is built
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

1. **Extern syntax.** Extern declarations are generated only; n00b has no
   `extern` syntax (§4.3).
2. **GC movement.** The collector does not move objects, so
   `n00b_ffi_borrow` hands C the object's storage directly (§4.5).
3. **Finalizers.** Handle wrappers have finalizers; debug builds report
   and release handles collected while open. `close()` is the release
   model (§4.5).
4. **Keyword arguments.** Keyword packs map to n00b keyword arguments; a
   binding can choose a generated options record instead.
5. **Dynamic calls.** Open until Phase 2: the run-time `ffi` module's
   generic call path uses either libffi or call thunks that writ generates
   from `abi_class` facts. libffi is ready now. writ's own thunks avoid an
   extra dependency, and are checked by the same conformance tests as the
   static path.
