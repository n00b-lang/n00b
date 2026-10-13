# writ — Implementation Plan

This is the implementation plan for writ, the contract language specified
in `dsl.md`.

writ is a **C library with a public API**. Compilers and tools embed it.
n00b, for example, holds writ declarations inside its own source files and
its compiler calls the API directly. The `writ` command-line tool is a thin
wrapper over that API, with no logic of its own. Contract, profile and
binding files use the `.writ` extension.

Lantern lexes and parses the language; its IR, analysis and codegen are not
used. libclang does header import and layout queries.

---

## 1. Scope

v1 is built in **vertical slices** (§5). Each slice takes one real library
all the way through:

language → model → verification → generated code → a working call

A language construct is implemented only when a slice's library needs it.
These form the **core tier** of `dsl.md`. Everything else in the spec stays
designed but unimplemented (the **extended tier**), and v1 rejects it with
"not supported yet" (`dsl.md` §1, Tiers).

The libraries, in order:

1. **zlib**: a C library with a header.
2. **resharp** (`github.com/ieviev/resharp`): a Rust library, the first
   n00b runtime dependency. It is bound through its Rust API (`lang rust`,
   `dsl.md` §8.9), with a generated C-ABI transport. Its hand-written
   `resharp-ffi` crate is not used.
3. The rest of the libraries the default n00b runtime links.

Across the slices, v1 delivers:

1. **libwrit**, a set of C libraries behind one public API:
   - parsing contract, profile and binding sources, from files or from
     text embedded in a host language's source;
   - resolution, defaults, checks and a queryable **model**, plus its
     canonical JSON form;
   - verification: layout via libclang and compiled probes, symbols via
     export tables, and argument-passing facts (`abi_class`) via LLVM IR
     probes;
   - import: a contract draft from C headers via libclang, and from Rust
     crates via rustdoc JSON;
   - generators:
     - C (headers, probes, pointer-normalized shims, ABI conformance
       tests);
     - the Rust transport (the generated `extern "C"` crate);
     - n00b (`n00b-integration.md`).
2. **`writ`**, the CLI: argument parsing and output only.

Slice 0 runs on the Mac. From Slice 1 on, each slice must pass on Mach-O
and Linux. Windows is the final slice:

| Platform | Object format | Where it runs | From |
|---|---|---|---|
| `aarch64-apple-darwin` | Mach-O | Natively on the development Mac | Slice 0 |
| `aarch64-linux-gnu` | ELF | In a container on the Mac. `x86_64-linux-gnu` runs under emulation, in full mode only. | Slice 1 |
| `x86_64-pc-windows-msvc` | PE/COFF | A Windows machine, through a verification bundle (§4.9) | Final slice |

**Not in v1** (designed in `dsl.md`; order in §8):

- extended-tier constructs that no slice's library needs;
- the Rust import generator;
- generators for other languages;
- C++ bindings;
- the `jvm` and `cpython` ABI families;
- build integrations other than Meson and Cargo;
- incremental re-resolution;
- building libwrit natively on Windows.

---

## 2. Code conventions

- C23 (`c_std=c2x`), built with clang. Lantern's headers need C23 plus GNU
  extensions. Lantern headers are private to libwrit. The **public writ
  headers are plain C11** and also compile as C++ and with MSVC, so any host
  can include them.
- **No comments**, in hand-written code, generated code, the grammar,
  build files or scripts. Names and structure carry the meaning.
- Generated code has no banners or explanatory comments. Provenance lives
  in the model and the JSON, not in the output.
- **Library rules:**
  - libwrit never prints, never exits, never aborts on user input, and
    never reads environment variables.
  - It has no mutable global state.
  - Every external process (compiler, `llvm-nm`) is started through a
    runner the host can replace.
  - All memory belongs to a session and is freed with it.
- **CLI rule:** `src/cli/` includes only public headers from
  `include/writ/`. A build check fails if it includes anything else.
- Diagnostics use lantern's diagnostic machinery (§4.11). Every diagnostic
  has a stable code.

---

## 3. Dependencies

### 3.1 Lantern, pinned

writ uses its own jj workspace of the lantern repository, at
`~/lantern-writ`. That directory is the lantern root for everything writ
does. The pin is the revision `~/lantern`'s default workspace is on when
this plan is approved:

```
jj -R ~/lantern workspace add --name writ -r <revision> ~/lantern-writ
```

- The pinned commit id is recorded in `~/writ/deps/LANTERN_REVISION`,
  which is committed to writ.
- `build.sh` refuses to build unless both hold:
  - the parent of the workspace's working-copy commit
    (`jj -R ~/lantern-writ log -r @- -T commit_id`) is the pinned commit;
  - the working copy is empty (`jj -R ~/lantern-writ diff --summary`
    prints nothing).
- Moving the pin is deliberate: run
  `jj -R ~/lantern-writ new <revision>`, update `LANTERN_REVISION`, and
  run the full suite on every supported platform.
- The default workspace, `~/lantern`, is never built from or modified by
  writ.
- `LANTERN_ROOT` overrides the location, so a different machine can put
  the workspace elsewhere.

Lantern builds one static library, `ir`, with its own `build.sh`. It needs
ICU (`icu-uc`, `icu-i18n`) and C23 with GNU extensions. Our `build.sh`
runs lantern's `build.sh` in `~/lantern-writ`, with `BUILD_DIR` pointing
outside the workspace, so build output never shows up as a jj change. It
then installs into a private prefix:

- `deps/prefix` on the Mac;
- `deps/prefix-linux` in the container.

Meson consumes it from there through `ir.pc`.

### 3.2 Other dependencies

| Dependency | Needed by | Used for |
|---|---|---|
| ICU | `libwrit` (through lantern) | Lantern's lexer |
| LLVM 22 libclang (C API) | `libwrit_clang` only | Import, layout, mangling |
| Rust stable (Cargo) | `libwrit`, at run time, through the process runner | Building generated transports and probe crates |
| Rust nightly | Importing Rust crates only | `rustdoc --output-format json` |
| LLVM 22 `clang`, `llvm-nm`, `llvm-readobj`, `llvm-cxxfilt` | `libwrit`, at run time, through the process runner | Probes, export tables for Mach-O, ELF and PE/COFF, demangling |
| Meson, Ninja | build | |
| Python 3 | build and tests only | Grammar embedding, spec-example extraction, golden-test runner, bundle runner |
| Podman (or Docker) | tests | The Linux platform |
| On the Windows machine: Visual Studio Build Tools, LLVM 22, Python 3, Rust (MSVC toolchain) | tests | Executing verification bundles |

The LLVM prefix is set by `-Dllvm_prefix=`. libclang and the LLVM tools
always come from that one prefix.

