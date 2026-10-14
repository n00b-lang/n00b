# The `'ffi_spec` Language

An `'ffi_spec` fence holds an n00b binding: n00b's choices for one writ
contract, in one direction. This document defines what it may say, what
each choice means for the generated n00b code, and what is reported when
a binding contradicts its contract. It is the input for
`n00b-integration.md` phases W1 and N3–N6.

---

## 1. Shape

An `'ffi_spec` fence is a writ binding file (`dsl.md` §9.3) whose target
language is `n00b`:

```writ
binding zlib for n00b import {
    module "zlib"
    name compress2 "compress"
}
```

- There is no n00b-only syntax. Everything n00b-specific lives in two
  places:
  - the **n00b profile** (`profiles/n00b.writ`), whose representation IDs
    (`string`, `buffer`, `list`, `record`, …) a binding chooses among;
  - the **n00b generator** (`writ_gen_n00b`), which gives those IDs their
    meaning in n00b code.
- A fence holds exactly one `binding` block, and it must say `for n00b`.
  Anything else is E0510 at the fence.
- `ffi_bind(contract, binding)` checks that the binding's contract name is
  the contract's `library` name (E0511 otherwise).
- Every binding item, its targets, and diagnostics E0501–E0509 are as in
  `dsl.md` §9.3. This document adds n00b's profile, its defaults, and how
  each choice appears in n00b.

---

## 2. Surface mapping

### 2.1 Modules and names

| Contract | n00b | Default |
|---|---|---|
| The contract | One module | `module` defaults to the `library` name |
| `fn f` | `func f` in the module | Name after `strip_prefix`, in `snake` case |
| `handle H`, `opaque` crossing as a handle | `type H` holding the C value | `pascal` case |
| `struct S` | `type S layout(c) { ... }` | `pascal` case |
| `enum E`, `flags F` | n00b `enum` / flag set with the same values | `pascal` case; members `upper` |
| `const C` | `const C` | `upper` case |
| `callback C` | n00b `callback` type | `pascal` case |

- Names that collide with n00b's reserved words get a trailing `_`.
- Two contract declarations that end up with one n00b name in one module
  are E0505, reported at the later declaration's binding item, or at the
  `strip_prefix` that caused it.
- A `lang rust` contract names impl items as `T::m`. `name`, `method`,
  `hide` and the other items accept `T::m` wherever they accept a
  function name.

### 2.2 Methods on handle types

- `method H.m = f` makes `f` a method of `H`. The receiver is `f`'s first
  parameter of type `H`. `static H.m = f` makes `f` an associated function
  `H.m(...)`.
- In a `lang rust` contract, every impl item is grouped by default: items
  taking `self`, `&self` or `&mut self` become methods, and the rest
  become associated functions.
- A handle's `release` function becomes the method `close()`, and is not
  also bound as a module function.
- C contracts are not grouped by default. Grouping is always written out.

### 2.3 Hiding and renaming

- `hide "glob"` removes declarations from the bound API. Hidden
  declarations still exist in the contract, so a binding can hide a
  function another function's plan calls (an error-detail function, say).
- `raw "glob"` binds a declaration whose facts include `unknown`. It is
  bound with raw pointers (`ptr`) for every slot the contract can't
  describe, and its n00b name gets the suffix `_raw`.
- A declaration that reaches an `unknown` fact and is neither hidden nor
  raw is E0508, and the compile fails.

---

## 3. Type choices

`profiles/n00b.writ`:

```writ
profile n00b {
    memory gc nonmoving
    pin for call via "n00b_ffi_borrow" release "n00b_ffi_release"
    pin longer   via "n00b_ffi_borrow" release "n00b_ffi_release"
    prim i8 -> "i8"
    prim i16 -> "i16"
    prim i32 -> "i32"
    prim i64 -> "i64"
    prim u8 -> "u8"
    prim u16 -> "u16"
    prim u32 -> "u32"
    prim u64 -> "u64"
    prim f32 -> "f32"
    prim f64 -> "f64"
    prim bool -> "bool"
    string string to cstr    via copy copies
    string string to utf8[n] via copy copies
    buffer buffer to bytes[n] via "Array[u8]" copies
    buffer borrow to bytes[n] via "n00b_ffi_borrow" zero_copy
    pack rest list native "List[T]"
    pack rest array native "Array[T]"
    pack keywords record native "options record"
    variadic call yes
    variadic define no
    error style result
    callback style closure
    handle close "close"
    lifetimes no
    naming fn snake
    naming type pascal
    naming const upper
    naming field snake
    naming module snake
    reserved "func", "type", "const", "return", "fail", "callback", "trait", "enum"
}
```

