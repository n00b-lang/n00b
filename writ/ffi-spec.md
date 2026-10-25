# The `'ffi` Literal

An `'ffi` literal holds one writ contract and n00b's choices for it. This
document defines what the literal may say, what each choice means for the
generated n00b code, how the bound API is named and exported, and what is
reported when a binding contradicts its contract. It is the input for
`n00b-integration.md` phases W1 and N2–N6.

---

## 1. Shape

An `'ffi` literal is an **embedded writ unit** (`dsl.md` §9.3): a contract,
then at most one binding block whose header is implied.

```
[-
    use <zlib.writ>

    binding {
        name compress2 "compress"
    }
-]'ffi

let packed = compress(data, 6)
```

- **The contract part** is any contract directives and declarations
  (`dsl.md` §3–§8), written inline, brought in with `use`, or both. A
  literal that `use`s a shared contract can add `refine` blocks to it. The
  contract part must name exactly one `library`.
- **The binding part** is `binding { ITEM* }` for an import, or
  `binding export { ITEM* }`. The contract is the literal's `library`, and
  the language is `n00b`. With no binding part, the literal imports the
  whole contract with the profile's defaults.
- There is no n00b-only syntax. Everything n00b-specific lives in two
  places:
  - the **n00b profile** (`profiles/n00b.writ`), whose representation IDs
    (`string`, `buffer`, `list`, `record`, …) a binding chooses among;
  - the **n00b generator** (`writ_gen_n00b`), which gives those IDs their
    meaning in n00b code.
- Every binding item except `module`, its targets, and diagnostics
  E0501–E0514 are as in `dsl.md` §9.3. This document adds n00b's profile,
  its defaults, and how each choice appears in n00b.

### 1.1 Binding happens where the literal is

- The preprocessor binds the literal when it evaluates it. There is no
  separate call to make.
- A literal is a statement on its own at module level, or in a
  module-level `when` when it's for some platforms only. It can't
  initialize a variable, and it can't be written in a function.
- The bound functions, types and constants are declared in the module the
  literal is in, as if they were written there. In that module they're
  called by name (`compress(...)`); another module reaches them through
  the module (`zlib.compress`, `zlib.Stream`, `zlib.Z_OK` when the
  literal is in `zlib`).
- They follow the visibility rules of the module's own declarations:
  constants and the bound API are public, and the generator's helpers are
  private. A package manifest exports them, or leaves them out, like any
  other member (§9).
- A bound name that the module already declares is an error, reported at
  the literal. Rename the binding with `name` or leave it out with `hide`
  (§2.3).
- The compiler supplies `'ffi`'s behaviour. The program carries the
  generated declarations, shims and link requirements; the writ unit and
  the literal's text never reach it.
- Every bound name in n00b code leads back to its literal, then to the
  contract it `use`s. `n00b build PROGRAM --show-ffi MODULE:LINE` prints
  the n00b declarations generated for the literal whose `[-` is on line
  LINE of MODULE.

---

## 2. Surface mapping

### 2.1 Names

| Contract | n00b | Default |
|---|---|---|
| The contract | Declarations in the module the literal is in | — |
| `fn f` | `func f` in the module | Name after `strip_prefix`, in `snake` case |
| `handle H`, `opaque` crossing as a handle | `type H` holding the C value | `Snake_Case` |
| `struct S` | `type S layout(c) { ... }` | `Snake_Case` |
| `enum E : T` | `open enum E: T` with the same values; a `closed` contract enum becomes a closed n00b `enum E: T` | `Snake_Case`; members `upper` |
| `flags F` | n00b flag set with the same values | `Snake_Case`; members `upper` |
| `const C` | `const C` | `upper` case |
| `callback C` | n00b `callback` type | `Snake_Case` |

- Names that collide with n00b's reserved words get a trailing `_`.
- Two contract declarations that end up with one n00b name are E0505,
  reported at the later declaration's binding item, or at the
  `strip_prefix` that caused it. A bound name that collides with one of
  the module's own declarations is reported by the n00b compiler (§1.1).
- A `lang rust` contract names impl items as `T::m`. `name`, `method`,
  `hide` and the other items accept `T::m` wherever they accept a
  function name.

### 2.2 Methods on handle types

- n00b calls with `.` go through traits, so bindings add no method
  syntax. A program that wants `h.m(...)` declares a trait for it.