---

## 4. Architecture

### 4.1 Components

| Library | Components | Contents | Depends on |
|---|---|---|---|
| `libwrit` | core | Sessions, sources, parsing, AST, resolution, checks, model, call plans, JSON lowering, formatting, diagnostics, the process-runner interface | lantern (hidden), ICU |
| | toolchain | Probe generation and compilation, symbol verification, the rustdoc-JSON importer, verification bundles | |
| | gen | Profile and binding resolution; the C, Rust transport and n00b generators | |
| `libwrit_clang` | clang | Translation units, layout queries, importer, attribute reading | `libwrit`, libclang |

Core, toolchain and gen share session internals, so they are one
partially linked object (§4.2): splitting them would leave those internals
global. None of them pulls in a heavy dependency. libclang is the one
heavy dependency, so it lives alone in `libwrit_clang`, which uses only
the public API. A compiler that needs the model links `libwrit` and never
pulls in libclang. The CLI links both.

The components remain separate directories under `src/`, with separate
size budgets (§4.13), and core never calls into toolchain or gen.

Each library ships as a static archive and as a shared library. Shared
library builds export only `writ_*` symbols, through an exported-symbols
list on Mach-O and a version script on ELF.

### 4.2 Lantern symbol isolation

A host may link its own lantern; the n00b toolchain is one such host. A
pinned lantern inside libwrit must not collide with it.

- `libwrit.a` is produced by `tools/isolate.sh`: a **partial link**
  (`-r`) of writ's objects together with lantern's `libir.a`, into a
  single object. Every symbol other than `writ_*` is then made local:
  - on Mach-O, with `clang -r -nostdlib -Wl,-all_load` and
    `-Wl,-exported_symbols_list,exports/writ.exp`;
  - on ELF, with `ld -r --whole-archive` followed by
    `objcopy --keep-global-symbols`.

  The archive contains that one object. `tests/host/isolation.sh` checks
  that `nm` shows only `writ_*` globals, and links and runs a host that
  defines its own `parse_pwz_parse`.
- The shared library gets the same isolation from its export list.
- ICU stays an ordinary external dependency. Lantern's use of ICU goes
  through ICU's versioned symbol names, so it coexists with a host's ICU.

### 4.3 Public API

The headers live in `include/writ/`:

| Header | Covers |
|---|---|
| `writ.h` | Core |
| `model.h` | Model views |
| `clang.h` | `libwrit_clang` |
| `toolchain.h` | The toolchain component of `libwrit` |
| `gen.h` | The generator component of `libwrit` |

`WRIT_API_VERSION` names the API version, and `writ_api_version()` returns
the version of the library actually linked.

**Sessions and sources:**

```c
typedef struct writ_session writ_session_t;
typedef struct writ_unit writ_unit_t;
typedef struct writ_model writ_model_t;

typedef uint32_t writ_status_t;
enum {
    WRIT_OK,
    WRIT_HAS_ERRORS,
    WRIT_INVALID_ARGUMENT,
    WRIT_TOOL_FAILED,
    WRIT_IO_FAILED,
    WRIT_OUT_OF_MEMORY,
};

typedef struct {
    const char *data;
    size_t      length;
} writ_str_t;

typedef uint32_t writ_source_kind_t;
enum {
    WRIT_SOURCE_CONTRACT_FILE,
    WRIT_SOURCE_CONTRACT_DECLARATIONS,
    WRIT_SOURCE_PROFILE,
    WRIT_SOURCE_BINDING,
};

typedef struct {
    writ_str_t host_path;
    uint32_t   first_line;
    uint32_t   first_column;
    uint64_t   first_byte;
} writ_origin_t;

typedef struct {
    uint32_t source_id;
    uint64_t start_byte;
    uint64_t end_byte;
    uint32_t line;
    uint32_t column;
} writ_span_t;

typedef struct {
    writ_status_t (*run)(void *context, const char *const *argv,
                         writ_str_t stdin_text, writ_str_t *stdout_text,
                         writ_str_t *stderr_text, int *exit_code);
    void *context;
} writ_process_runner_t;

typedef struct {
    size_t                struct_size;
    writ_process_runner_t process_runner;
    writ_str_t            llvm_prefix;
} writ_session_config_t;

writ_session_t *writ_session_new(const writ_session_config_t *config);
void            writ_session_free(writ_session_t *session);

writ_unit_t    *writ_unit_new(writ_session_t *session);
writ_status_t   writ_unit_add_source(writ_unit_t *unit, writ_source_kind_t kind,
                                     writ_origin_t origin, writ_str_t text,
                                     uint32_t *source_id);
writ_status_t   writ_unit_add_file(writ_unit_t *unit, writ_str_t path,
                                   uint32_t *source_id);
```

**Embedding.** A host such as n00b passes each writ block it finds in its
own source as a `WRIT_SOURCE_CONTRACT_DECLARATIONS` source:

- The block holds declarations only. Its `writ_origin_t` gives the host
  file and where the block starts.
- Every span writ reports is in **host coordinates**, so the compiler's
  diagnostics point at the right line of the n00b file.
- Library-level directives (`library`, `link`, `header`, `platforms`, ...)
  come from one `WRIT_SOURCE_CONTRACT_FILE` source in the same unit. That
  source may be text the host builds itself.
- Any number of sources make up one contract.

**Resolution and the model:**

```c
typedef struct writ_platform writ_platform_t;

writ_status_t writ_platform_from_triple(writ_session_t *session, writ_str_t triple,
                                        const writ_platform_t **platform);
writ_status_t writ_platform_set_version(writ_platform_t *platform, writ_str_t library,
                                        writ_str_t version);

writ_status_t writ_resolve(writ_unit_t *unit, const writ_platform_t *platform,
                           const writ_model_t **model);
```

The model is immutable and owned by the session. `model.h` exposes it as
**read-only views**: plain structs with counts and arrays, mirroring the
lowered JSON schema one-for-one. Each view starts with `struct_size`, so
fields can be added without breaking hosts compiled against an older
header.