### 3.1 Choices, per contract construct

| Contract construct | n00b choices (profile ID) | Default |
|---|---|---|
| `in *cstr`, `in utf8[n]` | `string` (copied to a NUL-terminated or counted buffer), `buffer` | `string` |
| `in *[u8; n]`, `bytes[n]` | `buffer` (copied), `borrow` (the n00b array's storage, for the span) | `buffer` |
| `out` arrays and returned owned buffers | `buffer` (a new `Array[u8]`), `string` (decoded as UTF-8; invalid text is an error) | `buffer`; `string` when the contract says `utf8` or `cstr` |
| Rest packs | `list` (`List[T]`), `array` (`Array[T]`) | `list` |
| Keyword packs | `record`: a generated `type F_Options` with one optional field per key | `record` |
| `out` / `inout` scalars | Extra results | Promoted (§3.2) |
| Labels | `label path -> "string"`, `label secret -> "string"` | The profile's |
| `?T` | `T?` | — |
| `bool`, fixed-width integers, `c_*` | The n00b scalar of the platform's width | — |

The choices are made with `string`, `buffer`, `pack` and `keywords`
items. A choice whose contract form doesn't match the slot is E0502:
`string compress2.source as string` is rejected because `source` is a
`*[u8; n]`, not text.

### 3.2 Results

- A function's n00b result is its return value, followed by its `out` and
  `inout` slots in parameter order.
- A count or length that the plan consumes (an array's `len`, a
  `cap ... len` cell) is not a result. It becomes the length of the
  returned array.
- One result is returned directly. Several are returned as a generated
  record `F_Result`, with one field per result, named after the slot.
- A return value that only carries the error status (`error ret != OK`
  with no other use of `ret`) is not a result.

`compress2` therefore binds as:

```
func compress(source: Array[u8], level: i32) -> Array[u8] fail: zlib.Error
```

---

## 4. Errors

- A function with `error` or `failure` outcomes returns `T fail: E`.
- `E` is the binding's `error PATTERN -> "Type"` choice. The default is a
  generated `type Error` in the module:

  ```
  type Error {
      code:   i64        # the `via` value, or the status itself
      text:   string?    # the error channel's text, when there is one
      fn:     string     # the contract function that failed
  }
  ```

- Error detail comes from the contract, never from the binding:
  - `via expr` fills `code`, and `text` when the expression is text
    (`via kv_errstr(ret)`);
  - a `text` error channel (`dsl.md` §6.4) fills `text`;
  - `errno`-style channels fill `code`.
- `error PATTERN -> "Type"` may name an n00b type with a constructor
  `Type.from_ffi(code: i64, text: string?, fn: string) -> Type`.
  The generator calls it, and checks it exists when the generated source
  is typed.
- Outcomes that are not failures (`outcome not_found success`) are not
  errors. A function with more than one success outcome returns an n00b
  enum of outcomes as an extra first result.

---

## 5. Ownership choices

| Contract | n00b |
|---|---|
| `for call` borrow | `n00b_ffi_borrow` before the call, `n00b_ffi_release` after |
| `keep until EVENT` | The borrow is held in the handle wrapper of the event's subject, and released when the wrapper performs or observes `EVENT` |
| `with x` (returned) | The result keeps a reference to `x`'s wrapper, so `x` stays alive while it is |
| `until next f(...)` / `until release(...)` borrowed results | Copied before the next call (`borrow ... as copy`, the default and the only choice, since `lifetimes no`) |
| `own ... release g` | Wrapped in a handle type whose `close()` calls `g` |
| Policy parameters | `policy f.p use ARM` fixes the arm and removes `p`. Without one, `p` becomes an enum parameter of the n00b function, limited to the arms the contract declares. |
| `consume` | The n00b wrapper is marked closed after the call; using it again fails with `Error` |

- **Handles close explicitly.** `close()` is the release. A handle that
  is collected while still open is reported in debug builds and released
  then. Finalizers are not part of the binding; when n00b's collector
  gains them, the profile changes and bindings don't.
- `borrow TARGET as lifetime` is E0504 for n00b.

---

## 6. Callbacks

- A `callback` type becomes an n00b `callback` type named by
  `callback C as "Name"`, defaulting to the contract name in `pascal`
  case. Its signature is the callback's, mapped as in §3.
- **With `userdata`**, the n00b callback value is stored in the runtime's
  handle table, and its index is passed as the `userdata`. The table entry
  lives exactly as long as the contract's span for the callback slot, and
  is freed at the span's event.
- **Without `userdata`**, the callback slot takes a named n00b function,
  not a callback value. The binding names it with
  `provide f.cb = "module.function"`, and the trampoline calls that
  symbol. An unnamed callback slot without `userdata` is E0509.

---

## 7. Loading

- `load static` (default): symbols are linked; libraries come from the
  contract's `link`, as `-l`, `-framework` or `.lib` per platform.
- `load dynamic`: the module gains
  `func open(path: string?) -> nil fail: Error`. With no path it searches
  the binding's `search_path`s and the platform's default for the
  library name. Missing symbols are reported by `open`, except those
  marked `weak`, `since` or feature-gated, which fail when called.
- `library "name" when os == linux` overrides the library file per
  platform, for both modes.

---

## 8. Export

```writ
binding kv for n00b export {
    provide kv_open        = "kvstore.open"
    provide kv_put         = "kvstore.put"
    provide kv_get         = "kvstore.get"
    provide kv_iter_new    = "kvstore.iter_new"
    provide kv_iter_next   = "kvstore.iter_next"
    provide kv_errstr      = "kvstore.errstr"
    on_panic error KV_ERR
    allocator own -> kvheap
    header "kv.h"
}
```

- Every declaration the contract provides needs a `provide` (E0509).
- The n00b function's signature must be the one the import mapping
  (§2–§4) would generate for that declaration. Mismatches are reported
  when the generated entry shims are typed, at the `provide` item.
- Symbols are the contract's names, or its `symbol` clauses.
- Entry shims root every GC value they create until they return, and
  attach the calling thread to the n00b runtime if it isn't attached.
  Contract `thread` constraints are checked in debug builds.
- An n00b `fail:` that isn't a contract error, or a runtime failure,
  becomes `on_panic`: `abort`, or `error EXPR` returning that value.
- `own` results are allocated with the named allocator.
- `header` names the C header generated with the shims. It defaults to
  `<library>.h`.

---

## 9. Diagnostics

Binding diagnostics are reported at the binding item, in the `'ffi_spec`
fence, with a note at the contract or profile line it conflicts with.
The n00b compiler forwards them through `core_diagnostics_report_at`
(`n00b-integration.md` §4.2).

| Code | Rejects |
|---|---|
| E0501–E0509 | As in `dsl.md` §9.3 |
| E0510 | A fence that isn't exactly one `binding ... for n00b` block |
| E0511 | `ffi_bind` with a binding for a different contract |

Errors in the generated n00b source (an `error` type without
`from_ffi`, an exported function with the wrong signature) report at the
binding item that chose them.

---

## 10. Complete bindings

### 10.1 zlib

```writ
binding zlib for n00b import {
    module "zlib"
    name compress2 "compress"
    name zlibVersion "version"
    hide "deflateInit_"
    hide "inflateInit_"
    type z_stream as "Stream"
    method z_stream.deflate = deflate
    method z_stream.inflate = inflate
}
```

### 10.2 resharp

```writ
binding resharp for n00b import {
    module "resharp"
    load dynamic
    search_path "target/release"
    buffer Regex::is_match.input as borrow
    buffer Regex::find_all.input as borrow
    error * -> "resharp.Error"
}
```

Bound API:

```
type Regex {}
func Regex.new(pattern: string) -> Regex fail: Error
func Regex.with_options(pattern: string, options: RegexOptions) -> Regex fail: Error
func (self: Regex) is_match(haystack: Array[u8]) -> bool fail: Error
func (self: Regex) find_all(haystack: Array[u8]) -> Array[Match] fail: Error
func (self: Regex) close() -> nil
```

### 10.3 kv

Import:

```writ
binding kv for n00b import {
    module "kv"
    strip_prefix "kv_"
    method Db.put = kv_put
    method Db.get = kv_get
    method Db.iter = kv_iter_new
    method Iter.next = kv_iter_next
    static Db.open = kv_open
    hide "kv_errstr"
}
```

Export: §8.

---

## 11. Decisions

These settle `n00b-integration.md` §6.

1. **Extern syntax.** An annotation only generated code produces. Users
   bind C through `ffi_bind`; an `extern` declaration in user code would
   be a second, unchecked way to do the same.
2. **GC movement.** Not visible to bindings. `n00b_ffi_borrow` roots,
   pins or copies, and generated code is the same in every case.
3. **Finalizers.** Explicit `close()`. Leaked handles are reported in
   debug builds (§5).
4. **Keyword arguments.** Generated options records (§3.1), since n00b
   has no keyword arguments.
5. **Dynamic calls.** writ's own thunks from `abi_class` facts, checked by
   the same conformance tests as the static path.