- `method H.m = f` binds `f` as the function `m` whose first parameter,
  `self: H`, is `f`'s first parameter of type `H`. It is called as
  `m(h, ...)`.
- `static H.m = f` binds `f` as the function `h_m(...)`. When `f` returns
  an `H`, the binding also defines `__init(modify self: H, ...)` with the
  same parameters, so `new[H](...)` builds an `H` and panics if `f` fails.
  `h_m` stays available as the fallible form.
- In a `lang rust` contract, every impl item is grouped by default: items
  taking `self`, `&self` or `&mut self` are bound as `method`, and the
  rest as `static`.
- A handle's `release` function becomes the function
  `close(modify self: H)`, and is not also bound under its own name.
- C contracts are not grouped by default. Grouping is always written out.

### 2.3 Hiding and renaming

- `hide "glob"` removes declarations from the bound API. Hidden
  declarations still exist in the contract, so a binding can hide a
  function another function's plan calls (an error-detail function, say).
- `raw "glob"` binds a declaration without a shim: the n00b function
  calls the C symbol directly, and its n00b name gets the suffix `_raw`.
  Integers keep their width. A pointer argument is `Ref[byte]`
  (`Ref[byte]?` when nullable), so the GC keeps the object alive through
  the call; a pointer result is its address as `uint`. A declaration whose
  facts include `unknown` can only be bound this way.
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
    string string to bytes[n] via copy copies
    pack rest list native "List[T]"
    pack rest array native "Array[T]"
    pack keywords kwargs native "keyword arguments"
    pack keywords record native "options record"
    variadic call yes
    variadic define no
    error style result
    callback style closure
    handle close "close"
    lifetimes no
    naming fn snake
    naming type Snake_Case
    naming const upper
    naming field snake
    naming module snake
    reserved "alias", "and", "as", "assert", "break", "callback", "case", "cast", "comptime", "const", "consume",
             "context", "continue", "elif", "else", "ensure", "enter", "enum", "error", "exit", "export", "fail",
             "false", "flagset", "for", "from", "func", "if", "import", "in", "inline", "let", "litmod", "modify",
             "new", "nil", "or", "out", "package", "private", "provides", "public", "record", "ref", "require",
             "return", "step", "switch", "thread", "to", "trait", "true", "try", "type", "typecase", "typeof",
             "unsafe_cast", "until", "var", "variant", "when", "where", "while", "with"
}
```

### 3.1 Choices, per contract construct

| Contract construct | n00b choices (profile ID) | Default |
|---|---|---|
| `in *cstr`, `in utf8[n]` | `string` (copied to a NUL-terminated or counted buffer) | `string` |
| `in *[u8; n]`, `bytes[n]` | `buffer` (copied), `borrow` (the n00b array's storage, for the span), `string` (the shim takes an n00b `string` and passes its UTF-8 bytes) | `buffer` |
| `out` arrays and returned owned buffers | `buffer` (a new `Array[u8]`), `string` (the shim decodes the bytes as UTF-8 into an n00b `string`; invalid text is an error) | `buffer`; `string` when the contract says `utf8` or `cstr` |
| Rest packs | `list` (`List[T]`), `array` (`Array[T]`) | `list` |
| Keyword packs | `kwargs`: n00b keyword arguments, one per key, optional unless the key's presence rule requires it; `record`: a generated `type F_Options` with one optional field per key | `kwargs` |
| `out` / `inout` scalars | Extra results | Promoted (§3.2) |
| Labels | `label path -> "string"`, `label secret -> "string"` | The profile's |
| `?T` | `T?` | — |
| `bool`, fixed-width integers, `c_*` | The n00b scalar of the platform's width | — |

The choices are made with `string`, `buffer`, `pack` and `keywords`
items. A choice the profile doesn't declare for the slot's form is E0502:
`string compress2.source as list` is rejected because `list` is a pack
form, and `source` is a `*[u8; n]`.

A `string` choice on a byte slot puts the conversion in the generated
shim. The n00b side sees a `string`, and the C side still receives (or
fills) bytes: `string Regex::is_match.input as string` lets n00b code
call `regex.is_match("text")`.

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
func compress(source: Array[u8], level: i32) -> Array[u8] fail: Ffi_Error
```

---

## 4. Errors