```c
typedef uint32_t writ_provenance_t;
enum {
    WRIT_PROVENANCE_MANUAL,
    WRIT_PROVENANCE_DEFAULT,
    WRIT_PROVENANCE_IMPORT_CLANG,
    WRIT_PROVENANCE_INFERRED,
};

typedef uint32_t writ_mode_t;
enum {
    WRIT_MODE_IN, WRIT_MODE_OUT, WRIT_MODE_INOUT, WRIT_MODE_CONSUME,
    WRIT_MODE_KEEP, WRIT_MODE_MOVED, WRIT_MODE_UNKNOWN,
};

typedef struct {
    size_t                   struct_size;
    writ_str_t               name;
    const struct writ_type_view *type;
    writ_mode_t              mode;
    writ_provenance_t        mode_provenance;
    const struct writ_extent_view *extent;
    const struct writ_span_fact_view *lifetime;
    const struct writ_ownership_view *ownership;
    bool                     requires_identity;
    bool                     observed_live;
    size_t                   overlap_permitted_count;
    const writ_str_t        *overlap_permitted;
    bool                     hidden_in_bindings;
    writ_str_t               derived_from;
    writ_span_t              declared_at;
} writ_slot_view_t;

typedef struct {
    size_t                   struct_size;
    writ_str_t               contract_name;
    writ_str_t               source_name;
    writ_str_t               link_symbol;
    writ_str_t               provider;
    size_t                   slot_count;
    const writ_slot_view_t  *slots;
    const writ_slot_view_t  *result;
    const struct writ_error_view *error;
    const struct writ_effects_view *effects;
    const struct writ_pack_view *packs;
    writ_span_t              declared_at;
} writ_fn_view_t;

size_t                 writ_model_fn_count(const writ_model_t *model);
const writ_fn_view_t  *writ_model_fn_at(const writ_model_t *model, size_t index);
const writ_fn_view_t  *writ_model_fn_named(const writ_model_t *model, writ_str_t name);
const struct writ_call_plan_view *writ_model_fn_plan(const writ_model_t *model,
                                                     const writ_fn_view_t *fn);
```

`writ_model_fn_plan` returns the function's call plan (§4.12): the
physical lowering every generator and host consumes.

Records, handles, vars, interfaces, effects, labels and classes follow the
same pattern: `writ_model_<kind>_count`, `_at` and `_named`.

**Diagnostics:**

```c
typedef uint32_t writ_severity_t;
enum { WRIT_ERROR, WRIT_WARNING, WRIT_NOTE };

typedef struct {
    writ_span_t span;
    writ_str_t  label;
} writ_diag_label_t;

typedef struct {
    writ_span_t span;
    writ_str_t  replacement;
    writ_str_t  description;
} writ_diag_suggestion_t;

typedef struct {
    size_t                        struct_size;
    writ_str_t                    code;
    writ_severity_t               severity;
    writ_str_t                    message;
    writ_diag_label_t             primary;
    size_t                        secondary_count;
    const writ_diag_label_t      *secondary;
    size_t                        note_count;
    const writ_diag_label_t      *notes;
    size_t                        suggestion_count;
    const writ_diag_suggestion_t *suggestions;
} writ_diag_t;

typedef void (*writ_diag_sink_fn)(void *context, const writ_diag_t *diag);

void              writ_session_set_diag_sink(writ_session_t *session,
                                             writ_diag_sink_fn sink, void *context);
size_t            writ_diag_count(const writ_session_t *session);
const writ_diag_t *writ_diag_at(const writ_session_t *session, size_t index);
writ_status_t     writ_diag_render(const writ_session_t *session, const writ_diag_t *diag,
                                   writ_str_t *rendered);
```

**Output.** Anything that produces files (the JSON form, generators, probe
sources, bundles) writes through a sink. The host decides where the bytes
go:

```c
typedef struct {
    writ_status_t (*write_file)(void *context, writ_str_t relative_path,
                                writ_str_t contents);
    void *context;
} writ_output_t;

writ_status_t writ_lower_json(const writ_model_t *model, const writ_output_t *output);
writ_status_t writ_format_unit(const writ_unit_t *unit, const writ_output_t *output);
```

`clang.h`, `toolchain.h` and `gen.h` follow the same conventions:

```c
writ_status_t writ_clang_import(writ_session_t *session, const writ_platform_t *platform,
                                const struct writ_import_request *request,
                                const writ_output_t *output);
writ_status_t writ_clang_verify_layout(const writ_model_t *model);
writ_status_t writ_toolchain_verify_symbols(const writ_model_t *model,
                                            const struct writ_binary_set *binaries);
writ_status_t writ_toolchain_write_bundle(const writ_model_t *const *models, size_t count,
                                          const writ_output_t *output);
writ_status_t writ_toolchain_check_bundle(writ_session_t *session, writ_str_t results_json);
writ_status_t writ_gen_c(const writ_model_t *model, const struct writ_gen_c_request *request,
                         const writ_output_t *output);
writ_status_t writ_gen_rust(const writ_model_t *model, const writ_unit_t *binding,
                            const writ_output_t *output);
```

**Threading.** Different sessions can be used concurrently from different
threads. A single session is used by one thread at a time. Lantern's
grammar object is built per session, and its error state is thread-local.

### 4.4 The API is a C ABI, and writ describes it

The public interface is a plain C ABI that any language can bind. Its own
contract lives at `contracts/writ.writ`, and writ checks itself the same
way it checks any other library.

Rules for the public headers, so the ABI is stable and easy to bind:

- Enumerated values are `uint32_t` typedefs with an anonymous `enum` for
  the constants. The ABI width is then fixed by the typedef, not by each
  compiler's choice of enum size.
- Public structs have no bitfields, and no members whose layout varies
  between compilers.
- Strings cross the boundary as `writ_str_t` (pointer and length). They are
  never NUL-terminated by contract.
- Every callback is paired with a `void *context`, which is the `userdata`
  shape (`dsl.md` §6.3).
- The API has no variadic functions.
- Every view and config struct begins with `struct_size`.
- Fallible functions return a status, and their results come back through
  `out` parameters.

`contracts/writ.writ` states what the headers can't:

- views, diagnostics and the model are borrowed `with session`;
- `writ_session_free` consumes the session;
- output and diagnostic callbacks are `for call`, and the process runner
  is `keep until writ_session_free(session)`;
- `writ_str_t` slots are counted UTF-8 (`*[u8; length] as utf8`);
- errors are `error ret != WRIT_OK`, with details in the session's
  diagnostics;
- every entry point is constrained to `thread same_as writ_session_new`.

Dogfooding starts in slice 1:

- the contract is resolved, and the layout of every public struct and the
  export list are verified against the built libraries, growing with the
  API in every slice;
- the generated conformance tests drive libwrit's own C API;
- once the Rust import generator exists, it produces libwrit's Rust
  bindings (§8).

### 4.5 Repository layout

```
~/writ/
  dsl.md  implementation.md  call-plan.md  n00b-integration.md
  build.sh
  meson.build  meson.options
  deps/
    LANTERN_REVISION  RESHARP_REVISION
    prefix-<platform>/  lantern-build-<platform>/
  grammar/
    writ.bnf
  include/writ/
    writ.h  model.h  clang.h  toolchain.h  gen.h
  contracts/
    writ.writ  resharp.writ
  defs/
    ast_nodes.def  model_fields.def  diag_codes.def  defaults.def  checks.def
  src/
    core/
      session.c  intern.c  loc.c  diag.c  runner.c  output.c  emit.c
      syntax/   parse.c  ast.c  fmt.c
      model/    resolve.c  platform.c  types.c  expr.c  defaults.c  checks.c  views.c  rust.c
      plan/     plan.c  render.c
      lower/    json.c  lower.c
    clang/      tu.c  layout.c  importer.c  attrs.c
    toolchain/  probe.c  exports.c  rustdoc.c  bundle.c
    gen/        profile.c  binding.c  conformance.c  c.c  rust.c  n00b.c
    cli/        main.c
  exports/
    writ.exp  writ.map
  profiles/
    c.writ  rust.writ  n00b.writ
  tools/
    linux.Containerfile  run_bundle.py  isolate.sh
    soft_keywords.py  gen_model.py  keyword_corpus.py  source_text.py
    check_alloc.py  check_containers.py  loc_budget.py  dupes.py
  tests/
    api/  host/  core/  enforce/  run_golden.py  extract_spec.py
    skeleton/ parse/ check/ lower/ plan/ layout/ import/ symbols/ gen/ spec/ corpus/
```

Each `src/` directory is one module, and each file is one responsibility.
There are no `util.c` / `helpers.c` files. A helper lives in the module
that owns its data, and is shared by being exported from that module's
internal header.

### 4.6 Parsing with lantern

- **Grammar.** `grammar/writ.bnf` is embedded at build time with lantern's
  `tools/embed_text.py`. It is loaded with `parse_bnf_declare_terminals(g, text)`
  and then `parse_bnf_load(text, start, g)`.
  - There are two start symbols: `file` (directives and declarations) and
    `declarations` (the embedded form).
  - `writ.bnf` holds rules only. `parse_bnf_load` does not read `@lex-*`
    directives, so the lexer is a `scan_lex_spec_t` built in
    `parse.c` and installed with `scan_grammar_set_lex`.
  - `tools/soft_keywords.py` extracts every `%"kw"` terminal from the
    grammar into the generated `writ_keywords.h`, and the lexer spec
    declares each one soft. No keyword is reserved (`dsl.md` §13): a
    keyword lexes as `IDENTIFIER`, so `len`, `cap`, `type`, `flags`,
    `error` and the rest remain usable as identifiers. The
    `keywords-are-identifiers` test parses a generated file that uses
    every keyword as a parameter name, a type name, a constant name and an
    expression operand.
- **Parser.** `parse_pwz_new_for_start`, `parse_pwz_parse`,
  `parse_pwz_get_tree`, with `parse_grammar_set_error_recovery(g, false)`.
- **Ambiguity.** PWZ resolves ambiguity silently, by picking the last
  alternative. The grammar is unambiguous by construction: every
  declaration starts with a keyword, and there are no optional-everything
  sequences. `@penalty` and `@disambig` are never used. In addition:
  - `parse_grammar_audit_ambiguity` runs as a test and must report
    nothing;
  - debug builds check `parse_forest_is_ambiguous` on every parse.
- **Tree to AST.**
  - `src/core/syntax/parse.c` flattens `PARSE_GROUP_ID` nodes, drops
    punctuation leaves, and copies strings and spans into the session's
    arena, adding the source's origin offsets.
  - It then frees the parser and the token stream.
  - No lantern type appears outside that file.
- **Syntax errors** are reported through lantern's diagnostics (§4.11),
  with the failing token as the primary span.

### 4.7 AST and model internals

- **AST**: one tagged struct per syntactic form, arena-allocated, each node
  carrying a host-coordinate span.
- **Model**: the resolved form for one platform. Every name is bound,
  `when` blocks are evaluated, `refine`s are merged, `use` is inlined, every
  default is applied, and every fact carries provenance. The views in
  `model.h` point directly into the model with no copying.
- **Defaults** live in one table in `defaults.c`, keyed by the `dsl.md`
  section that defines each one.

### 4.8 Facts from libclang, and facts from probes

| Fact | Source |
|---|---|
| Size, alignment, field offset (bits), bitfield width | `clang_Type_getSizeOf`, `clang_Type_getAlignOf`, `clang_Cursor_getOffsetOfField`, `clang_Cursor_isBitField`, `clang_getFieldDeclBitWidth` |
| Nullability | `clang_Type_getNullability`, `clang_Type_getModifiedType` |
| C++ mangling, including ctor/dtor variants | `clang_Cursor_getMangling`, `clang_Cursor_getCXXManglings` |
| Integer constant macros | Detailed preprocessing record; tokenize the body and evaluate simple expressions; anything else becomes `unknown` |
| Deleted, defaulted, virtual, abstract | `clang_CXXMethod_isDeleted`, `isDefaulted`, `isVirtual`, `isPureVirtual`, `clang_CXXRecord_isAbstract` |
| `counted_by`, `lifetimebound`, `ownership_*`, `swift_attr` | Surfaced as `CXCursor_UnexposedAttr`. `attrs.c` tokenizes the attribute extent. |
| Trivially copyable, trivially relocatable | Compiled probe: `static_assert(std::is_trivially_copyable_v<T>)` per class, per platform |
| Layout as the platform's real compiler does it | Compiled probe (§4.9) |
| How each argument and result is passed (`abi_class`: registers, memory, `sret`, `byval`, coerced types) | A probe function per signature, compiled with `clang -S -emit-llvm -target <triple>`. The lowered LLVM signature is read back. |

### 4.9 Verifying each platform

**Layout is checked twice:**

1. libclang values against the contract's assertions.
2. A generated probe compiled with the platform's real compiler: `clang`
   on Mach-O and Linux, `cl.exe` on Windows. The probe contains
   `_Static_assert`s on `offsetof`, `sizeof` and `_Alignof`, plus a
   bitfield probe that writes known bit patterns through each field and
   checks the bytes.

If the two disagree, writ reports a toolchain mismatch, separately from
contract errors.

**Symbols** come from `llvm-nm` / `llvm-readobj`, which read Mach-O, ELF and
PE/COFF from any host.

**Generated bindings** are built and run against real libraries.

How each platform runs:

- **Mach-O**: native.
- **Linux**: `./build.sh test --linux` runs the whole build and test suite
  inside the `tools/linux.Containerfile` image. The lantern workspace is
  mounted read-only, and output goes to `build-linux/`.