- A function with `error` or `failure` outcomes returns `T fail: E`.
- `E` is the binding's `error PATTERN -> "Type"` choice. The default is
  the builtin record `Ffi_Error`, which every literal shares:

  ```
  record Ffi_Error {
      code: i64        # the `via` value, or the status itself
      text: string?    # the error channel's text, when there is one
      fn:   string     # the contract function that failed
  }
  ```

  A binding that wants failures its callers can tell apart from other
  libraries' names its own type.

- Error detail comes from the contract, never from the binding:
  - `via expr` fills `code`, and `text` when the expression is text
    (`via kv_errstr(ret)`);
  - a `text` error channel (`dsl.md` §6.4) fills `text`;
  - `errno`-style channels fill `code`.
- `error PATTERN -> "Type"` may name an n00b type with a constructor
  `__init(modify self: Type, code: i64, text: string?, fn: string)`.
  The generator builds failures with `new[Type](code, text, fn)`, and the
  constructor's existence is checked when the generated source is typed.
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
| Policy parameters | `policy f.p use ARM` fixes the arm and removes `p`. Without one, `p` becomes an enum parameter of the n00b function with a member per arm, and each arm is also bound as `f_arm`. The `else` arm is the member `OWNED`: the shim hands C its own copy of the slot the arm consumes, with a destructor it provides. |
| `consume` | The n00b wrapper is marked closed after the call; using it again fails with the binding's error type |

- **Handles close explicitly.** `close()` is the release. A handle that
  is collected while still open is reported in debug builds and released
  then. Finalizers are not part of the binding; when n00b's collector
  gains them, the profile changes and bindings don't.
- `borrow TARGET as lifetime` is E0504 for n00b.

---

## 6. Callbacks

- A `callback` type becomes an n00b `callback` type named by
  `callback C as "Name"`, defaulting to the contract name in
  `Snake_Case`. Its signature is the callback's, mapped as in §3.
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
  `func open(path: string?) -> nil fail: Ffi_Error`, called as `open(nil)`
  (`resharp.open(nil)` from another module); a module that has its own
  `open` renames it with `name`. With no path it searches
  the binding's `search_path`s and the platform's default for the
  library name. Missing symbols are reported by `open`, except those
  marked `weak`, `since` or feature-gated, which fail when called.
- `library "name" when os == linux` overrides the library file per
  platform, for both modes.
- `link "c"` names the target's C library. It's never passed as `-l`, and
  it needs no probe. On Linux, n00b programs link no libc: they're static
  binaries started by the runtime, and the n00b pool allocator
  (`sys.pool`) serves C's `malloc` family for shims. A contract whose C
  code needs libc says `link "c"`, as does a contract for any system
  library built against glibc. The program is then an ordinary libc
  program, linked against the n00b runtime built for libc, and so is its
  compile-time runner. On other platforms the C library is always linked,
  so `link "c"` changes nothing.

### 7.1 System calls

A `syscall` declaration (`dsl.md` §7.7) is a minimal proxy for the
kernel ABI, and its n00b binding is just as minimal. Names follow §2.
None of the type choices in §3 apply, and neither does `Ffi_Error` (§4).

**Signature.**

| Contract | n00b |
|---|---|
| Integer parameter or result | The n00b integer of the same width and signedness |
| `*T` parameter | `Ref[byte]` |
| `?T` parameter | `Ref[byte]?` (`nil` passes 0) |
| Pointer result | `uint`: the address. Memory the kernel hands back, such as an `mmap` region, isn't GC memory. |
| The errno check | `fail: E`, carrying the positive errno. `E` is `i32` unless the binding chooses a type (below). `infallible` syscalls have no `fail:` channel. |

**The errno type.** The binding's `error * -> "Type"` item (§4) chooses
`E` for every syscall in the literal. For a syscall, `Type` must be an
`open enum` or a distinct integer type (`type Errno = i32`). The generated
code converts the errno with `cast[Type]`, with no constructor and no
allocation. The type must hold every errno, not only the ones it names,
because the kernel can return codes newer than any table. A closed enum
doesn't qualify: converting an integer to one isn't a safe cast. Any
other type is E0515. A literal whose `fn`s have error clauses needs a §4
error type for those, so keep `syscall`s in a literal of their own.

The `os` module defines `open enum Errno: i32`, with member values chosen
per platform by `when`. Linux uses the same numbers on x86_64 and aarch64,
but macOS doesn't (`EAGAIN` is 11 on Linux and 35 on macOS). Shims
written over libSystem or Windows fail with that same type.