- **Windows**: a **verification bundle**, produced by
  `writ_toolchain_write_bundle` (`writ bundle`). It contains:
  - the probe sources;
  - the fixture libraries' sources and build scripts for `cl.exe` /
    `link.exe`;
  - the generated Rust crates and their test programs;
  - a manifest of expected results;
  - `run_bundle.py`.

  Run `python run_bundle.py` from a VS developer prompt on the Windows
  machine. It writes `results.json`. Then `writ_toolchain_check_bundle`
  (`writ bundle-check`) on the Mac reports the results like a local
  verify. The bundle needs no network access and nothing from this repo.

A platform listed in a contract without a completed run is reported as
"not verified", never as passing.

### 4.10 Windows-specific facts

- **Data symbols exported from DLLs** are reached through `__imp_<name>`
  pointers. A DLL-provided `var` is accessed through that indirection, in
  generated Rust and in probes.
- **Import libraries**: `link` maps to `<name>.lib` for linking and
  `<name>.dll` at run time.
- **Symbol decoration**: x64 C symbols are undecorated. 32-bit
  `stdcall`/`fastcall` decoration is out of scope.
- **MSVC C++ mangling** comes from `clang_Cursor_getMangling` with
  `-target x86_64-pc-windows-msvc`. It is checked against the DLL's
  exports and `llvm-cxxfilt`'s Microsoft demangler.
- **`va_list`** is a `char *`. `va_list` shims are compiled and run in the
  bundle.
- **Layout**: MSVC bitfield allocation, `#pragma pack`, 32-bit `long` and
  16-bit `wchar_t` all have fixtures.
- **Text**: `cstr(utf16)` and `A`/`W` symbol pairs have a fixture DLL that
  exports both.

### 4.11 Diagnostics

libwrit reports through lantern's diagnostic API (`ir/diag.h`,
`lib/diagnostic.h`):

- every diagnostic has a primary label, secondary labels and notes;
- **suggestions** (`ir_diag_suggest`) carry a span and replacement text,
  which drive fix-its (`dsl.md` §10.3);
- severity policies (`ir_policy_t`) let a host promote or silence codes;
- rendering is available as text and JSON (`ir_diags_render_text`,
  `ir_diags_render_json`).

Lantern stays hidden inside libwrit (§4.2), so the public API exposes
diagnostics as plain `writ_diag_t` values. Each one mirrors lantern's
diagnostic: code, severity, message, primary and secondary labels with
host-coordinate spans, notes and suggestions. `writ_diag_render` returns
lantern's text rendering. A host such as n00bb copies each `writ_diag_t`
into its own diagnostics. Lantern objects never cross between two lantern
copies.

writ's diagnostic codes are kept in a `.def` table in the same form as
lantern's `ir_diags.def`. Slice 0 settles how that table registers with
lantern's diagnostic code space.

### 4.12 Internal structure: one of everything

libwrit has exactly one implementation of each piece of infrastructure.
For containers and memory, that implementation is lantern's, which libwrit
already links privately. writ code never defines its own growable array,
hash map, string builder, arena or string type.

| Need | The one implementation |
|---|---|
| Growable sequence | `core_list_t(T)` |
| Fixed-size array | `core_array_t(T)` |
| Hash map | `core_dict_t`, keyed by interned id or by pointer |
| Interning, dense ids in first-seen order | `core_indexed_table_t` |
| Text and byte building | `core_buffer_t` |
| Strings | `core_string_t` inside libwrit, `writ_str_t` at the API |
| Bulk memory | One `core_arena_t` per session |
| Hashing | `core_hash_raw` (xxh3) |
| Optional values, fallible results | `core_option_t`, `core_result_t` |
| Expressions | `src/core/model/expr.c` (below) |
| Generated text in any language | `src/core/emit.c` (below) |
| Physical lowering of a call | `src/core/plan/plan.c` (below) |
| External processes | The session's process runner |
| JSON output | `src/core/lower/json.c` |
| JSON input (rustdoc, bundle results) | Lantern's JSON grammar (`grammars/json.bnf`), if Slice 2's benchmark on resharp's rustdoc output is fast enough. Otherwise a single reader in `toolchain/rustdoc.c`. |

Two checks enforce this, and both run in `./build.sh test`:

- **`tools/check_alloc.py`** fails if any file other than
  `core/session.c` calls `malloc`, `calloc`, `realloc` or `free`.
  Everything else allocates from the session arena or through lantern's
  containers.
- **`tools/check_containers.py`** fails on any struct in `src/` that pairs
  a pointer member with `len`, `cap`, `count` or `size` members. The only
  exception is the public `*_view_t` structs, which are read-only windows
  over arena data.

#### Identity and memory layout

**Interned names.** Every identifier and string literal in a session is
interned once, in the session's string table (a `core_indexed_table_t`).
Code passes `writ_sym_t` (a `uint32_t`) everywhere. Comparing names is an
integer comparison, and maps are keyed by symbol.

**Locations without line numbers.** A location is three integers:

```c
typedef struct {
    uint32_t source;
    uint32_t start;
    uint32_t end;
} writ_loc_t;
```

Each source keeps one sorted list of line-start offsets. Line and column
are computed by binary search only when a diagnostic or a view asks for
them, so nodes never store them.

**Flat AST.** Every node of a unit lives in one contiguous
`core_list_t(writ_node_t)`:

```c
typedef struct {
    uint16_t   kind;
    uint16_t   flags;
    writ_loc_t loc;
    uint32_t   first_child;
    uint32_t   child_count;
    writ_sym_t name;
    uint32_t   payload;
} writ_node_t;
```

- Children are a range in the unit's `core_list_t(uint32_t)` child-index
  list. Nothing points to anything, and there are no parent pointers.
- There is **one** traversal, `writ_ast_walk`, which passes the path from
  the root to the visitor.
- The typed per-kind accessors (`writ_fn_params(unit, node)`) are generated
  from `defs/ast_nodes.def`. They are the only way code reads node
  structure.

**Interned types.** Types are hash-consed: each distinct type (`*[u8; n]`,
`?Regex`, `utf8[len]`) is built once per session in a
`core_indexed_table_t` keyed by its structure, and referred to by
`writ_type_id`. Type equality is id equality.

**Model as dense tables.** The resolved model is one table per kind
(functions, slots, records, handles, error channels, conventions,
symbols), each a `core_list_t` of fixed-size records, referenced by
`uint32_t` ids. Each fact is stored inline in its record, with its
provenance packed into a bit field.

- Name lookup at library scope is one `core_dict_t` from symbol to
  declaration id.
- Lookup within a parameter or field list is a linear scan. These lists
  are short, and a scan over a contiguous array is faster than hashing.

**Views built once.** After resolution, `views.c` lays out every public
`*_view_t` in contiguous arena arrays, in one pass over the model tables.
Views never change after that, so view pointers are stable for the life
of the session.

#### Shared engines

**One expression engine** (`expr.c`). Extents, `when` conditions, `error`
predicates, `via` expressions, `sizing`, `none` values and `expands`
arguments are all the same expression nodes. They share:

- one checker;
- one evaluator, used for constant folding and conformance-test values;
- one printer, driven by a per-language operator table, which writes an
  expression in C, Rust or n00b.

Convention patterns use one glob matcher, in the same file.

**One call plan** (`plan.c`, specified in `call-plan.md`). For each
function, platform and set of binding options, `writ_plan_build` computes
a `writ_call_plan_t`. It holds:

- logical items, physical slots and locals;
- outcomes;
- fixed phases (prepare, attempt, classify, one block per outcome,
  finally) with five named edges;
- a closed set of eighteen steps, each with a mirror for the export
  direction.

The plan is part of the public model (`writ_model_fn_plan`). Every
consumer reads the plan: the C, Rust and n00b generators, the conformance
generator, n00bb, and the run-time `ffi` module. **Generators print plans;
they never re-derive shapes from the contract.**

**One emitter** (`emit.c`). `writ_emit_t` writes indented lines and blocks
into a `core_buffer_t`, and prints expressions through the per-language
operator table. Every generator, the formatter and the probe writer use
it.

#### Single-source tables

Anything that would otherwise be written once per node kind, field or
code is listed once in a `.def` table and expanded where needed:

| Table | Expanded into |
|---|---|
| `defs/ast_nodes.def` | Node kinds, typed accessors, formatter dispatch, tree dump |
| `defs/model_fields.def` | Model records, the public view structs, the JSON writer, the JSON schema, the view-compatibility test |
| `defs/diag_codes.def` | Codes, severities, default messages, `diag-codes.md` |
| `defs/defaults.def` | The defaults table, each entry citing its `dsl.md` section |
| `defs/checks.def` | Every check, with its tier and `dsl.md` section |

`include/writ/model.h` is generated from `model_fields.def` and checked
in. The build fails if the checked-in header is stale.

### 4.13 Size budget and duplication

Each component has a size budget, counted in non-blank lines of
hand-written C (no tests, no generated files):

| Component | Budget |
|---|---|
| `core/` excluding `syntax`, `model`, `plan`, `lower`: session, interning, locations, diagnostics, runner, output, emitter | 1,500 |
| `core/syntax`: lantern adapter, AST, formatter | 2,500 |
| `core/model`: resolution, platforms, types, expressions, defaults, checks, views | 5,000 |
| `core/plan` | 1,500 |
| `core/lower` | 800 |
| `clang/` | 3,000 |
| `toolchain/` | 3,000 |
| `gen/`: profiles and bindings, conformance, C, Rust transport, n00b | 5,500 |
| `cli/` | 600 |
| **Total** | **23,400** |

- `tools/loc_budget.py` prints every component's size against its budget
  in `./build.sh test`. A component more than 20% over budget fails the
  build until this table is changed, with the reason.
- `tools/dupes.py` compares token shingles across `src/` and fails on any
  two regions that are near-copies above a fixed length. Duplication is
  fixed by moving the code to the module that owns its data, never by
  raising the threshold.

---

## 5. Slices

Each slice ends with a real library called through generated code, so a
wrong idea in the language surfaces in the slice that introduced it. Within
a slice, every piece lands its **API first**, with C tests in `tests/api/`
that use only public headers, and then its thin CLI subcommand.

The grammar, checks and generators grow slice by slice. A construct enters
when its slice's library needs it, and with it come its grammar rule, its
checks, its model view, its JSON form and its tests. Spec examples are
tagged with the tier they belong to (`writ` for core, `writ-extended`,
`writ-fragment`), and only core examples are required to parse.

### Slice 0 — Walking skeleton

**Goal.** A hand-written `tests/skeleton/zlib.writ` describing
`compress2`, `compressBound` and `uncompress` goes through:

parse → model → call plan → generated C shim and conformance test

The conformance test round-trips data on the Mac.

This proves the spine every later slice extends, at its thinnest:

- the grammar, and the lantern adapter;
- the flat AST;
- the model tables;
- the call plan;
- the emitter;
- the generators.

The call plan is specified in `call-plan.md`. Everything else in this
slice follows from it.

**Setup:**

- `jj git init ~/writ`, with `.gitignore` covering `deps/prefix*/`,
  `deps/lantern-build*/`, `build*/` and `bundle*/`.
- Create the `~/lantern-writ` workspace and `deps/LANTERN_REVISION`
  (§3.1).
- `build.sh`: check the pin, build lantern into its prefix, run Meson and
  Ninja. It takes `test` and `--sanitize`.
- Meson targets: `libwrit` (static), with the partial-link and
  symbol-localization step (§4.2), and the `writ` CLI.

**Constructs:**

- `library`, `link`, `header` and `const`;
- `fn`;
- `*` / `?` pointers, and `*[T; n]` with `cap` / `len`;
- `in`, `out` and `inout`;
- `error ... via`;
- `sizing needs`, with `pure` functions in its expression.

Nothing else is accepted yet.

**Work, module by module.** Each module lands only what these constructs
need. Later slices extend the same files.

| Module | Lands |
|---|---|
| `core/session.c`, `intern.c`, `loc.c`, `diag.c`, `output.c` | The session and its arena, interning, locations with line tables, diagnostics on lantern's machinery, the output sink |
| `core/emit.c` | `writ_emit_t`, with the C row of the operator table |
| `grammar/writ.bnf`, `core/syntax/parse.c` | The grammar for the constructs above, with every keyword soft; `writ_unit_add_source` / `writ_unit_add_file` |
| `defs/ast_nodes.def`, `core/syntax/ast.c` | The node kinds, generated accessors, `writ_ast_walk` |
| `core/model/types.c`, `expr.c`, `resolve.c`, `defaults.c`, `checks.c`, `views.c`, `defs/*.def` | The type table, expressions, resolution, defaults, checks and views for the constructs above; `writ_resolve` and the generated `model.h` |
| `core/plan/plan.c` | `writ_plan_build` and `writ_model_fn_plan`, as specified in `call-plan.md` |
| `gen/c.c`, `gen/conformance.c` | A C header, pointer-normalized shims and conformance tests, printed from plans |
| `cli/main.c` | `parse`, `check` and `gen c` |

**The two measurements** most likely to change the design, recorded in
`implementation.md`:

1. Lantern's soft keywords with the PWZ parser on the real grammar, with
   `len`, `cap`, `type` and `flags` as parameter names.
2. Partial-link symbol isolation: `nm` on `libwrit.a` shows only `writ_*`
   globals, and a host program that defines its own `parse_pwz_parse`
   links against it and runs.