So, with `error * -> "Errno"`,
`syscall write(fd: c_int, buf: *u8, count: usize) -> isize` binds as

```n00b
func write(fd: i32, buf: Ref[byte], count: uint) -> int fail: Errno
```

- **GC memory versus addresses.** A `Ref[byte]` is a GC reference, so the
  object stays alive through the call. Memory outside the GC heap, such as
  the address `mmap` returned that `munmap` later takes, is declared
  `usize` in the contract and passed as `uint`.
- **No allocation.** The failure is a scalar errno, so a syscall binding
  never allocates and is safe on paths that can't allocate.

**Lowering.**

- **No shim and no symbol.** The generated n00b function calls an extern
  marked as a kernel entry with its number for the target. Instead of
  emitting a call, the compiler emits the instruction inline as an LLVM
  inline-asm expression. Its constraints come from the kernel-entry table
  in `dsl.md` §7.7, and it is always marked as having side effects and
  clobbering memory. Nothing is linked and no C is involved, so a Linux
  build stays libc-free.
- **The errno check is n00b code** in the generated function: the range
  test on the raw register, `fail` with `-ret`, then the conversion to
  the result type.
- **Always available.** `load dynamic` doesn't apply: `open()` doesn't
  check syscalls, and they never fail as unavailable.
- **No stack map.** The instruction isn't a call, so it gets no stack map,
  and a non-blocking syscall is invisible to the collector.
- **Blocking syscalls.** For a syscall with the `blocking` effect, the
  generated function enters the collector's blocking state just before the
  instruction and leaves it just after: the same transitions
  `__gc_enter_blocking` and `__gc_leave_blocking` make today. `Ref[byte]`
  arguments live in the frame across both transitions, so their objects
  stay reachable. While the thread is blocked, a collection can run on
  another thread. The caller must therefore make sure that memory the
  kernel writes during a blocking syscall holds no GC references (byte
  buffers are the normal case). The binding can't check this, because the
  pointee type is erased.
- **Effects** map onto the compiler's effect summary as they do for a
  `fn` (`compiler_ffi.c`, `native_effects`). `dsl.md` §7.7 limits a
  syscall to effects that function maps, so a syscall never contributes
  the "unknown" summary.

**Platform structure.** Syscalls exist only on Linux targets, so a module
that also builds elsewhere chooses an implementation per platform with
`when`. Typed APIs, buffer handling, errno names and any retrying belong
in that n00b code, not in the contract:

```n00b
when target_os() == "linux" {
    open enum Errno: i32 {
        EPERM       = 1,
        ENOENT      = 2,
        EINTR       = 4,
        EAGAIN      = 11,
        EWOULDBLOCK = 11,
        # ...
    }

    [-
        library kernel
        platforms x86_64-linux-gnu, aarch64-linux-gnu

        syscall write(fd: c_int, buf: *u8, count: usize) -> isize
          number 1 when arch == x86_64
          number 64 when arch == aarch64
          effects output, blocking

        binding {
            error * -> "Errno"
            # This module's own `write` is the API.
            name write "syscall_write"
        }
    -]'ffi

    func write(fd: i32, data: Array[u8]) -> int fail: Errno {
        let count: uint = __array_length(data)
        if count == 0 {
            return 0
        }
        return syscall_write(fd, unsafe_cast[Ref[byte]] ref data[0], count)!
    }
}
```

On macOS and Windows the same function is written over libSystem or
runtime helpers instead. Reading errno after a raw libSystem call is
correct only if nothing between the call and the read can overwrite it.
Whether n00b can place a collector safepoint there has not been checked
yet.

---

## 8. Export

```
[-
    use "kv.writ"

    binding export {
        provide kv_open      = "kvstore.open"
        provide kv_put       = "kvstore.put"
        provide kv_get       = "kvstore.get"
        provide kv_iter_new  = "kvstore.iter_new"
        provide kv_iter_next = "kvstore.iter_next"
        provide kv_errstr    = "kvstore.errstr"
        on_panic error KV_ERR
        allocator own -> kvheap
        header "kv.h"
    }
-]'ffi
```

- An export literal declares nothing in its module. `--show-ffi` finds it
  by its module and line, like an import literal.
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

## 9. Packages

A literal's bound names are members of the module it's in, so they follow
n00b's package rules like the rest of that module.

- **Inside the package**, they're reached by importing the module the
  literal is in.
- **Outside the package**, a consumer sees them only through the logical
  view a `package` statement builds. A package with no manifest exports
  what the module's default visibility makes public. A manifest decides
  for itself: `export MODULE` exports all the module's public members,
  the bound names among them, and `export MODULE.name` exports one.
- To keep the bound names under their own name, put the literal in its
  own module and export that from a subpackage.

A package that is a C library's binding and nothing else keeps the
literal in the package's `__init.n`:

```
[-
    use <zlib.writ>

    binding {
        name compress2 "compress"
        name zlibVersion "version"
    }
-]'ffi
```

A consumer imports the package and calls through its name:

```
import zlib

let packed = zlib.compress(data, 6)
```

A package that contains a binding alongside other code gives the binding
its own module and a subpackage:

```
import archive.formats
import archive.zlib_binding

package archive {
    export formats.open_archive
    export package zlib
}

package archive.zlib {
    export zlib_binding
}
```

Here the consumer calls `archive.zlib.compress(data, 6)`.

---

## 10. Diagnostics

Binding diagnostics are reported at the binding item, in the `'ffi`
literal, with a note at the contract or profile line it conflicts with.
Diagnostics in a contract that the literal `use`s are reported in that
file. The n00b compiler forwards them through `core_diagnostics_report_at`
(`n00b-integration.md` §4.2).

| Code | Rejects |
|---|---|
| E0501–E0513 | As in `dsl.md` §9.3; E0510 covers a literal with more than one binding part |
| E0514 | A literal whose contract part doesn't name exactly one `library`, or whose binding has a `module` item |
| E0515 | An `error * -> "Type"` choice for a literal with `syscall`s, where `Type` isn't an `open enum` or a distinct integer type (§7.1) |
The n00b compiler itself rejects:

- a literal that initializes a variable, or isn't a statement at module
  level;
- a bound name the module already declares, at the literal;
- a user literal modifier named `ffi`, as it does for any user modifier
  that would shadow a builtin one.

Errors in the generated n00b source (an `error` type without
`from_ffi`, an exported function with the wrong signature) report at the
binding item that chose them.

---

## 11. Complete literals

### 11.1 zlib

```
[-
    use <zlib.writ>

    binding {
        name compress2 "compress"
        name zlibVersion "version"
        hide "deflateInit_"
        hide "inflateInit_"
        type z_stream as "Stream"
        method z_stream.deflate = deflate
        method z_stream.inflate = inflate
    }
-]'ffi
```

### 11.2 resharp

```
[-
    use <resharp.writ>

    binding {
        load dynamic
        search_path "target/release"
        string Regex::is_match.input as string
        buffer Regex::find_all.input as borrow
    }
-]'ffi
```

Bound API:

```
type Regex {}
func open(path: string?) -> nil fail: Ffi_Error
func regex_new(pattern: string) -> Regex fail: Ffi_Error
func __init(modify self: Regex, pattern: string)
func regex_with_options(pattern: string, options: Regex_Options) -> Regex fail: Ffi_Error
func is_match(self: Regex, input: string) -> bool fail: Ffi_Error
func find_all(self: Regex, input: Array[u8]) -> Array[Match] fail: Ffi_Error
func close(modify self: Regex)
```

### 11.3 kv

Import:

```
[-
    use "kv.writ"

    binding {
        strip_prefix "kv_"
        method Db.put = kv_put
        method Db.get = kv_get
        method Db.iter = kv_iter_new
        method Iter.next = kv_iter_next
        static Db.open = kv_open
        hide "kv_errstr"
    }
-]'ffi
```

Export: §8.

---

## 12. Decisions

These settle `n00b-integration.md` §6.

1. **Extern syntax.** An annotation only generated code produces. Users
   bind C through `'ffi` literals; an `extern` declaration in user code
   would be a second, unchecked way to do the same.
2. **GC movement.** Not visible to bindings. `n00b_ffi_borrow` roots,
   pins or copies, and generated code is the same in every case.
3. **Finalizers.** Explicit `close()`. Leaked handles are reported in
   debug builds (§5).
4. **Keyword arguments.** Keyword packs become n00b keyword arguments by
   default; a binding can choose a generated options record instead
   (§3.1).
5. **Dynamic calls.** writ's own thunks from `abi_class` facts, checked by
   the same conformance tests as the static path.