Both hold on Mach-O. Soft keywords hold for all keywords of the grammar
(`keywords-are-identifiers`), and isolation holds with the toolchain
described in §4.2 (`host-isolation`). The skeleton is 3,400 lines of
hand-written C, within its pro-rata share of every §4.13 budget.

**Enforcement from the first line of code:** `check_alloc.py`,
`check_containers.py`, `loc_budget.py` and `dupes.py` (§4.12–§4.13).

**Types introduced:** `writ_session_t`, `writ_sym_t`, `writ_loc_t`,
`writ_diag_t` (and its label and suggestion structs), `writ_output_t`,
`writ_emit_t`, `writ_unit_t`, `writ_node_t`, `writ_type_id`,
`writ_model_t`, the generated model and view types, and the call-plan
types in `call-plan.md`.

**Done when:**

- `writ gen c tests/skeleton/zlib.writ` emits a header, shims and a
  conformance test. The test round-trips data through
  `compress2` / `uncompress`, sized with `compressBound`, under ASan and
  UBSan.
- Golden call plans for the three functions match (`tests/plan/`).
- A contract with a type error reports it at the right line and column,
  with the right code.
- Both measurements are recorded, and both pass.
- The enforcement tools pass, and each fails on a planted violation.
- The skeleton's size is within the §4.13 budget's pro-rata share. If it
  isn't, the plan is revisited before Slice 1.

### Slice 1 — zlib, complete, on Mach-O and Linux

**Target**: the rest of zlib:

- checksums (`crc32`, `adler32`);
- `zlibVersion`;
- the streaming API (`deflateInit`/`deflate`/`deflateEnd`,
  `inflateInit`/`inflate`/`inflateEnd` on `z_stream`), whose `inflate`
  is the first function with several outcomes.

The zlib contract is now imported from `zlib.h` rather than written by
hand, and it is verified.

A small **libc variadic set**, since zlib has none:

- `snprintf` (`format printf`, `sizing query`);
- `vsnprintf` (`va_list`);
- `execl` (`... of cstr until null`);
- `open` (an optional trailing mode argument);
- `strtol` (an error channel with `reset before call`).

**Core-tier constructs added:**

| Area | Constructs |
|---|---|
| Header directives | `define`, `platforms`, `when` on platform |
| Declarations | `type`, `opaque`, `struct` (`layout c`), `refine` |
| Types | `cstr`, `va_list` |
| Modes and ownership | `consume`, `own ... release` |
| Lifetimes | Spans: `for call`, `static`, `with`, `until` |
| Errors | `outcome`, `error_channel` with `reset before call` |
| Sizing | `sizing query` |
| Variadics | `...` with `of`/`until`/`count`, `format printf`, `va_list` |
| Authoring | `unknown`, `conventions`, `expands` |

**Work, module by module.** Every item extends a Slice 0 module, or adds
one of the modules reserved for it in §4.5.

| Module | Adds |
|---|---|
| `grammar/writ.bnf`, `core/syntax/*`, `core/model/*` | The constructs above: grammar, nodes, types, resolution (`refine` merging under the narrow-only rule, convention matching, `expands`), defaults, checks, views |
| `core/syntax/fmt.c` | `writ_format_unit` (`writ fmt`) |
| `core/model/platform.c` | `writ_platform_from_triple` for the Mach-O and Linux triples; `when` evaluation |
| `core/plan/plan.c` | Handles, `cstr`, spans and ownership actions, `sizing query`, outcomes with `classify`, `expand` for every element rule, plan-wide `reset` |
| `core/runner.c` | The default process runner and the replaceable interface |
| `core/lower/json.c`, `lower.c` | `writ_lower_json`, generated from `model_fields.def` |
| `clang/tu.c`, `layout.c` | Translation units per platform; `writ_clang_verify_layout` |
| `clang/importer.c`, `attrs.c` | `writ_clang_import`, written out through `fmt.c` |
| `toolchain/probe.c` | Layout probes and `abi_class` probes (LLVM IR), compiled through the runner |
| `toolchain/exports.c` | `writ_toolchain_verify_symbols` for Mach-O and ELF |
| `gen/c.c`, `gen/conformance.c` | Probe sources; one shim per argument count used for variadic calls; `va_list` wrappers; conformance scenarios for the new shapes (released handles, streaming state, every `inflate` outcome, each variadic element rule) |
| `cli/main.c` | `fmt`, `lower`, `import`, `verify` |
| Build | `libwrit_clang`, shared libraries with export lists, pkg-config files, `build.sh --linux`, `tools/linux.Containerfile` |

**Types introduced:** `writ_platform_t`, `writ_process_runner_t`, plus
generated model fields.

`contracts/writ.writ` starts here, covering the API as it exists. It grows
with every slice, and its layout and symbols are verified against the
built libwrit.

**Tests:**

- core spec examples parse, format round-trip, and resolve;
- the ambiguity audit is empty;
- every keyword parses as a parameter name, a field name and a type name,
  in a generated test file that uses each one in all three positions;
- a libFuzzer target on `writ_unit_add_source` runs under ASan and UBSan;
- `host/embedded` checks spans in host coordinates;
- `host/headers`: public headers compile as C11 and C++17;
- the zlib import matches its golden draft, and the refined contract
  resolves with no `unknown`s;
- golden call plans for every zlib function;
- the conformance tests round-trip and stream through the shims under
  ASan and UBSan, on Mach-O and in the Linux container;
- the libc variadic set runs through generated shims: `snprintf` sized by
  query, `vsnprintf` through its wrapper, `execl` with a sentinel-ended
  list, `open` with and without its mode argument, and `strtol` reporting
  overflow only after `errno` was reset;
- `z_stream` layout and every zlib symbol verify on both platforms;
- `contracts/writ.writ` verifies against the built libwrit.

### Slice 2 — resharp, through its Rust API

**Spec pause.** The slice starts by defining the profile and binding
formats (`dsl.md` §9.2–§9.3) and the `'ffi_spec` form
(`n00b-integration.md` §5.1), with sign-off before any generator code.

**Target**: resharp's public Rust API (`Regex`, `RegexOptions`, `Match`,
`UnicodeMode`, and the feature-gated capture API), described as a
`lang rust` contract (`dsl.md` §8.9). The resharp revision under test is
pinned in `deps/RESHARP_REVISION`, and fetched with `jj git clone` into
`~/resharp-writ`, outside the writ tree.

**Constructs introduced:**

- `lang rust`, `crate`, `impl` blocks and Rust types (`dsl.md` §8.9);
- error channels (§6.4);
- the empty-array rules and `bytes`/`utf8` shorthand (§6.1);
- `none` values and success-domain views (§4.6);
- build features (§4.7);
- handle thread rules (§4.3);
- fix-its (§10.3).

**Work, module by module.** Every item extends a Slice 1 module, and the
slice adds no new infrastructure.

| Module | Adds |
|---|---|
| `grammar/writ.bnf` | `lang rust`, `crate`, `impl`, Rust types, `error ... display`, `error_channel`, `feature`, `null if empty`, `as *U`, `sizing retry`, `none`, `shared` / `thread_affine`, `bytes` / `utf8` |
| `core/model/types.c` | Rust types as interned types; `bytes` and `utf8` as spellings of existing types (no new type kinds) |
| `core/model/resolve.c` | `impl` scopes, error channels, features, handle thread rules |
| `core/model/checks.c` | Element reinterpretation sizes, success-domain views, `none` value types, channel use |
| `core/model/rust.c` | **Derivation of the transport's C contract from the Rust contract**, as an ordinary model built through the existing tables. This is the only Rust-specific model code. |
| `core/plan/plan.c` | `null if empty`, reinterpreted elements, `sizing retry`, channel fetches, `none` checks |
| `toolchain/rustdoc.c` | `writ_toolchain_import_rust`: rustdoc JSON to a `lang rust` contract, written out through `fmt.c` |
| `gen/profile.c`, `gen/binding.c` | Profile and binding resolution |
| `gen/rust.c` | **The Rust transport**: the `resharp-writ` crate, printed from the plans of the derived C contract. It has `extern "C"` functions with pointer-and-length strings and slices (non-null when empty), handles with generated release functions, a per-thread error channel carrying `Display` text, and `catch_unwind` at every entry point. |
| `gen/c.c` | `resharp.h`, from the derived contract's plans, unchanged from Slice 1 |
| `gen/conformance.c` | The new shapes; the existing scenarios rerun against the transport |

**Types introduced:** none outside generated model fields. Rust
declarations are model records like any other.

**Tests:**

- the import of the pinned revision matches its golden contract;
- the transport crate builds against the pinned revision with stable
  Rust;
- a differential corpus of patterns and inputs gives identical results
  through the transport and through `resharp::Regex` directly;
- Miri runs the transport's `extern "C"` functions from Rust tests,
  including empty inputs and zero-capacity outputs;
- the conformance tests drive the transport's C ABI from C under ASan and
  UBSan;
- symbols in the built library match the derived C contract, with the
  feature-gated functions present exactly when the feature is on.

### Slice 3 — n00b imports zlib and resharp

The writ side of `n00b-integration.md`:

| Module | Adds |
|---|---|
| `profiles/n00b.writ` | The n00b profile |
| `gen/n00b.c` | `writ_gen_n00b`: n00b declarations printed from plans, plus link requirements |
| `gen/c.c` | Shims and trampolines emitted for bitcode compilation; the same printer as Slice 1 |
| `core/model/expr.c` | The n00b row of the operator table |

n00bb reads the same call plans through `writ_model_fn_plan`, so the
compiler never re-derives shapes. n00bb's phases N1–N4 run with zlib and
resharp as the test libraries.

**Done when** an n00b program compresses data with zlib and matches text
with resharp, on Mach-O and Linux.

### Slice 4 onward — The n00b runtime libraries

Each remaining library the default n00b runtime links is its own slice:

1. a contract;
2. verification;
3. conformance tests;
4. an n00b binding.

Extended-tier constructs move into the core tier only when one of these
libraries needs them.

### Final slice — Windows

Everything built in the earlier slices runs on `x86_64-pc-windows-msvc`:

- the verification bundle (§4.9): `writ_toolchain_write_bundle`,
  `writ_toolchain_check_bundle`, `tools/run_bundle.py`;
- the Windows-specific facts in §4.10, each with its fixture;
- public headers compiled with `cl.exe`;
- every library's conformance, layout and symbol tests;
- the n00b bindings.

**Done when** every earlier slice's tests pass on Windows through the
bundle.

---

## 6. Testing summary

| Layer | Method |
|---|---|
| API | C tests in `tests/api/` against public headers only |
| Embedding | `tests/host/`: span mapping, symbol isolation, headers as C11/C++/MSVC, view-size compatibility |
| Grammar | Core-tier spec examples, ambiguity audit, fuzzing |
| AST | Round trip and idempotent formatting |
| Model and checks | Pass/fail cases per check, per platform |
| Lowered form | Golden JSON, determinism, schema validation, agreement with the model views |
| Layout | libclang plus compiled probes on each platform's real compiler |
| Argument passing | `abi_class` from LLVM IR probes, checked per platform |
| Symbols | Fixture libraries per object format, plus each target library |
| Generated bindings | ABI conformance tests generated from each contract, run against every generator's output |
| Rust transport | Differential tests against the library's own API; Miri on the generated `extern "C"` layer |
| C code | ASan and UBSan (`./build.sh test --sanitize`) |

---

## 7. Risks

| Risk | Mitigation |
|---|---|
| Lantern behavior changes | Pinned jj workspace; every lantern call is in `parse.c`; moving the pin requires every supported platform to pass |
| Symbol clashes between writ's lantern and a host's lantern | Partial link plus localization, a shared-library export list, and the `host/isolation` test |
| Silent ambiguity resolution in PWZ | Unambiguous grammar; audit test; forest check in debug builds |
| A construct designed in `dsl.md` turns out wrong in practice | Constructs are implemented only when a slice's library needs them, and each slice ends in a working call |
| resharp's API moves | `deps/RESHARP_REVISION` pins the revision under test; moving it reruns slice 2 |
| libclang gaps | Token-level attribute reading; compiled probes |
| Code growth: duplicate infrastructure, per-generator reimplementation, list-everything data structures | One implementation of each piece (§4.12), enforced by the allocation and container checks; generators print shared call plans; size budgets and the duplication check (§4.13); every slice lists the types it introduces |
| View structs growing over time | `struct_size` on every view, plus the `api/view_compat` test |
| Windows arrives last, after the design has settled on two platforms | Windows-specific facts (§4.10) are designed now, and contracts list Windows from the start. Until the final slice it is reported as "not verified". |
| x86_64 Linux under emulation is slow | Full mode only; aarch64 Linux runs every time |

---

## 8. After v1

1. The Rust import generator, and `writ-sys` / `writ-build` generated from
   `contracts/writ.writ`.
2. libwrit built natively on Windows. That depends on lantern building
   there.
3. Incremental re-resolution for hosts that edit one embedded block at a
   time.
4. C++ shim generator (`dsl.md` §8.1 stage 0), `implementable` subclass
   shims, then direct C++ linking stages 1–4.
5. Generators and profiles for Python, Java (Panama), Go, Swift and C#.
6. Build integrations: CMake, Meson module, Bazel, Python build backends.
7. `abi jvm` and `abi cpython`.
