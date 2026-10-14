# writ — Contract Language Specification

A small declarative language for describing a native-ABI surface to a binding
generator or to a human writing bindings by hand. The surface can be a C
library, or a C++ library called by linking directly to its symbols, for
example from Rust (§8). It describes:

1. **Layout**: what the bytes look like.
2. **Lifetime**: who owns each pointer, how long the other side may hold it,
   and who releases it with what.
3. **Shape**: how one logical value (a slice, a string, a closure, an error)
   spreads across several physical ABI slots.
4. **Names**: how one entity is named in the contract, in its source
   language, in the linker's symbol table, and in each target language (§7).

A C header answers only the first question, and only partly. The rest
usually lives in prose docs, comments, or nowhere. This DSL gives those
answers a place to live, in a form a generator can check.

---

## 1. Design principles

| Principle | Consequence |
|---|---|
| **Declarations look like headers.** | One declaration per C entity, in C parameter order. You can write the DSL with the header open next to it, line for line. |
| **Physical first, logical second.** | Every slot has a physical ABI type. Logical meaning (slice, string, bool, enum) is layered on top and never changes the physical layout. |
| **Defaults cover the safe common case.** | A `const T *` parameter with no annotation means "borrowed, read-only, for the duration of the call". Most declarations need no annotations at all. |
| **Unknown is a value.** | `unknown` means "not yet determined". Tools can emit `unknown`, humans resolve it, generators refuse it (or fall back to an explicit `unsafe` raw binding). This is how automation and manual review work together. |
| **Layout is asserted, never invented.** | The DSL follows the platform's C ABI rules. Size and offset annotations are assertions, checked by compiling a probe against the real header. |
| **Refine, don't restate.** | You can import raw declarations from a header and add only the missing contract facts in `refine` blocks. |
| **One canonical lowered form.** | Every file lowers to a fully explicit form (no defaults, no sugar, JSON-serializable). Generators consume only that form, and it is easy to diff. |
| **Names are not symbols.** | The contract name, the source-language name, the link symbol and each target-language name are separate facts. Each can be stated, derived, and checked on its own. |

Non-goals: re-specifying the C++ ABI (the DSL relies on the Itanium and MSVC
rules and states only what a binding needs to call through them), assuming
any Rust ABI (§8.9), describing semantics beyond the boundary (what a
function *does*), or being a general type system.

**Tiers.** The language is specified in full, and implemented in two tiers.
Tools reject an extended-tier construct with "not supported yet". A
construct moves to the core tier when a library being bound needs it.

| Tier | Sections |
|---|---|
| **Core** | §3; §4.1–§4.4 (without refcounted handles); §4.6–§4.8; §5.1–§5.5, §5.7, §5.9; §6.1–§6.4; variadic functions and rest packs, including `va_list` and `format printf` (§6.6); thread constraints and handle thread rules (§4.3, §6.5); §7.1–§7.3; `var` with `const`/`mut` (§7.4); §7.5–§7.6; §8.9; §9.1–§9.3; §9.5 for Meson and Cargo; §10–§12 |
| **Extended** | Unions (§4.5); bit-level layout (§4.9); refcounted handles (§4.3); policy parameters (§5.6); `identity`, `live` and overlap (§5.8); effects (§6.5); keyword packs (§6.6); labels (§6.7); `var` attributes beyond `const`/`mut` (§7.4); C++ (§8.1–§8.8); other ABI families (§9.4); other build integrations (§9.5) |

---

## 2. A quick taste

```writ
library zlib
link    "z"
header  <zlib.h>

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

From this a generator knows:

- `dest` is a caller-allocated output buffer. Its capacity goes in via
  `*destLen` and the number of bytes written comes back through the same slot.
- `source` is a read-only byte slice borrowed for the call. Its length is
  `sourceLen`, so a high-level binding takes one `bytes` argument and fills
  in both slots.
- Any return value other than `Z_OK` is an error.

The idiomatic high-level signature, e.g. `compress2(source: bytes, level: int) -> bytes`,
can be derived mechanically. No hand-written glue is needed.

---

## 3. File structure

### 3.1 Three kinds of file

The system keeps three concerns apart, each in its own kind of file:

| File | Says | Written by | Mentions languages? |
|---|---|---|---|
| **Contract** (`.writ`) | The boundary: layout, lifetime, shapes, packs, effects, symbols | Whoever owns or wraps the library | No, apart from the library's own source language (`lang c` / `lang c++`) |
| **Language profile** (§9.2) | How one language represents strings, slices, packs, errors, GC pinning, naming | Shipped with the tools, extensible | Exactly one language |
| **Binding** (§9.3) | How one language *imports* or *exports* one contract: names, idiom choices, overrides | Whoever generates bindings for a language | Exactly one language and one direction |

A contract never says how Rust or Python spells anything. A binding never
changes a fact about the boundary. Everything in the rest of this section,
and in §4–§8, is contract syntax.

Terminology used throughout:

- **Platform**: an OS/arch/ABI triple such as `x86_64-linux-gnu`.
- **Target language**: a language a binding is generated for.
- **Provider**: the side that implements a declaration.
- **Consumer**: the side that calls it.

### 3.2 Contract header

```writ
library sqlite3                  # logical name; namespace for generated code
link    "sqlite3"                # library to link / dlopen; repeatable
header  <sqlite3.h>              # repeatable; parsed together, in order
header  <sqlite3ext.h>
define  SQLITE_ENABLE_COLUMN_METADATA      # macros that change what the headers mean
include_path "/opt/sqlite/include"
compile_commands "build/compile_commands.json"   # or take flags from the build (§9.5)
encoding utf8                    # default text encoding for cstr / str
platforms any                    # or a list: x86_64-linux-gnu, aarch64-apple-darwin, ...
lang    c                        # source language: c (default) or c++ (§8)

import <sqlite3.h> raw           # optional: pull in raw declarations (see §10.2)

# declarations: const, type, opaque, handle, struct, union, enum, flags,
#               keywords, callback, fn, var, interface, refine
```

Comments start with `#`. Identifiers are C identifiers, used verbatim.
Trailing commas are allowed everywhere.

**Several headers.** A library can list any number of headers. They are
parsed as one translation unit, in order, with the given `define`s and
include paths, since many headers mean different things under different
macros (`_FILE_OFFSET_BITS`, `WIN32_LEAN_AND_MEAN`, feature-test macros).
Each imported declaration records which header it came from. A contract
may also span several files with `use "other.writ"`, for libraries split
into modules or for sharing common types between libraries.

**No headers.** For a binary with no headers (a plugin ABI, a stripped
vendor library, a library written in another language that exports a C
ABI), leave out `header`. Then nothing can be probed, so every record must
have explicit layout (§4.4), and `verify` checks only symbols.

**Platform scope.** `platforms` names the set of platforms the contract
claims to support, and applies to the whole file. Every fact holds on every
listed platform unless it is inside a `when` block (§4.7) or carries a
per-platform suffix (`@size(112 on lp64, 88 on llp64)`, `symbol "..." when
os == macos`). Any declaration can be restated per platform. `when` blocks
can contain any declaration, including `refine`, so a contract that differs
on Windows says so in one place. `verify` runs once per listed platform, and
the lowered form is produced per platform.

---

## 4. Types and memory layout

### 4.1 Primitives

| DSL | Meaning |
|---|---|
| `i8 i16 i32 i64`, `u8 u16 u32 u64` | Exact-width integers |
| `f32 f64` | IEEE floats |
| `bool` | C `_Bool` (1 byte on all supported targets) |
| `usize isize` | `size_t` / `ptrdiff_t` |
| `uptr` | `uintptr_t` |
| `void` | Only as a return type or a pointee |
| `c_char c_schar c_uchar c_short c_ushort c_int c_uint c_long c_ulong c_longlong c_ulonglong` | C types whose size depends on the platform |
| `i16be u32le ...` | Explicit byte order, for wire/file formats. Physically the same as the plain integer type. |

`c_long` is 4 bytes on Windows and 8 bytes on LP64. Use the C name when the
header uses the C name. Generators resolve it per platform.

### 4.2 Pointers

```writ-fragment
*T          non-null pointer to one T
?T          nullable pointer to one T
*[T; n]     non-null pointer to n contiguous Ts         (an array view; see §6)
?[T; n]     nullable pointer to n contiguous Ts
*void       untyped pointer (?void if nullable)
**T         non-null pointer to non-null pointer to T
*?T         non-null pointer to a nullable pointer to T (a typical `T **out`)
```

`*` and `?` are the two pointer constructors: `*` is non-null and `?` is
nullable. There is no separate "nullable" modifier, because only pointers
(and handles, which are pointers or integer handles) can be null.

Pointers are **non-null by default**. You say nullable explicitly. In C,
`const` is a fact about access, and the DSL expresses access as a parameter
mode (`in`, `out`, `inout`; §5). Imported `const` is used only to pick the
default mode.

### 4.3 Aliases, opaque types, handles

```writ
type uLong = c_ulong                 # pure alias; same type

opaque sqlite3                       # incomplete type; only ever behind a pointer
opaque sqlite3_stmt
opaque stat_buf source "struct stat" # a type C names only by its tag

handle Db   = *sqlite3       release sqlite3_close_v2
handle Stmt = *sqlite3_stmt  release sqlite3_finalize
```

A `handle` is a named type with a **release function** attached. It is
usually a pointer, but may be an integer. It is the main tool for lifetime
management:

- When the callee produces a handle (as a return value or an `out` param), the
  caller **owns** it by default and must release it exactly once.
- When a handle is passed as an ordinary parameter, it is **borrowed** by
  default.
- A handle is non-null by default, like any pointer. `?Db` is a nullable handle.

Generators turn handles into RAII objects, finalizer-backed wrappers,
context managers, and similar constructs.

**Integer handles.** File descriptors, sockets, Win32 `HANDLE`s stored as
integers, GL object names and the like are handles too. For these, give the
value that means "none" instead of relying on null:

```writ
handle Fd     = c_int   none -1  release close
handle Socket = uptr    none ~0  release closesocket    # Windows SOCKET

fn open(path: *cstr, flags: c_int as OpenFlags, ...) -> own ?Fd error ret == none via errno
fn read(fd: Fd, buf: out *[u8; cap n, len ret], n: usize) -> isize error ret < 0 via errno
fn dup(fd: Fd) -> own ?Fd error ret == none via errno
```

`none` gives the value that means "no handle" (§4.6), and `?Fd` means "may
be `none`". This is how you get a *distinct* `fd` type, not just a label.
Compare the three ways to name an integer:

| Construct | Distinct type in bindings? | Carries ownership? | Use for |
|---|---|---|---|
| `type fd = c_int` | No: a pure alias | No | Readability only |
| `c_int as Fd` (logical view) | Yes: a newtype | No | Values with meaning but no lifetime (`uid`, `pid`, enum-like codes) |
| `handle Fd = c_int none -1 release close` | Yes | Yes | Resources that must be released |

Labels (§6.7) are a fourth, separate mechanism. They attach metadata and
never change the type.

**Thread rules on handles.** Whether a handle may be used from several
threads is a fact about the handle type, so it is stated there:

```writ
handle Regex = *opaque release resharp_free  shared
handle Window = *GtkWindow release gtk_window_destroy  thread_affine
```

| Rule | Meaning |
|---|---|
| `shared` | Any thread may use it, including concurrently. The library serializes internally if it needs to. |
| `thread_affine` | Every call must come from the thread that created it |
| (neither) | Any thread may use it, but not concurrently. Bindings serialize access. |

A function taking a handle inherits the handle's rule, unless the function
states its own `thread` constraint (§6.5). `*opaque` declares an anonymous
opaque pointee in place.

**Reference-counted handles.** Many APIs share ownership by counting
references instead of having a single owner. Examples are COM
(`AddRef`/`Release`), CoreFoundation (`CFRetain`/`CFRelease`), GObject
(`g_object_ref`/`g_object_unref`), and Swift's `SWIFT_SHARED_REFERENCE`.
Declare these with `refcounted` and a retain function:

```writ-extended
handle CFString = *__CFString  refcounted retain CFRetain release CFRelease
handle Widget   = *GtkWidget   refcounted retain g_object_ref release g_object_unref
                               floating sink g_object_ref_sink
```

For a refcounted handle, the ownership words count references:

| Contract | Meaning | Generator obligation |
|---|---|---|
| `-> own CFString` | **+1**: the caller receives a reference it must release | Wrap; release on drop |
| `-> CFString with x` / `until ...` | **+0**: borrowed, no reference handed over | Retain to keep it longer than the span, or copy |
| `p: CFString` (param) | Borrowed. The callee retains it if it wants to keep it. | Nothing |
| `p: consume CFString` | The callee takes over the caller's reference ("steals" it) | Hand over the reference. If you need to keep using the handle, retain it first. |
| `-> own Widget floating` | A floating reference that the first owner must "sink" | Call `sink` immediately (or let the container sink it) |

`keep` is not needed for a refcounted parameter, because the callee holds
its own reference. Generators use `retain` to turn a +0 borrow into an owned
wrapper when the target language needs one.

**Weak references** need no special construct. A weak reference is its own
handle type with its own release function. Upgrading it to a strong
reference is an ordinary function that may fail:

```writ
handle WeakRef = *GWeakRef release g_weak_ref_clear
fn g_weak_ref_get(w: WeakRef) -> own ?GObj     # null if the object is gone
```

### 4.4 Records

```writ
struct z_stream @size(112) @align(8) {
    next_in:   ?[u8; avail_in]       @0
    avail_in:  c_uint                 @8
    total_in:  c_ulong                @16
    next_out:  ?[u8; avail_out]      @24
    avail_out: c_uint                 @32
    total_out: c_ulong                @40
    msg:       ?cstr static          @48
    state:     ?void private         @56
    zalloc:    ?alloc_func            @64
    zfree:     ?free_func             @72
    opaque:    ?void userdata(zalloc, zfree)  @80
    data_type: c_int                  @88
    adler:     c_ulong                @96
    reserved:  c_ulong private        @104
}
```

- By default (`layout c`), fields are laid out with the platform's C rules.
  The DSL never reorders fields.
- `@N` after a field asserts its byte offset. `@size(N)` and `@align(N)` on
  the record assert its size and alignment. All of these are **optional**
  under `layout c`. They are **checked**, never applied, and belong in the
  file when a tool generated them or when a value is surprising.
- Assertions can be per-platform. Write `@size(112 on lp64, 88 on llp64)`, or
  put the declaration in a `when` block (§4.7).

**Layout modes.** A record says which rules produced its layout:

| Mode | Meaning | Offsets |
|---|---|---|
| `layout c` (default) | The platform's C rules, as the C compiler would lay it out | Optional; checked against a probe |
| `layout explicit` | Every byte position is given. No compiler rules apply. | **Required** on every field, plus `@size` |
| `layout explicit bits in U order` | Every field placed at a bit offset (§4.9) | **Required** (`@bit(N)`, or `@N` for byte-aligned fields), plus `@size` |

`layout explicit` is for wire and file formats, for libraries with no
headers, and for data produced by a language whose own rules differ from
C's (Go, Swift, or Rust without `repr(C)`). There is no "use language X's
rules" mode. Those rules are unstable or unspecified in most languages, and
the contract must not depend on a language. Such a language describes what
it actually produces, either by C rules or explicitly.
- `private` means generated code must not read or write the field. It still
  takes up space in the layout.

Layout modifiers, which do change layout and must match the header:

```writ-fragment
struct Packed @pack(1) { ... }           # #pragma pack(1)
struct Wide   @align(64) { ... }         # alignas on the record
field:  u32 @align(16)                   # alignas on a field
flags:  u32 bits 3                       # C bitfield; follows the platform's C bitfield rules
```

Bitfields, fields packed into an integer, bit-addressed records and
bit-slices are covered in §4.9.

Inline fixed arrays use brackets with no pointer: `name: [c_char; 16]`.

A **flexible array member** is an inline array in the last position whose
count names another field:

```writ
struct Packet {
    count: u32
    items: [Item; count]                 # FAM; sizeof(Packet) excludes it
}
```

### 4.5 Unions

```writ-extended
union Raw { i: i64, d: f64, p: *void }   # plain C union

struct Value {
    kind: c_int as ValueKind
    v:    union by kind {                # tagged: the tag picks the live arm
        VK_INT  => i: i64
        VK_REAL => d: f64
        VK_TEXT => s: *cstr
        VK_NULL => _                     # no live arm
    }
}
```

The physical layout of `union by` is identical to a plain C union. The tag
only tells generators which arm they may read.

### 4.6 Enums, flags, logical views

```writ
enum ValueKind : c_int {
    VK_INT = 1, VK_REAL = 2, VK_TEXT = 3, VK_NULL = 5,
}

enum Level : c_int { LOW = 0, HIGH = 1 } closed

flags OpenFlags : c_int {
    READONLY = 0x1, READWRITE = 0x2, CREATE = 0x4,
}
```

Enums are open: values not listed may appear, and bindings must accept
them. `closed` says any other value is a contract violation.

The `as` keyword attaches a **logical view** to a physical type without
changing its layout:

```writ-fragment
ok:    c_int as bool          # 0 = false, non-zero = true
kind:  c_int as ValueKind
when:  i64   as unix_seconds  # a named view generators can map (e.g. to datetime)
```

**Views on results apply to the success domain.** A function's `error`
clause removes its error values first, and the view applies to what is
left:

```writ
fn resharp_is_match(r: Regex, input: bytes[len] null if empty, len: usize) -> i32 as bool
  error ret == -1 via last_error
```

`1` means `true`, `0` means `false`, and `-1` is the error. The checker
rejects a view whose success domain doesn't fit the view type.

**`none` values.** Any type can name the value that means "absent":

```writ
struct Span { start: usize, end: usize } none { start: usize_max, end: usize_max }
handle Fd = c_int none -1 release close
```

`?Span` is then a `Span` that may be `none`, and bindings lift it to the
language's optional type. For pointers and pointer handles, `none` is
null and needs no declaration.

### 4.7 Target and version conditionals

```writ
when os == windows {
    type wchar = u16
} else {
    type wchar = u32
}

when platform in [x86_64-*, aarch64-*] {
    type reg = u64
}
```

Conditions can test `os`, `arch`, `abi`, `platform` (a triple glob),
`pointer_width`, `libc`, `stdlib`, and `version`.

**Library versions.** A library declares how to find out its version at
runtime. Then declarations can say when they appeared or disappeared:

```writ
version query sqlite3_libversion_number() -> c_int   # 3038000 for 3.38.0
        format "MAJOR*1000000 + MINOR*1000 + PATCH"

fn sqlite3_error_offset(db: Db) -> c_int  since 3.38
fn sqlite3_old_api(db: Db) -> c_int       removed 3.40

when version >= 3.45 {
    refine sqlite3_prepare_v3 {           # a contract that changed between versions
        pzTail: out ??c_char with zSql
    }
}
```

`since` and `removed` are shorthand for `when version` blocks. Generators
bind such symbols weakly (`dlsym`, or weak linking on Darwin). If the
loaded library is too old, the generated function returns a clear
"unavailable" error. It doesn't fail at load time. `verify` checks each
`since` against the export tables of the oldest and newest versions it is
given.

**Build features.** Some declarations exist only when the library was
built with an option: a Cargo feature, a `configure` flag, a compile-time
define. Declare the feature, then condition on it:

```writ
feature capture_groups = cargo "experimental_capture_groups"
feature column_metadata = define "SQLITE_ENABLE_COLUMN_METADATA"

when feature capture_groups {
    fn resharp_capture_slots(r: Regex) -> usize  pure
}
```

The platform description says which features the library under test was
built with. `verify` checks that the symbols exist exactly when their
feature is on. Bindings for late-loaded libraries treat feature-gated
functions like `weak` ones: present or not, decided at load time.

### 4.8 The layout report

`writ layout <file> --platform <triple>` prints the resolved layout. Its output
is also valid DSL with every assertion filled in, so you can paste it back in
to pin the layout:

```writ-fragment
struct z_stream @size(112) @align(8)   # x86_64-linux-gnu
  @0    next_in    ?[u8; avail_in]    size 8  align 8
  @8    avail_in   c_uint              size 4  align 4
  @12   <pad 4>
  @16   total_in   c_ulong             size 8  align 8
  ...
```

For records that use bit addressing (§4.9), the report prints bit offsets
and shows which storage unit each field falls in:

```writ-fragment
  @bit 48   flags      u3     unit byte 6       bits 48..50
  @bit 51   frag_off   u13    units bytes 6–7   bits 51..63   (straddles)
```

### 4.9 Bit-level layout and bit-slices

Bits show up at the boundary in several forms. The DSL handles each one
explicitly:

| Form | Example | Construct |
|---|---|---|
| C bitfields | `unsigned ro : 1;` | `bits N` (compiler rules) |
| Fields packed into one integer by hand | flags in a `u32` | `as packed { ... }` |
| Records addressed at bit granularity | protocol headers, register maps | `layout explicit bits` with `@bit(N)` |
| Inline arrays of sub-byte elements | 2-bit codes, 4-bit pixels in a struct | `[u2; 64]` inside a bit-addressed record |
| Bit-slices passed through the ABI | bitmaps, bit-stream buffers | `*[bit in U order; n from s]` |

#### Bit order and storage units

Once bits cross a byte boundary in memory, two conventions have to be
stated. Value-level views (`as packed`) don't need them. Everything that
addresses bits *in memory* does:

- **Storage unit**: the integer type bits are grouped into (`u8`, `u32le`,
  `c_ulong`, ...). Units are laid out in array order. A unit's own bytes
  follow its type's byte order. Plain types use the platform's.
- **Bit order**: how bits are numbered inside each unit's *value*.

| Order | Bit 0 is | Used by |
|---|---|---|
| `lsb0` | The least significant bit of unit 0 | Linux bitmaps (`in c_ulong lsb0`), DEFLATE, most little-endian hardware registers |
| `msb0` | The most significant bit of unit 0 | Network protocols, MPEG/H.264 bitstreams, ASN.1 PER (`in u8 msb0`) |

Multi-bit fields follow the same order. Under `msb0`, a field's most
significant bit is at its lowest bit address. Under `lsb0`, its least
significant bit is. That is the conventional reading in each case, and it
makes straddling fields well-defined. One consequence: in a record using
`in u8 msb0`, a byte-aligned `u16` or `u32` is big-endian (network order).
In one using `in u8 lsb0`, it is little-endian. No separate endianness
annotation is needed.

#### C bitfields (compiler rules)

Their placement is set by the compiler and differs between GCC/Clang and
MSVC: allocation unit, order, and whether fields may straddle a unit. Write
`bits N`, and optionally assert the bit offset within the record with
`@bit(N)`. `verify` checks it with a probe on each platform:

```writ-extended
struct Mode {
    readonly: u32 bits 1   @bit(0)
    kind:     u32 bits 3   @bit(1)
    _:        u32 bits 28                # unnamed padding bits
}
```

#### Fields packed by hand into one integer

The library uses shifts and masks itself. This is not a C bitfield, so it
doesn't depend on the compiler. It is a logical view over one integer's
*value*: bit 0 is the least significant, whatever the byte order.

```writ-extended
struct Header {
    word: u32 as packed {
        present:  bool  @0
        readonly: bool  @1
        level:    u4    @4..7          # unsigned
        delta:    i8    @8..15         # sign-extended
        _:              @16..31        # reserved; preserved on write
    }
}
```

#### Bit-addressed records

`layout explicit bits` places every field at a bit offset from the start of
the record, using the record's storage unit and bit order. Fields can have
any width from 1 to 64 bits. They can start at any bit and straddle unit
boundaries:

```writ-extended
struct IPv4Header layout explicit bits in u8 msb0 @size(20) {
    version:   u4                 @bit(0)
    ihl:       u4                 @bit(4)
    dscp:      u6                 @bit(8)
    ecn:       u2                 @bit(14)
    total_len: u16                @bit(16)
    ident:     u16                @bit(32)
    flags:     u3 as FragFlags    @bit(48)
    frag_off:  u13                @bit(51)      # straddles bytes 6 and 7
    ttl:       u8                 @bit(64)
    proto:     u8 as IpProto      @bit(72)
    checksum:  u16                @bit(80)
    src:       [u8; 4]            @bit(96)
    dst:       [u8; 4]            @bit(128)
}
```

- `uN` and `iN` (any N from 1 to 64) and one-bit `bool` are valid in
  bit-addressed records and `packed` views. Logical views (`as Enum`) work
  as usual.
- Byte-aligned fields can still be written with byte offsets (`@8`). The
  two forms can be mixed, because `@N` means `@bit(8N)`.
- **Inline sub-byte arrays**: `codes: [u2; 64] @bit(32)` puts element *i*
  at bit `32 + 2i`, in the record's bit order.
- A `layout c` record can't use `@bit` for anything except asserting C
  bitfield positions. Bit addressing needs `layout explicit bits`, because
  the C compiler doesn't do it.

**Access width.** Hardware registers and some shared flag words must be
read and written in whole units of a fixed width. Narrower accesses fault or
have side effects. `access u32` on a record or field makes generators use
aligned loads and stores of exactly that width. Writing a sub-unit field
becomes read-modify-write of its unit, preserving the other bits:

```writ-extended
struct UartCtrl layout explicit bits in u32le lsb0 @size(4) access u32 live {
    enable:   bool  @bit(0)
    parity:   u2 as Parity  @bit(1)
    baud_div: u12   @bit(8)
    _:        u12   @bit(20)         # reserved; written back unchanged
}
```

Fields that share a storage unit share one memory location. Concurrent
writes to *different* fields of the same unit race, just as C bitfields do.
Bindings treat a unit as the unit of synchronization. They use
`access atomic` (atomic fetch-or / fetch-and / compare-and-swap on the
unit) or a lock. They never update a field on its own.

#### Bit-slices

A bit-slice is a run of bits in memory. A pointer can't address a bit, so
at the ABI a bit-slice is a **shape** (§6) of up to three slots: a base
pointer, a starting bit offset, and a length in elements. Like other
shapes, a binding hides the derived slots and passes one bit-slice value.

```writ-fragment
*[bit in U order; n]             # n bits starting at bit 0 of the pointee
*[bit in U order; n from s]      # n bits starting at bit offset s
*[uW in U order; n]              # n packed W-bit elements (W from 1 to 64)
```

Examples:

```writ-extended
# Linux-style bitmaps: unsigned long words, LSB-first in each word
fn bitmap_set(map: inout *[bit in c_ulong lsb0; nbits from start],
              start: c_uint, nbits: c_uint)
fn find_first_bit(addr: *[bit in c_ulong lsb0; size], size: c_ulong) -> c_ulong
  # result is a bit index into addr, or `size` if none

# a bit-stream reader: a struct whose fields describe a bit-slice
struct BitReader {
    buf: *[bit in u8 msb0; len * 8 - pos from pos]   # the unread bits
    len: usize                                       # bytes
    pos: usize                                       # bits consumed
}

# 4-bit pixels, two per byte, high nibble first
fn blit4(dst: out *[u4 in u8 msb0; w * h], src: *[u4 in u8 msb0; w * h],
         w: c_int, h: c_int)
```

- The pointer's underlying extent is the number of whole units covering
  `[s, s + n·W)`. Generators compute it to check bounds and size copies.
- `in U order` can be left out when the library declares a default with
  `bit_order in u8 lsb0` (for example).
- All the usual contract facts apply: modes, spans, `identity`, overlap.
  Two bit-slices *overlap* if they share any unit, not just any bit,
  because a write to one rewrites the whole unit.
- **Bit pointers**: a lone "pointer to a bit" (byte pointer + bit index) is
  a bit-slice of length 1: `*[bit in u8 msb0; 1 from idx]`.

How languages lift bit-slices comes from their profiles (§9.2). Rust uses
`bitvec::BitSlice<U, O>` (or generated accessors). C++ uses a generated
`bit_span` and `std::bitset` for fixed sizes. Java uses a `BitSet` copy or
an accessor view. Python uses an accessor object over a `memoryview`. Where
a language has nothing native, the generator emits get/set accessors, and
writes are read-modify-write that preserve neighboring bits.

---

## 5. Lifetime and ownership

### 5.1 The six questions

Every pointer that crosses the boundary has six answers. The DSL gives each
one a place:

| Question | Where it's answered | Default |
|---|---|---|
| Can it be null? | `?T` vs `*T` | non-null |
| How much memory is behind it? | `*T` (one) or `*[T; n]` (n) | one element |
| Who reads and who writes? | mode: `in` / `out` / `inout` | `in` |
| How long may the other side use it? | **span**: `for call`, `until ...`, `with x`, `static` | `for call` for params; **no default for returns** |
| Who frees it, with what? | `own ... release F`, `consume ... alloc A`, handle's `release` | nobody (borrowed) |
| How must the target pass it? | `identity`, `live`, `may overlap` (§5.8) | copying allowed; no overlap for writable args |

There is deliberately **no default span for returned pointers** or for
pointers written through `out` params. Getting those wrong is the most
common FFI bug, so you have to state them. An importer writes `unknown` there.

`?` only ever means "nullable" (`?T`, `?Db`). "Not yet determined" is always
spelled `unknown`, so you can search for it and it can't be confused with
nullability.

### 5.2 Parameter modes

| Mode | Meaning | Caller's obligation |
|---|---|---|
| `in` (default) | Callee reads during the call | Keep it alive and unchanged until the call returns |
| `out` | Callee writes. Initial contents are ignored. | Provide storage |
| `inout` | Callee reads, then writes | Provide initialized storage |
| `consume` | The call ends the caller's right to use this value | Never use it again (see below) |
| `keep` | Borrowed, but the callee **retains** the pointer after returning | Keep it alive until the stated span ends |
| `moved` | C++ `T&&`: callee may take the contents, object stays alive (§8.5) | Still destroy it; don't rely on its contents |

`consume` and `keep` must be written explicitly. Neither can be a default.

`consume` describes what the call does to the *caller*. After the call, the
caller's copy of the value is dead, whatever the callee did with it:

- **Freed it:** `free`, `fclose`, `sqlite3_finalize`, `config_free`.
- **Took ownership:** the callee adopts a buffer and frees it later. Add
  `alloc A` to say which allocator the memory must come from.
- **Replaced it:** `realloc`. The old pointer is dead, and the result is
  the new value.

Generators enforce this on the target side. A wrapper object is marked dead
after the call. A Rust binding takes the argument by value. A C++ binding
takes an rvalue or calls `release()`.

If the value is consumed only when the call succeeds, say so with
`on error x: not consumed`:

```writ
fn setvbuf(
    stream: File,
    buf:    keep ?[c_char; size] until fclose(stream),
    mode:   c_int,
    size:   usize,
) -> c_int error ret != 0

fn my_lib_adopt_buffer(
    buf: consume *[u8; len] alloc malloc,  # callee will free() it
    len: usize,
)

fn realloc(ptr: consume ?void alloc malloc, size: usize) -> own ?void release free
  error ret == null and size != 0
  on error ptr: not consumed               # old block is still valid on failure

fn fclose(f: consume File) -> c_int error ret != 0
  # File is dead even on failure, so no `on error` clause
```

### 5.3 Spans

A span says how long a pointer stays valid **for the side that did not
allocate it**.

```writ-fragment
for call                       # only during this call (default for params)
static                         # forever; nobody frees it
static per thread              # forever, but only on the thread that obtained it
with x                         # valid while object x is alive (released when x is)
until EVENT[, EVENT...]        # valid until any of the listed events happens
unknown                        # not yet determined: generators refuse to emit safe code
```

An event is a call involving a specific object:

```writ-fragment
until sqlite3_finalize(stmt)
until step(stmt), reset(stmt), finalize(stmt)   # any of these
until release(stmt)                             # whatever stmt's handle release is
until next sqlite3_errmsg(db)                   # next call to the same function
until next strerror() on thread                 # next call on the same thread only
until next any(db)                              # next call of anything taking db
```

Events happen on any thread unless they are marked `on thread`. Thread
spans are about whether a pointer is still valid. Whether you may *call*
a function from a given thread is a separate question, covered by `thread`
in §6.5. When a binding can't track an event precisely, the safe default is
to copy the data out right after the call (§12.4).

### 5.4 Returned and produced memory

Three forms:

```writ-fragment
-> own T release F         # caller owns it; must call F exactly once
-> T with x                # borrowed from x; dies with x
-> T until EVENT...        # borrowed; dies at the event
-> T static                # never freed
```

The same suffixes apply to `out` params that receive a pointer:

```writ
fn sqlite3_open_v2(
    filename: *cstr,
    ppDb:     out *Db,                  # Db is a handle, so caller owns it by default
    flags:    c_int as OpenFlags,
    zVfs:     ?cstr,
) -> c_int
  error ret != SQLITE_OK
  # NOTE: sqlite sets *ppDb even on failure; the caller must still release it
  on error ppDb: own                    # state the error-path ownership too

fn sqlite3_errmsg(db: Db) -> *cstr until next sqlite3_errmsg(db), release(db)

fn sqlite3_column_text(stmt: Stmt, i: c_int)
    -> ?[u8; sqlite3_column_bytes(stmt, i)]
       until step(stmt), reset(stmt), finalize(stmt), sqlite3_column_text16(stmt, i)

fn sqlite3_mprintf(fmt: *cstr, ...) -> own ?cstr release sqlite3_free
```

`on error` lets a function state different contracts on the failure path.
These come up all the time: "`*out` is set only on success" and "`*out` is set
even on failure". `on error x: unset` means nothing was written through `x`.
`on error x: own` means the caller still owns, and must release, what was
written.

### 5.5 Allocators

`release F` names a release function. `alloc A` names the allocator a
consumed pointer must come from. Allocator domains are declared once:

```writ
allocator malloc       alloc malloc        release free
allocator sqlite       alloc sqlite3_malloc release sqlite3_free
```

`release sqlite` and `release sqlite3_free` mean the same thing. Generators use
allocator domains to copy data into the right heap before a `consume`.

### 5.6 Lifetime chosen at runtime (policy parameters)

Some APIs take an argument that decides the lifetime of another argument.
`sqlite3_bind_text` is the classic example:

```writ-extended
fn sqlite3_bind_text(
    stmt: Stmt,
    i:    c_int,
    text: *[c_char; n >= 0 ? n : until 0]  span by dtor,
    n:    c_int,
    dtor: policy {
        SQLITE_STATIC    => text: keep until release(stmt), reset(stmt), next bind(stmt, i)
        SQLITE_TRANSIENT => text: for call            # sqlite copies it
        fn(*void)        => text: consume release dtor  # sqlite calls dtor(text) later
    },
) -> c_int error ret != SQLITE_OK
```

A generator may pick one arm, usually `SQLITE_TRANSIENT` because it is
simplest and safe, or expose all of them.

### 5.7 Fields

Records use the same vocabulary, relative to whoever owns the record:

```writ
struct Config {
    name:   own *cstr release free        # freeing Config means freeing name
    parent: ?Config with self            # back-pointer, borrowed
    label:  *cstr static
}

fn config_free(c: consume *Config)        # deep release semantics come from field contracts
```

**Arrays of pointers.** An array's elements carry their own contracts, so
the container and its contents can have different owners. GIR calls these
cases `transfer full`, `transfer container` and `transfer none`:

```writ
# transfer full: caller owns the array AND each string; g_strfreev frees both
fn g_strsplit(s: *cstr, delim: *cstr, max: c_int)
    -> own *[own *cstr; until null] release g_strfreev

# transfer container: caller owns the array, but the strings belong to the table
fn g_hash_table_get_keys_as_array(t: HashTable, len: out *c_uint)
    -> own *[*cstr with t; *len] release g_free

# transfer none: nothing is handed over
fn g_get_system_data_dirs() -> *[*cstr static; until null] static
```

When an element is `own` but has no `release`, the container's release
function frees the elements too. If the elements must be freed one at a
time, give them their own `release`, and the generator frees them before
the container.

**Arenas and graphs.** No new construct is needed. A structure whose parts
are all freed together is owned by one handle (the arena, the document, the
graph). Every interior pointer is `with` that owner, however the parts point
at each other, cycles included:

```writ
handle Doc = *xmlDoc release xmlFreeDoc

struct xmlNode {
    parent:   ?xmlNode with doc
    children: ?xmlNode with doc
    next:     ?xmlNode with doc
    doc:      *xmlDoc   with doc
}

fn xmlDocGetRootElement(doc: Doc) -> ?xmlNode with doc
fn xmlUnlinkNode(n: *xmlNode)       # node now belongs to nobody...
fn xmlFreeNode(n: consume *xmlNode) # ...so the caller must free it
```

A generator only has to keep the owner alive while any interior pointer is
reachable. Ownership that moves between owners node by node (like
`xmlUnlinkNode`) is stated on the functions that move it, not on the graph.

### 5.8 Address, visibility, and overlap

The modes say which way data flows. They don't say whether the target may
pass a **copy** instead of its own storage. Target languages differ here.
Some pass by value, some by reference, some copy in and copy out (Ada
`in out`, Fortran with non-contiguous arrays), and some have a garbage
collector that moves objects. Three facts about the C side decide what is
legal:

```writ-fragment
identity              # C cares about the address itself
live                  # the memory's contents matter while the call is running
may overlap x[, y]    # this argument is allowed to share memory with x (and y)
```

**`identity`**: C uses the address, not just the bytes behind it. It may store
the pointer, compare it with another pointer, use it as a lookup key, or
return it. The target must pass its real storage, and that storage must not
move for the stated span. `keep` and `consume` (without `alloc`) imply
`identity`. Write it explicitly for the other cases:

```writ-extended
fn hashmap_insert(m: Map, key: *void identity, val: *void) -> c_int   # keyed by address
fn list_contains(l: List, node: *Node identity) -> bool              # compares pointers
```

**`live`**: some code can observe or change the contents while the call is
still running. Usually that's a callback, sometimes another thread or a
signal handler. A copy would be stale on one side or the other. For example,
C writes progress into a buffer, and a progress callback reads it:

```writ-extended
fn transfer(
    status: inout *Status live,              # C updates it; on_progress reads it
    on_progress: Progress for call,
    ud: ?void userdata on_progress,
) -> c_int
```

`live` implies the target passes its real storage for the duration of the
call. It does not imply anything about after the call; that is still the
span's job.

**`may overlap`**: by default, a writable argument (`out`, `inout`) must not
share any memory with another pointer argument of the same call. Read-only
arguments may share memory with each other freely. `may overlap` grants
permission to share:

```writ-extended
fn memcpy(dest: out *[u8; n], src: *[u8; n], n: usize) -> *void with dest
fn memmove(dest: out *[u8; n] may overlap src, src: *[u8; n], n: usize) -> *void with dest

# in-place allowed: output may be the same buffer as the input
fn aes_ctr(out: out *[u8; n] may overlap in, in: *[u8; n], n: usize, key: *Key)
```

The default is "no overlap" because that is the safe assumption when a
generator can't prove otherwise. When the target language could hand C
overlapping storage (two slices of one array, say) and the contract doesn't
allow it, the generator must copy one argument or reject the call.

`may overlap` is a permission, not a requirement. Copying an argument that
may overlap is always still correct. If the C code actually *depends* on two
arguments being the same memory, that is `identity`, not overlap.

### 5.9 Passing mechanisms

Generators choose how to pass each argument. These rules say which choices
are legal for a contract. A generator should pick the cheapest legal option
for its language.

| Mechanism | What the target does | Legal when |
|---|---|---|
| **By value** | Passes the bits; C parameter is not a pointer | The physical type is not a pointer. Nothing to decide. |
| **Copy-in** | Makes a temporary, passes its address, discards it after | `in`, not `identity`, not `live`, span is `for call` |
| **Copy-out** | Passes an uninitialized temporary, copies the result back after | `out`, not `identity`, not `live`, span is `for call` |
| **Copy-in/copy-out** | Temporary initialized from caller, copied back after | `inout`, not `identity`, not `live`, span is `for call` |
| **By reference** | Passes the address of the target's own storage | Storage has the exact C layout, doesn't move for the span, and no forbidden overlap with other arguments |
| **Pinned by reference** | Same, but the target first pins a movable (GC) object | Required for `identity`, `live`, and `keep` arguments in moving-GC languages |
| **Allocate-and-hand-over** | Copies into allocator `A`'s heap, then passes that | `consume ... alloc A` on plain data (not `identity`) |

Notes:

- **Copy-back extent.** Copy-out and copy-in/copy-out copy back only the part
  that is valid after the call. That is `len` if the extent has one,
  otherwise the whole extent. Copy-back on the error path follows the
  `on error` clause: `unset` means don't copy back, and
  `on error x: partial` means only `len` elements are valid.
- **Promoting `out` to a return value.** An `out` argument that copy-out could
  pass can become a return value (or one of several) in the high-level API.
  `out` arguments that are `identity` or `live` can't.
- **Struct by value.** `p: Point` and `-> Point` are passed by value under
  the C calling convention. A target that cannot pass or return aggregates
  by value may use a generated C shim that converts to a pointer. This is
  always legal, because a value has no address C can depend on.
- **Consumed handles.** Consuming a handle is always `identity`. The target
  passes the real handle and then marks its wrapper dead. Only plain data
  with `alloc A` can be copied before handing it over.
- **Strings and slices** that need conversion (re-encoding, adding a NUL) are
  copy-in by nature. If such an argument is also `identity`, `live`, or
  `keep`, the generator must allocate the converted copy with a matching
  lifetime and keep it alive as long as the span requires.

A field without a span inherits `with self`: it is valid as long as the
containing record is.

---

## 6. Shapes: one logical value, several ABI slots

Shapes are dependent annotations. A pointer's count or a callback's context
**names another slot**. The generator then hides the dependent slot in the
high-level API and fills it in automatically.

### 6.1 Array views

```writ-fragment
*[T; n]                  # exactly n elements; n is a sibling param/field or expression
*[T; cap c, len l]       # capacity c on input, l elements valid on output
*[T; until 0]            # sentinel-terminated (any value; `null` for pointer arrays)
*[T; n * size]           # expressions: + - * /, sizeof(T), ternary, field access
```

Rules:

- If the count is a plain sibling param, a high-level binding **hides that
  param**. A slice argument fills in both slots.
- `x.in` and `x.out` refer to an `inout` slot's value before and after the call.
- `*x` dereferences a pointer slot: `len *outLen`. That is shorthand for `outLen.out`
  when `outLen` is `out`.
- A count may be a call: `sqlite3_column_bytes(stmt, i)`. The called
  function must be declared `pure`, meaning it has no side effects that
  matter to the binding. Lint rejects a count that calls anything else.
  The generator evaluates a parameter's count before the call. It evaluates
  a returned extent immediately after the call, before making any other call
  through the binding.

  ```
  fn sqlite3_column_bytes(stmt: Stmt, i: c_int) -> c_int  pure
  ```

Common idioms:

```writ
# (ptr, len) input slice
fn write(fd: c_int, buf: *[u8; count], count: usize) -> isize error ret < 0 via errno

# caller buffer + capacity in + written-length out
fn read(fd: c_int, buf: out *[u8; cap count, len ret], count: usize) -> isize
  error ret < 0 via errno

# size-query / two-call pattern
fn snprintf(buf: out ?[c_char; cap n, len min(ret, n - 1)], n: usize, fmt: *cstr, ...) -> c_int
  sizing query(buf = null, n = 0) needs ret + 1

# argv-style null-terminated pointer array
fn execv(path: *cstr, argv: *[*cstr; until null]) -> c_int error ret == -1 via errno

# array of descriptors, each with its own slice
struct iovec { iov_base: *[u8; iov_len], iov_len: usize }
fn writev(fd: c_int, iov: *[iovec; iovcnt], iovcnt: c_int) -> isize error ret < 0 via errno
```

**Sizing protocols.** `sizing` says how a caller finds out how big an
output buffer must be. There are three protocols:

| Protocol | Meaning |
|---|---|
| `sizing needs E` | The capacity is known before the call: `E` is an expression over the inputs, and may call `pure` functions (`sizing needs compressBound(sourceLen)`) |
| `sizing query(args) needs E` | Calling with the given argument substitutions (usually a null buffer and zero capacity) returns the needed size `E`, then the caller allocates and calls again |
| `sizing retry needs E` | Every call succeeds and writes as much as fits. `E` is the total, and a buffer of that size holds everything. |

Generators that return complete results emit the matching loop. When a
function has more than one capacity-bounded output, the clause names the
one it applies to: `sizing retry for out needs ret`.

**Empty arrays and null.** `*[T; n]` is non-null **even when `n` is 0**.
Callees that build slices from (pointer, length) pairs require it; Rust's
`slice::from_raw_parts` is one. Generators pass a dangling, aligned,
non-null pointer for an empty array. Two forms relax the rule:

| Form | Null allowed |
|---|---|
| `*[T; n]` | Never |
| `*[T; n] null if empty` | Only when `n == 0` |
| `?[T; n]` | Always: null means the array is absent |

**Reinterpreted elements.** When a library flattens records into a
scalar array, count the array in records and give the physical pointer
type with `as`:

```writ-fragment
struct Span { start: usize, end: usize }

out: out *[Span; cap / 2] as *usize
```

The checker requires `Span`'s size to be an exact multiple of `usize`'s,
with no padding.

A truncating output under `sizing retry` looks like this:

```writ
fn resharp_find_all(r: Regex, input: bytes[len] null if empty, len: usize,
                    out: out *[Span; cap cap / 2, len min(ret, cap / 2)] as *usize,
                    cap: usize) -> isize
  sizing retry needs ret
```

**Shorthand.** The commonest shapes have short spellings. They expand in
the lowered form, and `fmt` keeps the spelling the author used:

| Shorthand | Means |
|---|---|
| `bytes[n]` | `*[u8; n]` |
| `utf8[n]` | `*[u8; n] as utf8` |
| `out bytes[cap -> len]` | `out *[u8; cap cap, len len]` |

### 6.2 Strings

```writ-fragment
cstr          = [c_char; until 0] with library encoding     (use as *cstr)
cstr(bytes)   NUL-terminated, no encoding (opaque bytes)
utf8[n]       counted UTF-8, not terminated                 (§6.1)
```

Counted text in another encoding is an array with a view:
`*[c_char; n] as latin1`.

`*cstr` passed `in` means: the generator must add a NUL terminator and must
reject interior NULs (or say what it does with them).

### 6.3 Callbacks and user data

```writ
callback Cmp = fn(a: *void, b: *void) -> c_int
  thread same                  # invoked on the calling thread
  unwind no                    # must not throw/panic across

fn qsort(
    base:   inout *[u8; nmemb * size],
    nmemb:  usize,
    size:   usize,
    compar: Cmp for call,
)

callback Handler = fn(ud: *void userdata, ev: *Event) -> void
  thread any
  reentrant no

fn bus_subscribe(
    bus: Bus,
    cb:  keep Handler until bus_unsubscribe(bus, ret), release(bus),
    ud:  ?void userdata cb,
) -> SubId
```

- `userdata cb` marks the `void *` context that goes with callback `cb`. A
  generator combines the two into one closure argument and routes the
  context to the closure.
- Inside the callback, `userdata` marks the slot that receives it.
- A callback's span (`for call`, `keep until ...`, `once`) decides how long
  the generator must keep the closure alive. `once` means it is invoked
  exactly once and then released.
- Callback-only attributes: `thread same|any|main`, `reentrant yes|no`,
  `unwind no` (the default; generators must catch and convert exceptions).

**Functions the consumer provides.** A callback is a function the consumer
implements and hands over by pointer. Some functions are instead provided
by the consumer *by symbol*: plugin entry points, hooks a library looks up
by name. Those go in an `interface` provided by the client (§9.1):

```writ
interface plugin provided by client {
    fn plugin_init(host: *HostApi, argv: *[cstr; argc], argc: c_int) -> c_int
      pack rest argv
      symbol "myplugin_init"
}
```

Every contract fact means the same thing whichever side provides a
declaration. Spans and ownership always describe the obligations of
whoever *calls* it.

### 6.4 Errors

The `error` clause on a function gives the failure condition, and `via`
says where the detail comes from:

```writ-fragment
error ret != SQLITE_OK                     via sqlite3_extended_errcode(db)
error ret == null                          via errno
error ret < 0                              via -ret as Errno
error err != null                          via err own release g_error_free
error ret == -1 and errno != EINTR         retry                  # generators may loop
```

`err` here refers to an `out ??GError` param. A generator turns this into
exceptions, `Result`, or multiple returns.

**Error channels.** Many libraries report error detail through a side
channel, separate from the return value: a thread-local message, `errno`,
`GetLastError`, a message tied to a handle. Declare the channel once:

```writ
error_channel errno per thread {
    value errno_location()
}

error_channel last_error per thread {
    text resharp_last_error(buf, cap)
    valid until next failure on thread
}

error_channel db_message per handle Db {
    text sqlite3_errmsg(db)
    valid until next any(db)
}
```

- `value` channels yield a value; `text` channels yield a message. A `text`
  function's buffer follows its own `sizing` clause in the usual way.
- `per thread` and `per handle H` say what the detail is attached to.
  `valid until` says when it is overwritten.
- `via` names either a channel (`error ret == null via last_error`) or an
  ordinary expression (`via -ret as Errno`). For a channel, generators
  fetch the detail immediately after the failing call, before anything
  else can overwrite it.
- `reset before call` marks a channel whose stale value would be mistaken
  for a new failure. `errno` is the classic case, for APIs where only
  `errno != 0` afterwards signals an error. Generators clear it before
  each call that uses it:

  ```
  error_channel errno per thread reset before call {
      value errno_location()
  }

  fn strtol(s: *cstr, end: out *?c_char with s, base: c_int) -> c_long
    error errno != 0 via errno
  ```

**Outcomes.** Some calls have more than two results that matter: several
kinds of success, or several kinds of failure with different rules about
their outputs. `outcome` declares them, in order. The first whose `when`
predicate holds is the call's outcome:

```writ
fn inflate(strm: inout *z_stream, flush: c_int) -> c_int
  outcome done     success when ret == Z_STREAM_END
  outcome progress success when ret == Z_OK
  outcome stalled  failure when ret == Z_BUF_ERROR  on stalled strm: partial
  outcome corrupt  failure when ret == Z_DATA_ERROR via strm.msg
  outcome failed   failure via ret as ZStatus
```

- `error P via X` is shorthand for a failure outcome `error` with
  predicate `P`, followed by an implicit success outcome `ok`.
- `on <outcome> x: ...` states what output `x` holds for one outcome.
  `on error x: ...` applies to every failure outcome.
- An outcome may carry `retry`, which makes the call again, up to the
  profile's retry limit.
- The outcomes must cover every result: the last one has no predicate, or
  the predicates are exhaustive over a closed enum view.

Bindings present several success outcomes as a result that carries the
outcome (an enum in Rust, a tagged result in n00b), and several failure
outcomes as distinct error kinds.

### 6.5 Effects and calling constraints

Functions carry two kinds of extra facts:

- **Effects**: what calling the function may *do*.
- **Constraints**: the conditions under which it may be *called*.

**Effects** are an open set. A few are built in and have defined meaning to
generators. Anyone can declare more:

```writ-extended
# built in: these have meaning to generators
#   blocking   may block the calling thread (release the GIL, keep off event loops)
#   noreturn   never returns
#   alloc      may allocate
#   none       no effects at all (with `pure`, also deterministic)

# user-declared: carried through to the lowered form and to bindings
effect input                     # reads from the outside world
effect output                    # writes to the outside world
effect io = input + output       # a named set
effect network implies io        # implication: network includes io
effect fs implies io

fn read(fd: Fd, buf: out *[u8; cap n, len ret], n: usize) -> isize
  effects input, blocking
fn write(fd: Fd, buf: *[u8; n], n: usize) -> isize
  effects output, blocking
fn sqlite3_exec(db: Db, sql: *cstr, cb: ?ExecCb, ud: ?void userdata cb, err: out *?cstr)
  effects io, blocking, of cb    # plus whatever cb does
fn strlen(s: *cstr) -> usize
  effects none  pure
```

- `effects of cb` makes a function's effects include its callback's. A
  callback type can also *limit* what the supplied callback may do:
  `callback Cmp = fn(...) -> c_int effects none` says the comparison
  function must have no effects. Bindings for languages that track effects
  enforce this. Other bindings document it.
- A function with no `effects` clause has **unknown** effects, which is not
  the same as `none`. Lint can require a clause on every function.
- User-declared effects mean nothing to the core generators. They exist so
  that bindings, language profiles and policy tools can act on them. A Rust
  binding might require a capability token for `output`. A sandboxing tool
  might reject `network` in a given module. An audit can list every function
  with `fs`.
- Effects can be declared in a shared file and pulled in with `use`, so a
  project has one vocabulary across all its libraries.

**Constraints** have fixed meanings:

```writ
fn gtk_widget_show(w: Widget)       thread main
fn sqlite3_close_v2(db: consume Db) thread any
fn pthread_mutex_unlock(m: *Mutex)  thread same_as lock(m)    # same thread that locked it
fn write(...)                       signal_safe
fn qsort(...)                       reentrant no
```

| Constraint | Values |
|---|---|
| `thread` | `any`, `main`, `same` (the caller's thread for a callback), `same_as EVENT` |
| `reentrant` | `yes`, `no` (must not be re-entered from a callback or another thread) |
| `signal_safe` | May be called from a signal handler |
| `unwind` | `no` (default), or `yes` (§8.5) |

### 6.6 Variadic and keyword arguments

A variadic or keyword parameter list is a **language-level** idea. Python
has `*args` and `**kwargs`. Swift, Java, Kotlin and C# have `T...` (which
are really arrays). JS has rest parameters and options objects. C++ has
`initializer_list` and variadic templates. Rust has none of these at the
function level.

The C ABI has exactly one native variadic mechanism, C's `...` (with its
companion `va_list`), and almost no language lowers its own varargs to it.
So the DSL treats variadic and keyword arguments as **packs**. A pack is a
logical parameter, like a slice (§6.1). It is carried by one of several
physical **lowerings**, and generators **lift** it back into whatever each
language uses natively.

Packs work in both directions:

- **Import:** a target language calls a function implemented in C (or C++,
  or any language exporting a C ABI). The generator lowers the caller's
  native varargs or kwargs into the declared form.
- **Export:** a function is implemented in the target language and called
  through the C ABI. This includes callbacks (§6.3) and declarations in
  `interface ... provided by client` blocks (§9.1). The generator lifts the declared form into the
  implementation's native varargs or kwargs.

The same contract drives both directions, so two languages can talk
through a pack without either of them knowing the other's conventions.

#### Rest packs (positional varargs)

```writ-extended
# homogeneous array: the most common lowering, and what most languages use internally
fn sum(xs: *[f64; n], n: usize) -> f64
  pack rest xs

# heterogeneous: an array of tagged values
struct Value { kind: c_int as ValueKind, v: union by kind { ... } }
fn call(fn_id: c_int, argv: *[Value; argc], argc: c_int) -> Value
  pack rest argv

# C variadic, homogeneous, sentinel-terminated
fn execl(path: *cstr, ... of *cstr until null) -> c_int error ret == -1 via errno
  pack rest ...

# C variadic, count given by an earlier argument
fn sum_ints(n: c_int, ... of c_int count n) -> c_long
  pack rest ...

# C variadic, types decided by a format string
fn sqlite3_mprintf(fmt: *cstr, ...) -> own ?cstr release sqlite3_free
  format printf(fmt)
  pack rest ...

# va_list: the same pack, already captured by a C caller
fn sqlite3_vmprintf(fmt: *cstr, ap: va_list) -> own ?cstr release sqlite3_free
  format printf(fmt)
  pack rest ap
```

A rest pack must have a known element type and a known end:

| Lowering | Element type from | End from |
|---|---|---|
| `*[T; n]` array | `T` | `n` |
| `*[T; until s]` array | `T` | the sentinel |
| `*[Value; n]` tagged array | each element's tag | `n` |
| `... of T count n` / `until s` | `T` | the count or sentinel |
| `... format printf(fmt)` | parsing `fmt` | parsing `fmt` |
| `va_list` | same as its `...` form | same as its `...` form |

A bare `...` with none of these is opaque. It gets only a raw binding, in
both directions.

#### Keyword packs

C has no keyword arguments, so libraries lower them in a handful of
recurring ways. A `keywords` record states which one:

```writ-extended
# options struct with a presence bitmask (bit i = field i was supplied)
keywords OpenOpts presence mask(present) {
    present:    u32
    timeout_ms: u32  = 30000
    readonly:   bool = false
    cache_kb:   u32
}
fn db_open(path: *cstr, opts: ?OpenOpts) -> own ?Db
  pack keywords opts

# options struct versioned by a leading size field (Win32 cbSize style)
keywords WinOpts presence size(cbSize) { cbSize: u32, flags: u32, ... }

# each field has a value meaning "not supplied"
keywords Limits presence sentinel { max_rows: i64 = -1 none -1, name: ?cstr }

# parallel name/value arrays
fn configure(names: *[*cstr; n], values: *[Value; n], n: usize)
  pack keywords (names, values)

# C variadic name/value pairs, null-terminated (g_object_new style).
# The first key is a fixed parameter; its value and the remaining pairs
# arrive through `...`.
fn g_object_new(type: GType, first: ?cstr, ... pairs(*cstr, by_name) until null)
    -> own GObj
  pack keywords (first, ...)
```

Presence encodings:

| Encoding | Meaning |
|---|---|
| `presence mask(f)` | Bit *i* of field `f` says whether keyword *i* was supplied |
| `presence size(f)` | Field `f` holds the struct size the caller knows about. Keywords past that size are absent. |
| `presence sentinel` | Each keyword's `none` value (§4.6) means "not supplied". Pointers use null. |
| `presence always` | Every keyword is always passed. The binding fills in the defaults. |

`by_name` means a value's type depends on its key. The generator needs a
key → type table to lower or lift it, given as `keys { "name": *cstr, "width": c_int, ... }`.
Without one, the pack can't be checked and gets only a raw binding.

A function can have a positional pack, a keyword pack, both, or several
ordinary parameters around them. Ordinary parameters with `= default`
(§8.6) are a third, simpler source of optional arguments, and need no pack.

#### How each language lifts a pack

| Lowering | Python | Rust | Swift | Java / Kotlin | JS | C++ |
|---|---|---|---|---|---|---|
| Rest, homogeneous | `*args` | `&[T]` / `impl IntoIterator<Item=T>` | `T...` | `T...` / `vararg` | `...rest` | `initializer_list<T>`, `span<const T>` |
| Rest, tagged | `*args` with conversion | `&[Value]` + `From` impls | `Any...` | `Object...` | `...rest` | variadic template converting to `Value` |
| Keywords record | keyword-only params | builder or `Opts { ..Default::default() }` | labeled params with defaults | builder | options object | designated initializers |
| Keyword pairs | `**kwargs` | builder | labeled params | builder / `Map` | options object | builder |

#### Bridging C's `...` and `va_list`

These two lowerings need special handling, because of what languages can
actually do with them:

| Direction | Case | Mechanism |
|---|---|---|
| Import, `...` | Number of arguments known at compile time (macros, variadic templates, generated code) | Call directly using the variadic calling convention. Rust, Swift, Zig, C++ and others can all *call* a C variadic function. |
| Import, `...` | Number of arguments known only at run time (Python `*args`) | Dynamic variadic call. libffi's `ffi_prep_cif_var`, or an equivalent per-target thunk. |
| Import, `va_list` | Any | A generated C shim `f_shim(fixed..., ...)` does `va_start` and calls `vf(fixed..., ap)`. The shim is then called as in the two rows above. |
| Export, `...` | Target implements a C variadic function | Few languages can *define* one (Rust only on nightly). A generated C shim with the `...` signature walks the arguments with `va_arg`, using the pack's element types and end, then calls the target-side function with the array form. |
| Export, `va_list` | Target implements a `va_list` consumer | The same shim technique, starting from `ap`. |

Rules that apply to every C variadic lowering:

- **Default argument promotions.** Integers narrower than `int` become
  `int`, and `float` becomes `double`. `... of f32` or `... of u8` is
  rejected by lint. Write `... of f64` or `... of c_int`.
- **Variadic calling convention.** It differs from the fixed-argument one on
  Apple arm64 (variadic arguments go on the stack) and on x86_64 SysV (`%al`
  must hold the vector-register count). A generator must never declare a
  variadic function as if it took fixed arguments.
- **`va_list` layout is per target.** It is an array of one struct on x86_64
  SysV, and a plain pointer on Apple arm64 and Windows. It is passed by
  pointer or by value accordingly. It is declared as the built-in type
  `va_list` and never written out by hand.
- **Format strings.** For an import with `format printf`, the safe wrapper
  formats in the target language. It then calls the C function as
  `f("%s", formatted)`, so a format string from the caller can never reach
  C. A wrapper that passes arguments through is generated only on request,
  and only when the format string is a compile-time constant the generator
  can check.

### 6.7 Labels

A **label** is metadata on a value. It names what the value *is*, apart
from its type. Labels never change layout, type, or generated calls.
Bindings and tools decide what each label means to them.

```writ-extended
label path            # a filesystem path
label secret          # must not be logged, persisted, or left in memory
label fd_number       # an integer that is a file descriptor (but not owned here)
label untrusted       # came from outside; validate before use

fn open(path: *cstr label path, flags: c_int, ...) -> own ?Fd error ret == none via errno
fn getpass(prompt: *cstr) -> *cstr static per thread  label secret
struct Creds { user: [c_char; 64], password: [c_char; 64] label secret }
fn fileno(f: File) -> c_int label fd_number
fn recv(s: Socket, buf: out *[u8; cap n, len ret] label untrusted, n: usize, flags: c_int) -> isize
```

What bindings do with labels:

- A Rust binding maps `path` to `&Path` / `PathBuf` instead of `&CStr`, and
  `secret` to a zeroizing buffer.
- A Python binding maps `path` to `os.PathLike`.
- A taint-tracking or audit tool follows `untrusted` and `secret`.

None of this is in the contract. The contract only says *this cstr is a
path*.

Labels, logical views (`as`), and handles are different tools:

- If the binding should get a **distinct type**, use `as View` (no
  ownership) or a `handle` (with ownership), as in §4.3.
- If it should get **extra meaning** that some tools care about, use a label.

A file descriptor the caller must close is `handle Fd`. A descriptor number
that is only reported, like `fileno`'s result, is a `c_int` with
`label fd_number`.

Like effects, labels can be declared once in a shared file and pulled in
with `use`.

---

## 7. Names and symbols

### 7.1 Four kinds of name

One entity has up to four names, and they need to stay separate:

| Layer | C example | C++ example | Used by |
|---|---|---|---|
| **Contract name** | `kv_open` | `Db.get` | This file: `refine`, span events, the lowered form's key |
| **Source name** | `kv_open` | `kv::Db::get(const char*) const` | The importer, the mangler, `verify` |
| **Link symbol** (per platform) | `kv_open` | `_ZNK2kv2Db3getEPKc` (Itanium), `?get@Db@kv@@...` (MSVC) | The linker, `dlsym`, Rust `#[link_name]` |
| **Target-language name** (per binding) | Rust `Db::open`, Python `kv.open` | Rust `Db::get`, Python `Db.get` | Generated code |

The first three are facts about the boundary and live in the contract. The
fourth is a choice one binding makes. It lives in that binding's file
(§9.3), not in the contract, so adding a language never touches the
contract.

Defaults:

- `lang c`: the source name is the contract name, and the symbol is the
  source name.
- `lang c++`: the symbol is derived from the source name and signature
  using the platform's mangling scheme (Itanium or MSVC).
- `lang rust`: the symbol is the generated transport's `extern "C"` name,
  or the rustc-derived symbol under direct linking (§8.9).
- Target-language names follow the language profile's naming rules
  (§9.2), adjusted by the binding.

Symbols are written without the platform's assembler-level decoration (the
extra leading `_` on Mach-O). That is the form `dlsym` and Rust `link_name`
expect.

### 7.2 Syntax

In the contract, the source name and the symbol can each be overridden,
per platform where it matters:

```writ
fn kv_open(path: *cstr, out_db: out *Db) -> c_int
  source "kv_open"                # default; shown for completeness
  symbol "kv_open"                # default
```

Target-language names are set in a binding:

```writ
binding kv for rust import {
    strip_prefix "kv_"
    name kv_open "Db::open"       # becomes an associated fn on Db
}
```

### 7.3 Why C needs this too

Even in plain C, the name in the header is often not the symbol you link
against:

```writ
# a macro that expands to a per-target function call
fn errno_location() -> *c_int static per thread
  source "errno" macro
  symbol "__errno_location" when libc == glibc
  symbol "__error"          when os == macos
  symbol "_errno"           when os == windows

# the header redirects to a variant symbol
fn stat(path: *cstr, buf: out *Stat) -> c_int error ret != 0 via errno
  symbol "stat$INODE64" when os == macos and arch == x86_64

# A/W pairs on Windows: the header macro chooses; the contract must choose too
fn CreateFile(name: *cstr(utf16), ...) -> own ?Handle release CloseHandle
  symbol "CreateFileW"

# versioned symbols
fn memcpy(...)
  symbol "memcpy" version "GLIBC_2.14" when libc == glibc

# inline function or macro: there is nothing to link
fn bswap32(x: u32) -> u32
  symbol none                     # generators must emit a C shim (§8.1)
```

**Macro entry points.** Some public "functions" are macros that call a real
symbol with extra fixed arguments. zlib's `deflateInit(strm, level)` is
really `deflateInit_(strm, level, ZLIB_VERSION, sizeof(z_stream))`.
`expands` declares the function callers see, and how it reaches the
symbol:

```writ
fn deflateInit(strm: inout *z_stream, level: c_int) -> c_int
  expands deflateInit_(strm, level, ZLIB_VERSION, sizeof(z_stream))
```

Bindings expose `deflateInit` and call `deflateInit_`. The importer
produces `expands` automatically from function-like macros that forward
to a real symbol.

Global variables use `var` (§7.4). Their symbols vary just as much:

```writ
var stdout: File
  symbol "__stdoutp" when os == macos
  symbol "stdout"    when libc == glibc
```

### 7.4 Global symbols

Global data is part of the boundary too, and it has its own lifetime and
access questions. `var` declares a data symbol:

```writ-extended
var zlib_version: [c_char; until 0] const                 # read-only data
var optind: c_int  mut                                    # getopt state; no synchronization
var environ: *[cstr; until null] mut  live
var errno_value: c_int  mut  per thread                   # true TLS symbol
var plugin_table: [PluginEntry; 16] const  provided by client  symbol "myplugin_table"
var sqlite3_temp_directory: ?cstr mut  own release sqlite3_free  since 3.0
var __stack_chk_guard: uptr  weak                          # may be absent at link time
```

| Attribute | Meaning |
|---|---|
| `const` / `mut` | Whether anyone may write it after load. `const` data may be cached by bindings. |
| `live` | Others (other threads, the library itself) may change it at any time, so every read goes to memory (§5.8). |
| `per thread` | A TLS symbol. Access goes through the platform's TLS model (`__tls_get_addr`, Mach-O TLV descriptors, the Windows TLS index), which generators must use instead of a plain address. |
| `weak` | The symbol may be missing. The binding exposes it as optional and checks for null at run time. |
| `valid after f()` | The value isn't meaningful until an initialization call has been made. |
| `own ... release F` | The global owns what it points to. Writing a new value means releasing the old one with `F`. |
| `provided by client` | The *consumer* defines this symbol (plugin tables, `main`, hooks that a library looks up by name). See §9.1. |
| `effects of read/write` | Effects of reading or writing it, for globals with side effects (memory-mapped registers, hooks) |

Bindings expose a `var` as an accessor (`get` / `set` functions or a
property), never as a bare address. Atomic or locked access is stated with
`access atomic` or `access locked_by f()/g()`. Without one of those, a `mut`
global gets no synchronization, and the binding marks it unsafe or
single-threaded.

Function symbols can be declared without a contract too, purely for linking
(to force a dependency, take an address for `dlsym`-style comparisons, or
re-export one). Write `symbol fn name` or `symbol var name`. These get a
raw address and nothing else.

### 7.5 In the lowered form

```json
"names": {
  "contract": "Db.get",
  "source":  {"lang": "c++", "qualified": "kv::Db::get",
              "signature": "(const char*) const"},
  "symbol":  {"x86_64-linux-gnu": "_ZNK2kv2Db3getEPKc",
              "aarch64-apple-darwin": "_ZNK2kv2Db3getEPKc",
              "x86_64-pc-windows-msvc": "?get@Db@kv@@..."},
  "provider": "library",
  "provenance": {"symbol": "derived"}
}
```

### 7.6 Verification

For each target, `verify` checks that:

- every symbol exists in the library's export table (`nm`, `dyld_info`,
  `dumpbin`),
- it is the right kind (function or data),
- the symbol version resolves,
- for C++, demangling the symbol gives back the source name and signature.

Derived symbols are never trusted until checked, because the details matter.
One example is libstdc++'s `[abi:cxx11]` tag (`B5cxx11` in the mangled
name), which appears on functions that return `std::string`. A mismatch is
reported with all four names side by side.

---

## 8. C++ and Rust libraries

Libraries written in C++ or Rust are described in their own terms, and the
C ABI is the transport between them and their consumers. Nobody has to
hand-write an `extern "C"` layer.

- **C++ (§8.1–§8.8):** link directly against existing symbols where the
  Itanium or MSVC ABI makes that safe. Generate shims everywhere else.
- **Rust (§8.9):** Rust has no stable ABI, so a generated transport is the
  default. Direct linking is an optimization, using facts derived from the
  exact compiler build.

### 8.1 Direct linking vs. generated shims

Each declaration takes `via direct`, `via shim`, or `via auto` (the default).
`auto` picks direct linking when all of the following hold, and a shim
otherwise:

| Condition | Why |
|---|---|
| The function has a symbol (not inline-only, not an uninstantiated template) | Nothing to link otherwise |
| It is not called through virtual dispatch (§8.6) | A symbol call skips the vtable |
| The calling convention matches C's | True on all 64-bit Itanium and MSVC x64 targets. Not true for 32-bit MSVC `thiscall`. |
| Every by-value argument and the return value is either trivial, or the generator implements §8.4 | Non-trivial types are passed indirectly |
| It is `noexcept`, or the target can carry a C++ exception safely (§8.5) | Unwinding through Rust frames is otherwise undefined behavior |

A shim is a generated `extern "C"` C++ function. It has the same contract,
and its own symbol replaces the original. The contract doesn't change, only
the `via` and the symbol do. `verify` reports which path each declaration
took and why.

**Rollout.** Calling a C++ library from Rust by linking straight to its
symbols, outside a monorepo build, is the part no existing tool does
portably. Crubit comes closest, and it is tied to Bazel. So the first
generators treat `auto` as `shim` everywhere. Like cxx, they generate
`extern "C"` wrappers, but driven by these contracts. `via direct` is turned
on in stages. Each stage starts only once `verify` can prove the symbol,
calling-convention and layout facts it depends on for every target:

| Stage | Linked directly |
|---|---|
| 0 | Nothing. Shims everywhere. |
| 1 | Free functions and static methods whose arguments are all trivial, `noexcept` |
| 2 | Non-virtual methods, constructors and destructors (with pinning for non-relocatable classes) |
| 3 | Non-trivial types passed and returned by value (§8.4) |
| 4 | Virtual calls through asserted vtable slots (§8.6) |
| Never | `throws` functions, inline-only functions, uninstantiated templates. These always use shims. |

None of this changes the contract format.

### 8.2 Classes

```writ-extended
lang c++
header <kv/db.hpp>

class Db = kv::Db @size(48) @align(8) {
    fields opaque              # layout not exposed; size and align still asserted
    trivial no                 # has a non-trivial copy/move ctor or dtor
    relocatable no             # must not be moved with memcpy
    copy no                    # copy ctor deleted
    move no

    ctor open(path: *cstr)                          source "Db(const char*)"
    dtor
    method get(this: in, key: *cstr) -> own ?cstr release free
                                                    source "get(const char*) const"
    method put(this: inout, key: *cstr, val: *[u8; n], n: usize)
      source "put(const char*, const unsigned char*, size_t)"
      error ret != 0
    static version() -> *cstr static
    virtual flush(this: inout) slot 2
}
```

Class properties are imported from clang and asserted like layout:

- **`trivial`**: trivially copyable and trivially destructible. A trivial
  class behaves exactly like a C struct. It can be passed by value, copied
  with memcpy, and declared with real fields.
- **`relocatable`**: the object may be moved to a new address with a plain
  byte copy. Rust moves are always byte copies, so a non-relocatable class
  (one that points into itself, such as libstdc++'s `std::string`, or one
  registered by address somewhere) must stay at one address. Rust holds it
  as `Pin<Box<T>>`, and every pointer to it is `identity` (§5.8).
- **`copy`** and **`move`**: which special members exist. These decide
  whether the generator can offer `Clone`, and which constructor symbols to
  link.

`this` is an explicit first parameter. Its mode follows C++ const-ness: a
`const` method takes `this: in`, a non-const method takes `this: inout`.

### 8.3 Constructors, destructors, `new` and `delete`

At the ABI level these are ordinary functions:

```writ-extended
ctor open(path: *cstr)
  # lowers to: fn(this: out *Db identity, path: *cstr)
  # Itanium C1 "complete object" symbol: _ZN2kv2DbC1EPKc
dtor
  # lowers to: fn(this: consume *Db identity)
  # ends the object's life; does NOT free its storage
  # Itanium D1 symbol: _ZN2kv2DbD1Ev
```

- A constructor builds the object in place in storage the caller provides.
  That storage must already be at the object's final address, so it is
  `identity`. Rust allocates `Box<MaybeUninit<Db>>` using the asserted size
  and alignment, calls the constructor, then pins the result.
- A destructor consumes the object but not its storage. The caller still
  frees the storage separately.
- Objects that a library function returns from `new` have to be released
  with `delete`. Declare that with `-> own *Db release delete`. That links
  the Itanium D0 "deleting destructor" symbol when it exists. Otherwise the
  generator emits a shim. The `cxx_new` allocator domain covers `consume`
  of heap objects.

### 8.4 By-value arguments and returns of non-trivial types

The C++ ABIs pass non-trivial types indirectly:

- **Itanium:** for a non-trivial argument, the caller builds a temporary,
  passes its address, and destroys the temporary after the call.
- **MSVC:** the same, except the callee destroys the argument.
- **Both:** a non-trivial return value goes through a hidden pointer. The
  callee constructs the result at the address the caller supplies.

The contract still says only `w: Widget` or `-> Widget`. The generator
derives the mechanism from `trivial` plus the target's ABI. In §5.9 terms:

- a non-trivial by-value argument is copy-in, but the copy must be made
  with the class's copy or move constructor, never with memcpy;
- a non-trivial return is `out`, at its final address (`identity`).

If the generator can't call the needed constructor directly, it falls back
to `via shim`.

### 8.5 References, moves and exceptions

| C++ | Contract | Notes |
|---|---|---|
| `T&` | `inout *T` | Non-null. Same span defaults as any pointer parameter. |
| `const T&` | `in *T` | |
| `T&&` | `moved *T` | The callee may take the contents. The caller **still owns the object and must destroy it**. This is unlike `consume`. |
| `std::unique_ptr<T>` by value | `consume own *T release delete` | Its layout is one pointer, but it is non-trivial, so Itanium passes it indirectly (§8.4) |

Exceptions are per-function:

- `noexcept` is the default for `lang c`.
- `throws` is the default for `lang c++` unless the header says `noexcept`.

A `throws` function is linked directly only if the target can carry a
foreign unwind (for Rust, `extern "C-unwind"`). Even then the target usually
can't *catch* it. Normally the generator routes `throws` through a shim that
catches and converts to an error, using the same `error` clause machinery
as §6.4:

```writ-extended
method put(this: inout, key: *cstr, val: *[u8; n], n: usize)
  throws {
    std::bad_alloc     => error ENOMEM
    kv::KeyError       => error KV_NOTFOUND via e.what()
    std::exception     => error KV_ERR via e.what()
  }
```

### 8.6 Virtuals, overloads, templates, defaults

- **Virtual methods.** Calling the symbol directly is only correct when the
  object's exact type is known (`final`). Otherwise the generator either
  calls through the vtable or uses a shim. On Itanium the vtable pointer is
  at offset 0, and the slot index is asserted with `slot N`. The default is
  a shim.
- **Overloads.** Each overload is its own declaration with its own contract
  name (`put_bytes`, `put_str`). The source signature tells them apart for
  mangling.
- **Templates.** Only instantiations that exist in the binary can be linked.
  List them with `instance kv::Cache<int>`. Anything else needs a shim,
  which instantiates the template itself.
- **Default arguments.** These are not part of the ABI. `p: c_int = 3`
  records the default as a source-level fact, so generators can offer the
  same default in the target language.

**Implementing a C++ interface in the target language.** Some APIs make
*you* subclass an abstract class: listeners, visitors, allocators. Mark that
class `implementable`. The generator then emits a C++ shim subclass. Each
virtual method in it forwards to a target-language callback (§6.3) through a
userdata pointer:

```writ-extended
class Listener = kv::Listener implementable {
    virtual on_event(this: inout, ev: *Event) -> void  slot 2
    dtor virtual
}

method subscribe(this: inout, l: keep *Listener until unsubscribe(this, l))
```

The shim's destructor releases the target-side object, so ownership follows
whatever the C++ side does with the `Listener`. Generating vtables directly,
without a C++ shim, is out of scope. It would mean reproducing RTTI,
offset-to-top and deleting destructors for each ABI. That costs a lot and
buys very little over a shim.

### 8.7 Standard library types

`std::` layouts depend on the **standard library implementation**
(libstdc++, libc++, MSVC STL), not on the compiler. They are declared per
stdlib and verified like any other layout.

View types fit the array-descriptor idiom from §6.1 directly. Note that the
field order differs between implementations:

```writ-extended
struct StrView = std::string_view trivial {
    when stdlib == libstdcxx { len: usize,  data: *[c_char; len] }
    else                     { data: *[c_char; len], len: usize }   # libc++, MSVC
}

method find(this: in, key: StrView) -> ?Entry with this
# Rust signature: fn find(&self, key: &str) -> Option<&Entry>
```

Owning containers (`std::string`, `std::vector`, `std::map`) are
non-trivial, and some are non-relocatable. The recommended form is
`fields opaque` plus generated accessor shims (`data()`/`size()`), or
conversion at the boundary.

### 8.8 What direct linking produces for Rust

For the `Db` class above on `x86_64-linux-gnu`, the generator emits roughly
this:

```rust
#[repr(C, align(8))]
pub struct Db { _storage: [u8; 48], _pin: core::marker::PhantomPinned }

unsafe extern "C" {
    #[link_name = "_ZN2kv2DbC1EPKc"]
    fn kv_Db_ctor(this: *mut Db, path: *const c_char);
    #[link_name = "_ZN2kv2DbD1Ev"]
    fn kv_Db_dtor(this: *mut Db);
    #[link_name = "_ZNK2kv2Db3getEPKc"]
    fn kv_Db_get(this: *const Db, key: *const c_char) -> *mut c_char;
}

impl Drop for Db { fn drop(&mut self) { unsafe { kv_Db_dtor(self) } } }
```

The generated wrapper methods are `Db::open(&CStr) -> Pin<Box<Db>>` and
`Db::get(&self, &CStr) -> Option<CString>`. `put` and `flush` don't appear
in the block, because `throws` and `virtual` sent them through generated
shims.

### 8.9 Rust libraries (`lang rust`)

Rust gives a binding nothing stable to link against:

- **Calling convention.** Functions not declared `extern "C"` use rustc's
  unspecified calling convention, which can change between compiler
  versions.
- **Layout.** The layout of most types, including `&str`, `Option`,
  `Result` and enums, is unspecified. The guarantees are narrow:
  `repr(C)`, `repr(transparent)`, and the null niche in `Option<&T>` and
  `Option<Box<T>>`.
- **Symbols.** Mangled names carry per-build hashes.
- **Existence.** Generic and `#[inline]` functions exist only where
  something instantiates them, and LTO can remove or internalize symbols.

What Rust *does* have is a signature that already states most contract
facts: ownership, borrowing, lifetimes, nullability, errors and thread
safety. A `lang rust` contract is therefore written in Rust's own types,
and is mostly imported (§12.2).

#### Describing a Rust API

```writ
library resharp
lang rust
crate resharp "0.7.6"
feature capture_groups = cargo "experimental_capture_groups"

struct Match { start: usize, end: usize }
enum UnicodeMode { Ascii, Default, Full, Javascript }
opaque RegexOptions
opaque Regex
error resharp::Error display

impl RegexOptions {
    fn default() -> RegexOptions
    fn unicode(self, mode: UnicodeMode) -> RegexOptions
    fn hardened(self, on: bool) -> RegexOptions
}

impl Regex {
    fn new(pattern: &str) -> Result<Regex, resharp::Error>
    fn with_options(pattern: &str, options: RegexOptions) -> Result<Regex, resharp::Error>
    fn is_match(&self, haystack: &[u8]) -> Result<bool, resharp::Error>
    fn find_all(&self, haystack: &[u8]) -> Result<Vec<Match>, resharp::Error>
    fn find_anchored(&self, haystack: &[u8]) -> Result<Option<Match>, resharp::Error>
}
```

Rust types map onto the contract vocabulary directly:

| Rust | Contract meaning |
|---|---|
| `&T`, `&self` | `in`, `for call` |
| `&mut T`, `&mut self` | `inout`, `for call` |
| `T` by value, `self` (not `Copy`) | `consume` |
| `&str` / `&[T]` | `utf8[n]` / `*[T; n]` |
| `String`, `Vec<T>` returned | Owned buffer, released by a generated free function |
| `Option<T>` | `?T` / `none` |
| `Result<T, E>` | `error`, with detail from `E` (below) |
| A returned lifetime tied to an argument (`fn f<'a>(&'a self) -> &'a T`) | `with self` |
| `Box<T>`, or any non-`repr(C)` type returned by value | A single-owner handle |
| `Arc<T>` | A refcounted handle (clone retains, drop releases) |
| `Send + Sync` / `Send` only / neither | `shared` / the default / `thread_affine` (§4.3) |
| `repr(C)` structs, primitives, fieldless enums | Passed by value as C types |
| Panics | Never cross the boundary (`unwind no`) |

`opaque` types cross as handles. `struct` types whose fields are all
transportable cross by value, as a C struct copy. `error E display` says
`E`'s `Display` text becomes the error detail. Generic items cross only
when listed as `instance`s.

#### The transport (default)

writ generates a crate, `<crate>-writ`, compiled together with the library
by Cargo. It contains one `extern "C"` function per contract function,
named `writ_<crate>_<item>`:

- `&str` and `&[T]` arrive as pointer and length (non-null even when
  empty, §6.1);
- owned results become handles with generated release functions;
- `Result` becomes a status plus a generated per-thread error channel
  carrying the `Display` text (§6.4);
- every entry point catches panics and reports them as errors;
- `Send`/`Sync` facts become the handles' thread rules.

The transport has its own C contract, which writ derives from the Rust
contract. Every consumer binds to that derived contract, so an n00b
binding, a C header and a Python binding all treat a Rust library the
same way they treat a C one.

#### Direct linking (optimization)

For a hot function, the transport's conversions can be skipped by calling
rustc's own symbol. Every fact then comes from **the exact build**:

- symbol names and lowered signatures come from the library built with
  `--emit=llvm-ir`;
- layouts come from a generated probe crate (`size_of`, `align_of`,
  `offset_of!`);
- the library must be built as an `rlib` or `staticlib` with the symbol
  reachable, not internalized by LTO.

These facts are tied to one compiler version and one set of build flags,
so they are re-derived on every build. Any mismatch falls back to the
transport. LLVM-based consumers, n00b included, can link the library's
bitcode and inline across the boundary. `via direct` / `via auto` work as
for C++ (§8.1): `auto` uses the transport until direct linking has been
verified for that function and build.

---

## 9. Languages, bindings, and build

### 9.1 Providers, consumers, and interfaces

A contract describes a boundary without a direction. It says which side
**provides** each declaration, and nothing about which language sits on
either side.

- By default, the **library** provides everything in a contract.
- An `interface ... provided by client` block holds declarations the
  *consumer* must provide: plugin entry points, hooks looked up by name,
  globals the library expects someone else to define.
- A callback type is a one-function interface, provided by whoever supplies
  the pointer.

```writ
library audio_host
interface host provided by library {          # the default, written out
    fn host_log(level: c_int, msg: *cstr)
    fn host_alloc(n: usize) -> own ?void release host_free
}
interface plugin provided by client {
    fn plugin_init(host_version: u32) -> c_int
    fn plugin_process(buf: inout *[f32; n], n: usize)  thread any
    var plugin_name: [c_char; until 0] const
}
```

A binding chooses a **role** for each interface. It **imports** an
interface it consumes, and **exports** one it provides. A Rust plugin
imports `host` and exports `plugin`. A C++ host imports `plugin` and
exports `host`. Both are generated from the same contract.

### 9.2 Language profiles

A **language profile** is a data file that describes one language's side of
the C ABI. It covers how that language represents and converts the things
the contract talks about. Generators read profiles, and so do tools that
reason across languages. The tools ship profiles for C, C++, Rust, Python,
Java, Go, Swift, JS (Node-API), and C#. Projects can extend or override them.

This is the "database of languages and idioms". It is deliberately kept
small and declarative. A profile does not generate code. The generator for
that language does, guided by the profile.

| Topic | What the profile records |
|---|---|
| **Strings** | The native string representation(s), and how each converts to every contract string form: `cstr`, `utf8[n]`, `cstr(utf16)`, labeled `path`. Whether the conversion copies, allocates, re-encodes, or can be zero-copy. |
| **Slices and arrays** | Native contiguous types and whether they can be borrowed without copying |
| **Packs** | Native forms of rest and keyword packs, which lowerings it can lift directly, and whether it can *call* and *define* C variadic functions (§6.6) |
| **Memory model** | `none` (manual), `rc` (refcounting), `gc nonmoving`, or `gc moving`. Pin and unpin operations, keep-alive operations, finalizers. |
| **Errors** | Exceptions, result types, or multiple returns, and how `error` clauses map onto them |
| **Concurrency** | Global locks (GIL) and how to release them for `blocking`, thread attachment (JNI `AttachCurrentThread`), event loops |
| **Naming** | Case conventions, reserved words, how module and namespace layout is derived |
| **Defaults for labels and effects** | e.g. `path` → `&Path` in Rust, `os.PathLike` in Python |

**Strings in particular.** The contract never states a language's string
ABI. `cstr` describes only the C side: NUL-terminated, in some encoding,
owned by someone. Most languages don't use NUL-terminated strings
internally. Rust `&str` and Go `string` are pointer + length UTF-8. Java and
C# strings are UTF-16 objects managed by the GC. Python `str` has its own
internal representation. Each profile says how its strings become what the
contract asks for, and what that costs:

| Language | `in *cstr` | `in *[u8; n] as utf8` (counted) |
|---|---|---|
| Rust | Copy to add NUL (`CString`), or zero-copy from `&CStr` | Zero-copy from `&str` |
| Go | Copy (`C.CString`) | Pin the string data for the call, or copy |
| Python | Encode to `bytes` (cached UTF-8 on `str`) | Same buffer, no NUL needed |
| Java (Panama) | `Arena.allocateFrom(String)` | Encode to a `MemorySegment` |

When both sides of an API can be designed, prefer counted strings. Every
non-C language can produce them more cheaply.

**Pinning: the contract says when, the profile says how.** For a GC
language, the contract's span says how long C may hold memory. The profile
supplies the operations to pin and unpin it:

```writ
profile java {
    memory gc moving
    pin    for call  via "Linker.Option.critical(true)"   # heap access during a downcall
    pin    longer    via copy                             # no long-term pinning: copy to an Arena
    keep_alive       via "Reference.reachabilityFence"
}
profile csharp {
    memory gc moving
    pin    for call  via "fixed"
    pin    longer    via "GCHandle.Alloc(Pinned)" release "GCHandle.Free"
}
profile go {
    memory gc moving
    pin    any       via "runtime.Pinner.Pin" release "runtime.Pinner.Unpin"
}
profile python {
    memory rc + gc nonmoving
    pin    buffer    via "PyObject_GetBuffer" release "PyBuffer_Release"
}
```

The generator combines the two. It pins when the contract requires the real
storage, and releases the pin exactly when the span ends:

| Contract span | When the binding releases the pin |
|---|---|
| `for call` | Right after the call returns. A scoped form is used if the profile has one. |
| `keep until EVENT` | When the binding performs or observes `EVENT` (e.g. inside its wrapper for `fclose`) |
| `with x` | When the wrapper for `x` is released |
| `static` | Never. The profile should copy into non-moving memory instead. |

If §5.9 allows a copy and the profile marks pinning as expensive or
unavailable for that span (`via copy`), the binding copies instead.

### 9.3 Bindings: import and export

A **binding** is one language's choices for one contract, in one
direction. Import and export bindings are separate, because they make
different choices:

- An **import** binding decides how to *present* things to callers in the
  target language.
- An **export** binding decides how to *connect* an implementation in the
  target language to the contract.

**Import binding**: the target language calls something another side
provides.

```writ-extended
binding sqlite3 for rust import {
    module  "sqlite"
    strip_prefix "sqlite3_"

    # grouping and names
    type Db   as "Connection"
    type Stmt as "Statement"
    method Db.prepare = sqlite3_prepare_v2
    method Stmt.step  = sqlite3_step
    name   sqlite3_open_v2 "Connection::open"

    # idiom choices the contract leaves open
    error   SQLITE_*        -> "sqlite::Error"            # Result<_, sqlite::Error>
    borrow  sqlite3_column_text  as lifetime              # tie to &Statement, don't copy
    policy  sqlite3_bind_text    use SQLITE_TRANSIENT     # pick one arm (§5.6)
    keywords OpenOpts            as builder
    label   path                 -> "&std::path::Path"

    # surface control
    hide    "sqlite3_*16*"                                 # UTF-16 variants not exposed
    raw     "sqlite3_some_unknown_api"                     # expose unsafe raw binding despite `unknown`
}
```

**Export binding**: the target language implements something others call.

```writ-extended
binding audio_host for rust export {
    provide plugin.plugin_init    = "crate::init"
    provide plugin.plugin_process = "crate::process"
    provide plugin.plugin_name    = "crate::NAME"

    on_panic    abort                  # or: return error value; never unwind (§6.3)
    allocator   own -> host_alloc      # memory returned as `own` must come from the declared allocator
    keywords    *   as struct
}
```

An export binding also covers **implementing a library in another language
against a contract**. A Rust crate can expose a C ABI that a contract
already specifies. The export binding maps each declaration to a Rust
function, and the generator emits the `#[no_mangle] extern "C"` layer. That
layer converts arguments, maps errors to codes, catches panics, and
allocates `own` results from the allocator the contract names.

Rules:

- A binding may not contradict the contract. It can only choose among the
  options the contract leaves open, or narrow them (expose fewer functions,
  pick one policy arm, copy where borrowing is allowed). `verify` checks
  every binding against its contract and its language profile. For
  example, `borrow ... as lifetime` is rejected for a language whose profile
  has no lifetimes.
- One contract can have any number of bindings: per language, per
  direction, or several per language (a thin raw binding and an idiomatic
  one).
- Choices that are the same for every binding of a language belong in the
  profile. Choices for one library belong in its binding.

### 9.4 Other ABI families

The C ABI is one **ABI family**. The design separates what depends on the
family from what doesn't:

| Family-specific | Family-neutral (reused unchanged) |
|---|---|
| Primitive types and layout rules | Modes, spans, ownership, `consume`, `identity`/`live`/overlap |
| How symbols are named and found | Shapes and packs |
| Calling mechanism | Effects, constraints, labels |
| Built-in handle kinds and error channel | Providers, interfaces, bindings, profiles, `unknown`, provenance |

A contract states its family with `abi`. `abi c` is the default and covers
`lang c` and `lang c++`. Planned families:

- **`abi jvm`**: a linked or embedded JVM, reached through JNI or Panama
  upcalls. Types are JVM types. Symbols are class + method + descriptor.
  Local references are borrowed `until` the native frame returns, and
  global references are `own ... release DeleteGlobalRef`. Pending
  exceptions are the error channel. "Thread must be attached" is a
  constraint.
- **`abi cpython`**: Python callables in an embedded or linked interpreter.
  `PyObject*` is a refcounted handle (new reference = +1, borrowed = +0;
  §4.3). "Holds the GIL" is a constraint. `NULL` + `PyErr` is the error
  channel. Rest and keyword packs map directly onto `*args` / `**kwargs`.
- Candidates: Wasm component model, Swift's stable ABI, Objective-C
  messaging.

```writ-extended
abi jvm
class CRC32 = java.util.zip.CRC32 {
    ctor new()
    method update(this: inout, b: jarray[i8], off: i32, len: i32)  pack rest b
    method getValue(this: in) -> i64  effects none
}
```

v1 implements only `abi c`. The others are designed for, but not built. The
test of the design is that adding a family means writing its type, symbol
and calling layers, while the lifetime, shape and effect layers stay as
they are.

### 9.5 Build integration

The tools are one CLI, `writ`, with these subcommands:

| Subcommand | Does |
|---|---|
| `import` | Reads headers and produces a contract draft |
| `lint` | Checks a contract for unknowns, conflicts and dead declarations |
| `verify` | Checks layout, symbols and bindings against the real toolchain and binary |
| `lower` | Produces the canonical JSON form |
| `gen` | Generates a binding's code |
| `check` | Fails if checked-in generated code is stale |

Generation is deterministic and cached on a hash of the contract, the
profile, the binding and the toolchain.

**No compiler-driver hook.** `writ` does not plug into `cc` or `rustc`. It
integrates at the build-system level, as bindgen and protoc do:

| Build system | Integration |
|---|---|
| Cargo | A `writ-build` crate for `build.rs`. It reads `TARGET` and the C compiler and flags via the `cc` crate. |
| CMake | `find_package(writ)` and `writ_add_binding(<target> CONTRACT ... LANG ... ROLE ...)`. Uses CMake's compiler, sysroot and flags. |
| Meson | A `writ` module wrapping `custom_target` |
| Bazel | `rules_writ` |
| Python | Hooks for scikit-build-core, maturin, setuptools `build_ext` |
| JVM / Swift / Go / .NET | Gradle plugin, SwiftPM build-tool plugin, `go generate`, MSBuild task |

**Toolchain facts come from the build, never guessed.** `import` and
`verify` must see the same compiler, target triple, sysroot, defines and
include paths as the real build. Otherwise they verify a different ABI from
the one that ships. The order of preference is:

1. the build-system integration passes them in;
2. `compile_commands.json`;
3. explicit `--cc`, `--sysroot` and `--define` flags.

For builds with none of these, `writ cc -- <compiler args>` can wrap the
compiler once to record the flags. That is a fallback, not the main path.

**Generated or checked in.** Generating at build time is the default where
the build already runs native tools (C, C++, Rust, Go). Where build-time
native tooling is painful (Python wheels, npm packages), check the
generated code in and run `writ check` in CI.

---

## 10. Making it easy by hand

### 10.1 What you usually write

For most functions you write nothing beyond the C signature. Add annotations
only where they matter:

| Situation | Write |
|---|---|
| A pointer param that is read-only, borrowed, non-null | nothing |
| It can be null | `?` |
| It's an array with a length param | `*[T; len]` |
| It's a C string | `*cstr` |
| The callee writes through it | `out` / `inout` |
| The callee holds onto it after returning | `keep ... until X` |
| The caller may not use it after the call | `consume` (plus `alloc A` if the callee frees raw memory) |
| C stores, compares, or returns the address | `identity` |
| A callback (or another thread) sees it mid-call | `live` |
| An output may share memory with an input | `may overlap x` |
| It returns a pointer | `own ... release F`, `with x`, `until X`, or `static` (required) |
| It returns a status code | `error ret != OK` |
| Error detail comes from a "last error" function or `errno` | `error_channel` once, then `via` |
| Many functions share a rule | `conventions for fn pattern { ... }` |
| An empty array may be passed as null | `null if empty` |

### 10.2 `refine`: annotate imported declarations

You don't have to rewrite headers. Import the raw declarations, then refine
only the slots that need contracts:

```writ
import <sqlite3.h> raw

handle Db   = *sqlite3       release sqlite3_close_v2
handle Stmt = *sqlite3_stmt  release sqlite3_finalize

refine sqlite3_prepare_v2 {
    zSql:   *[c_char; nByte >= 0 ? nByte : until 0]
    ppStmt: out *?Stmt                         # null when zSql was only whitespace
    pzTail: out ??c_char with zSql           # points into zSql
    error ret != SQLITE_OK via sqlite3_extended_errcode(db)
}

refine sqlite3_column_blob {
    -> ?[u8; sqlite3_column_bytes(stmt, i)] until step(stmt), reset(stmt), finalize(stmt)
}
```

The parameter names in `refine` are the header's names. A refinement may
only *add* facts or *narrow* them (for example, `?` to `*`). It can never
change physical types, and the tool rejects one that tries.

### 10.3 Unknowns and notes

```writ
refine some_api {
    -> *cstr unknown                 # lifetime not determined yet
    note "docs say 'valid until next call' but unclear if per-thread"
}
```

`unknown` keeps the binding honest. A generator emits only a raw `unsafe`
binding for it, and `writ lint` lists every `unknown` in the file. `unknown`
can appear anywhere a fact is expected: a span, an extent
(`*[T; unknown]`), a mode, or an overlap permission (`may overlap unknown`).

**Fix-its.** Every `unknown`, every lint finding and every heuristic
inference comes with **suggested replacements**: a span plus the text to
put there, carried in the diagnostic. `writ fix` applies the suggestions
the author accepts. In practice, writing a contract means importing it,
reading the suggestions, and accepting or editing them. Facts are rarely
written from scratch.

### 10.4 Conventions

Libraries repeat their rules across many functions. A `conventions` block
states a rule once, for every declaration whose name matches a pattern:

```writ
conventions for fn resharp_* {
    error ret == -1 or ret == null via last_error
}

conventions for fn z* {
    error ret < 0 via ret as ZStatus
}
```

- A convention supplies defaults. A fact stated on a declaration replaces
  the convention's fact for that declaration.
- Any fact a declaration can carry can come from a convention: `error`,
  `effects`, `thread`, spans on returns of a given type.
- When several conventions match, the one with the longest literal prefix
  wins. Two that tie are an error.
- Facts that come from a convention have provenance `convention` in the
  lowered form. `writ explain fn` shows where each fact came from.

A convention only applies a rule the author wrote, so it is a fact, not an
inference.

---

## 11. The lowered form (for tools)

Every DSL file lowers to a canonical form with all defaults made explicit,
all sugar expanded, imports resolved, and one target selected. Its JSON
encoding is the interchange format generators consume:

```json
{
  "fn": "compress2",
  "params": [
    {"name": "dest", "phys": "ptr", "null": false, "mode": "out",
     "pointee": "u8", "extent": {"cap": "destLen.in", "len": "destLen.out"},
     "span": "call", "owner": "caller", "identity": false, "live": false,
     "overlap": [], "hidden": false},
    {"name": "destLen", "phys": "ptr", "null": false, "mode": "inout",
     "pointee": "c_ulong", "span": "call", "owner": "caller",
     "hidden": true, "derived_from": "dest"},
    {"name": "source", "phys": "ptr", "null": false, "mode": "in",
     "pointee": "u8", "extent": {"len": "sourceLen"},
     "span": "call", "owner": "caller", "hidden": false},
    {"name": "sourceLen", "phys": "c_ulong", "hidden": true, "derived_from": "source"},
    {"name": "level", "phys": "c_int", "hidden": false}
  ],
  "ret": {"phys": "c_int"},
  "error": {"when": "ret != 0"},
  "provenance": {"dest.extent": "manual", "level": "import"}
}
```

Properties:

- **Stable and diffable.** Keys are sorted, nothing is implicit, and
  formatting is canonical. A re-import that changes a contract shows up as a
  one-line diff.
- **Provenance.** Each fact records whether it came from `import`,
  `infer` (a heuristic, see §12.3), or `manual`. Review tools highlight
  inferred facts nobody has confirmed.
- **Round-trippable.** `writ fmt --explicit` prints the lowered form back as
  DSL.
- **Derived ABI facts.** Some generators make raw calls themselves:
  libffi-based dynamic languages, JITs, and direct C++ linking. For each
  target, the lowered form includes how each argument and the return value
  are actually passed (`"abi_class": "registers" | "memory" | "indirect"`,
  with the register split for aggregates). These facts come from clang's
  ABI lowering and are checked by `verify`. They are never written in the
  DSL source, because they follow from layout and target and cannot be
  chosen.

---

## 12. Automation

### 12.1 Pipeline

```text
header ──import──▶ raw decls ──infer──▶ draft (unknown + @inferred)
                                          │
                                   human refines
                                          ▼
            probe ◀──verify── contract file ──lower──▶ canonical JSON ──▶ generators
```

- **import** (via libclang) produces exact physical types and layout for each
  target. It also reads any contract annotations the library already has
  (§12.2). It never guesses contracts.
- **infer** applies heuristics and marks every result `@inferred`. Nothing
  inferred is trusted by default. A strict generator setting can require
  that all `@inferred` facts be confirmed.
- **verify** emits a C probe file with a `_Static_assert` for every layout
  assertion and every constant, compiles it against the real header for each
  target, and fails on any mismatch.
- **lint** reports every `unknown`, unconfirmed inferences, release functions that
  are never used, handles with no producer, and count expressions that refer
  to slots that don't exist.

### 12.2 Importing existing facts

**C and C++ headers.** The importer already parses headers with libclang, so it also reads the
contract-related clang attributes a header carries. That costs nothing extra.
These facts are recorded with provenance `import:clang`. They count as
confirmed, because the library's own authors wrote them, but lint reports
any that conflict with a heuristic or a manual fact.

v1 reads no other annotation system. GIR, SAL, and win32metadata importers
are not part of the language or the core tools. Each one would be a
separate front end that emits this DSL, and is worth building only for a
project that binds GNOME or Win32 at scale. Their vocabularies informed the
design (§16), and the plan is to validate the design against them by hand:
lower a sample of about 50 real GIR- and SAL-annotated functions into this
DSL. Anything that can't be expressed is a design gap. Writing an importer
isn't needed to find those gaps.

| Clang / Swift annotation | Contract |
|---|---|
| `_Nonnull` / `_Nullable` | `*` / `?` |
| `counted_by(n)`, `sized_by(n)` | `*[T; n]`, `*[u8; n]` |
| `ended_by(end)` | `*[T; end - p]` (a pointer range, not a sentinel) |
| `lifetimebound` on param `p` | `-> ... with p` |
| `lifetime_capture_by(x)` on param `p` | `p: keep ... with x` |
| `ownership_returns(A)`, `ownership_takes(A, i)` | `-> own ... alloc A`, param i `consume alloc A` |
| `noescape` | `for call` (explicitly) |
| `SWIFT_SHARED_REFERENCE(retain, release)` | `refcounted retain ... release ...` |
| `SWIFT_RETURNS_RETAINED` / `SWIFT_RETURNS_UNRETAINED` | `-> own` (+1) / borrowed (+0) |

The DSL keeps GIR's callback scopes and Swift's +1/+0 meanings where they
overlap with ours, so people who know those systems find familiar ground.
Sharing vocabulary doesn't require an importer.

**Rust crates** are imported from rustdoc's JSON output:

```sh
cargo +nightly rustdoc -p <crate> -- -Z unstable-options --output-format json
```

The importer reads:

- public items and signatures, with their lifetimes;
- `Send` and `Sync` implementations;
- `repr` attributes;
- `cfg(feature = ...)` gates;
- which types implement `Display`.

It writes the `lang rust` contract of §8.9. Facts come with provenance
`import:rustdoc`. The JSON format is versioned (`format_version`), and the
importer refuses versions it doesn't know. A nightly toolchain is needed
only for importing, never for building a binding.

### 12.3 Inference heuristics (suggestions, never facts)

| Pattern | Suggestion |
|---|---|
| `T *p, size_t n` adjacent, `n` named `*len`, `*size`, `*count`, `n*` | `p: *[T; n]` |
| `const char *` param | `*cstr` |
| `T **out` param, last positions | `out` |
| Function `X_free(X*)`, `X_destroy`, `X_release`, `X_close` | `handle X release ...` |
| Returns `X *` and a matching release function exists | `-> own X` |
| The release function's own argument | `consume` |
| `restrict` on a writable pointer in the header | nothing (no-overlap is already the default) |
| Writable pointer without `restrict`, next to a same-typed input | `may overlap unknown` (someone must decide) |
| Pointer stored into a struct field, or compared, in available source | `identity` |
| Function-pointer param followed by `void *` | `userdata cb` |
| Returns `int` and the library defines `*_OK` | `error ret != *_OK` |
| `const char *` return | `-> *cstr unknown` (span deliberately not guessed) |
| Refcounted handle returned by a `*Create*` / `*Copy*` function vs. a `*Get*` function (CoreFoundation rule) | `-> own` (+1) vs. borrowed (+0) |

### 12.4 What generators do with contracts

| Contract | GC language (Python, JS, ...) | Rust | C++ |
|---|---|---|---|
| `in *[T; n]` | Accept a buffer and pin it for the call | `&[T]` | `std::span<const T>` |
| `out *[T; cap c, len l]` | Allocate, call, truncate to `l` | `&mut [MaybeUninit<T>]` → `&[T]` | `std::span<T>` + returned size |
| `own T release F` | Wrapper object + finalizer + `close()` | `struct` with `Drop` | `unique_ptr` with deleter |
| `with x` | Wrapper keeps a reference to `x` | Lifetime `'x` | Comment + debug check |
| `until EVENT` | Copy out eagerly (safe default), or invalidate on event | Borrow tied to `&self`. Event methods take `&mut self`. | Copy out |
| `keep until EVENT` | Keep the object alive in a side table until the event | Lifetime or `Pin<Box<_>>` stored in owner | Owner holds a `shared_ptr` |
| `consume` (handle) | Mark the wrapper dead after the call | Take by value (move) | Take an rvalue / `release()` |
| `refcounted` handle | Wrapper retains or releases; +0 results are retained on wrap | `Clone` = retain, `Drop` = release | Intrusive smart pointer |
| `consume alloc A` (data) | Copy into `A`'s heap first | `into_raw` / allocator copy | `release()` |
| `identity` / `live` | Pin; pass real storage | `&mut T` / raw pointer, never a temp | Pass address of the real object |
| writable, no `may overlap` | Copy one side if the arguments might share memory | Borrow checker already forbids it | Debug assert that ranges don't overlap |
| callback + `userdata` | Closure trampoline | `extern "C"` trampoline + `Box<dyn FnMut>` | `std::function` trampoline |
| `unknown` | Raw binding only | `unsafe fn` only | Raw only |

---

## 13. Grammar sketch

```ebnf
file        = { directive | decl } | profile { profile } | binding { binding } ;
(* ---------- contract files ---------- *)
directive   = "library" ID | "link" STRING | "header" HEADER | "encoding" ID
            | "define" ID [ "=" expr ] | "include_path" STRING
            | "compile_commands" STRING | "use" STRING
            | "platforms" platform_list | "import" HEADER [ "raw" ]
            | "abi" ( "c" | "jvm" | "cpython" | ID ) | "lang" ( "c" | "c++" | "rust" )
            | "crate" ID STRING
            | "feature" ID "=" ( "cargo" | "define" | "configure" ) STRING
            | "version" "query" ID "(" ")" "->" type "format" STRING
            | "bit_order" bit_storage ;

decl        = const | alias | opaque | handle | record | union | enum | flags
            | callback | fn | refine | allocator | when | var | class
            | keywords | interface | effect | label | link_symbol
            | conventions | error_channel | rust_impl | rust_error ;

conventions = "conventions" "for" ( "fn" | "handle" | "struct" ) PATTERN
              "{" { fn_clause } "}" ;
error_channel = "error_channel" ID ( "per" "thread" | "per" "handle" ID )
              [ "reset" "before" "call" ]
              "{" ( "value" | "text" ) call [ "valid" "until" event { "," event } ] "}" ;
rust_impl   = "impl" ID "{" { "fn" ID rust_sig { fn_clause } | when } "}" ;
rust_error  = "error" PATH "display" ;

interface   = "interface" ID "provided" "by" ( "library" | "client" )
              "{" { decl } "}" ;
effect      = "effect" ID [ "=" ID { "+" ID } ] [ "implies" ID { "," ID } ] ;
label       = "label" ID ;
link_symbol = "symbol" ( "fn" | "var" ) ID { name_clause } ;

keywords    = "keywords" ID "presence" presence "{" { field [ "=" expr ]
              [ "none" expr ] } "}" ;
presence    = "mask" "(" ID ")" | "size" "(" ID ")" | "sentinel" | "always" ;

var         = "var" ID ":" type { var_attr | name_clause } ;
var_attr    = "const" | "mut" | "live" | "per" "thread" | "weak"
            | "valid" "after" event | "own" | "release" ID
            | "provided" "by" ( "library" | "client" )
            | "access" ( "atomic" | "locked_by" event "/" event )
            | "since" VERSION | label_use | effects ;
class       = "class" ID "=" QNAME { rec_attr } [ "implementable" ]
              "{" { class_item } "}" ;
class_item  = "fields" "opaque" | field
            | ( "trivial" | "relocatable" | "copy" | "move" ) ( "yes" | "no" )
            | "ctor" ID fn_sig { fn_clause }
            | "dtor" [ "deleting" | "virtual" ]
            | ( "method" | "static" | "virtual" ) ID fn_sig { fn_clause }
            | "instance" QNAME ;

const       = "const" ID ":" type "=" expr ;
alias       = "type" ID "=" type ;
opaque      = "opaque" ID [ "source" STRING ] ;
handle      = "handle" ID "=" ( type | "*" "opaque" ) [ "none" expr ]
              [ "refcounted" "retain" ID ] "release" ID [ "floating" "sink" ID ]
              [ "shared" | "thread_affine" ] ;
allocator   = "allocator" ID "alloc" ID "release" ID ;
record      = "struct" ID { rec_attr } "{" { field } "}" [ "none" "{" field_values "}" ] ;
rec_attr    = "layout" ( "c" | "explicit" [ "bits" bit_storage ] )
            | "@size" "(" size_list ")" | "@align" "(" INT ")" | "@pack" "(" INT ")"
            | "access" ( type | "atomic" ) | "live" | "source" STRING ;
bit_storage = "in" type ( "lsb0" | "msb0" ) ;
union       = "union" ID "{" field { "," field } "}" ;
enum        = "enum" ID ":" type "{" { ID "=" expr "," } "}" [ "closed" ] ;
flags       = "flags" ID ":" type "{" { ID "=" expr "," } "}" ;
callback    = "callback" ID "=" fn_sig { cb_attr } ;
fn          = "fn" ID fn_sig { fn_clause } ;
refine      = "refine" ID "{" { slot_refine | fn_clause } "}" ;
when        = "when" cond "{" { decl } "}" [ "else" "{" { decl } "}" ] ;
cond        = expr | "feature" ID | cond ( "and" | "or" ) cond | "not" cond ;

fn_sig      = "(" [ param { "," param } [ "," varargs ] ] [ "," ] ")" [ "->" ret ] ;
varargs     = "..." [ "of" type ( "until" expr | "count" ID )
                    | "pairs" "(" type "," ( type | "by_name" ) ")" "until" expr ] ;
param       = ID ":" [ mode ] type { slot_attr } [ "=" expr ] ;   (* "= expr": source-level default *)
ret         = [ "own" ] type { slot_attr } ;
mode        = "in" | "out" | "inout" | "consume" | "keep" | "moved" | "unknown" ;
field       = ( ID | "_" ) ":" [ "own" ] type { slot_attr } [ "@" INT ] [ "@bit" "(" INT ")" ]
              [ "access" ( type | "atomic" ) ] ;

type        = prim | ID | "va_list" | ( "*" | "?" ) type
            | ( "*" | "?" ) "[" type ";" extent "]"
            | ( "*" | "?" ) "[" ( "bit" | UINT_N ) [ bit_storage ] ";" expr [ "from" expr ] "]"
            | UINT_N | SINT_N            (* u1..u64, i1..i64: bit-addressed records and packed views only *)
            | "[" type ";" expr "]" | "cstr" [ "(" ID ")" ]
            | ( "bytes" | "utf8" ) "[" ( expr | expr "->" expr ) "]"
            | type "as" ID | type "as" "*" type
            | type "bits" INT | "union" "by" ID "{" arms "}"
            | rust_type                   (* lang rust only *)
            | "policy" "{" arms "}"
            | type "as" "packed" "{" { ( ID | "_" ) ":" [ type ] "@" INT [ ".." INT ] } "}" ;
extent      = expr | "until" expr | "cap" expr "," "len" expr | "unknown" ;

slot_attr   = span | "own" | "release" ID | "alloc" ID | "userdata" [ ID ] | "private"
            | "static" | "span" "by" ID | "@align" "(" INT ")"
            | "identity" | "live" | "may" "overlap" ( ID { "," ID } | "unknown" )
            | "null" "if" "empty"
            | label_use ;
rust_type   = "&" [ LIFETIME ] [ "mut" ] rust_type | PATH [ "<" rust_type { "," rust_type } ">" ]
            | "[" rust_type "]" | "str" | "Self" | "(" [ rust_type { "," rust_type } ] ")" ;
rust_sig    = "(" [ ( "&" [ "mut" ] "self" | "self" | ID ":" rust_type )
                    { "," ID ":" rust_type } ] ")" [ "->" rust_type ] ;
label_use   = "label" ID { "," ID } ;
effects     = "effects" ( "none" | effect_item { "," effect_item } ) ;
effect_item = ID | "of" ID ;
span        = "for" "call" | "static" [ "per" "thread" ] | "with" ID
            | "until" event { "," event } | "once" | "unknown" ;
event       = [ "next" ] ( ID | "any" ) "(" [ expr { "," expr } ] ")" [ "on" "thread" ] ;

fn_clause   = "error" expr [ "via" expr ] [ "retry" ]
            | "outcome" ID ( "success" | "failure" ) [ "when" expr ] [ "via" expr ] [ "retry" ]
            | "expands" ID "(" [ expr { "," expr } ] ")"
            | "on" ID ID ":" ( "own" | "unset" | "partial" | "not" "consumed" | span )
            | "sizing" [ "query" "(" assigns ")" | "retry" ] [ "for" ID ] "needs" expr
            | effects
            | "thread" ( "any" | "main" | "same" | "same_as" event )
            | "reentrant" ( "yes" | "no" ) | "signal_safe" | "unwind" ( "yes" | "no" )
            | "note" STRING | name_clause | label_use
            | "via" ( "direct" | "shim" | "auto" )
            | "noexcept" | "throws" [ "{" { QNAME "=>" "error" expr [ "via" expr ] } "}" ]
            | "slot" INT | "pure" | "format" "printf" "(" ID ")"
            | "pack" "rest" ( ID | "..." )
            | "pack" "keywords" ( ID | "(" ( ID | "..." ) { "," ( ID | "..." ) } ")" )
            | "keys" "{" { STRING ":" type "," } "}"
            | "since" VERSION | "removed" VERSION ;

name_clause = "source" STRING [ "macro" ]
            | "symbol" ( STRING | "none" ) [ "version" STRING ] [ "when" cond ] ;

(* ---------- language profiles ---------- *)
profile     = "profile" ID "{" { profile_item } "}" ;
profile_item= "memory" mem_kind { "+" mem_kind }
            | "pin" ( "for" "call" | "longer" | "any" | "buffer" )
              "via" ( STRING | "copy" ) [ "release" STRING ]
            | "keep_alive" "via" STRING
            | "string" ID "to" type "via" STRING [ "copies" | "zero_copy" ]
            | "pack" ( "rest" | "keywords" ) "native" STRING
            | "variadic" ( "call" | "define" ) ( "yes" | "no" )
            | "error" "style" ( "exceptions" | "result" | "multi_return" )
            | "bits" "native" STRING
            | "label" ID "->" STRING | "naming" ID ID ;
mem_kind    = "none" | "rc" | "gc" ( "moving" | "nonmoving" ) ;

(* ---------- bindings ---------- *)
binding     = "binding" ID "for" ID ( "import" | "export" ) "{" { binding_item } "}" ;
binding_item= "module" STRING | "strip_prefix" STRING | "name" ID STRING
            | "type" ID "as" STRING | "method" QNAME "=" ID
            | "error" PATTERN "->" STRING | "borrow" ID "as" ( "lifetime" | "copy" )
            | "policy" ID "use" ID | "keywords" ( ID | "*" ) "as" ( "builder" | "struct" | "kwargs" )
            | "label" ID "->" STRING | "hide" STRING | "raw" STRING
            | "provide" QNAME "=" STRING | "on_panic" ( "abort" | "error" expr )
            | "allocator" "own" "->" ID ;
```

(Approximate. Precedence will tighten as the implementation grows. `?` appears
only as the nullable prefix on pointers and handles.)

**No keyword is reserved.** C APIs name their parameters and fields `len`,
`cap`, `size`, `count`, `value`, `text`, `type`, `name`, `flags`, `mode`,
`error`, `version` and `label`, so a contract must be able to use any
keyword as an identifier. Every keyword is contextual:

- a top-level line or block member always starts with a keyword;
- parameter lists, field lists and expressions contain only names in
  name positions.

So a keyword is recognized only where the grammar expects one. Anywhere
else it is an ordinary identifier, and
`fn f(type: c_int, flags: c_int, len: usize, error: out *c_int)` is
valid.

---

## 14. Worked example: a small library, end to end

```writ
library kv
link    "kv"
header  <kv.h>
encoding utf8

allocator kvheap alloc kv_malloc release kv_free

opaque kv_db
opaque kv_iter
handle Db   = *kv_db   release kv_close
handle Iter = *kv_iter release kv_iter_free

enum kv_status : c_int { KV_OK = 0, KV_NOTFOUND = 1, KV_ERR = 2 }

struct kv_entry @size(32) @align(8) {
    key:     *[u8; key_len]   with self   @0
    key_len: usize                        @8
    val:     *[u8; val_len]   with self   @16
    val_len: usize                        @24
}

fn kv_open(path: *cstr, out_db: out *Db) -> c_int as kv_status
  error ret != KV_OK via kv_errstr(ret)
  on error out_db: unset

fn kv_put(db: Db, key: *[u8; klen], klen: usize, val: *[u8; vlen], vlen: usize)
    -> c_int as kv_status
  error ret != KV_OK

# value is copied into a kv-heap buffer the caller owns
fn kv_get(db: Db, key: *[u8; klen], klen: usize,
          out_val: out **[u8; *out_len] own release kv_free,
          out_len: out *usize)
    -> c_int as kv_status
  error ret == KV_ERR
  on error out_val: unset                # KV_NOTFOUND is not an error, but sets nothing

fn kv_iter_new(db: Db, prefix: ?[u8; plen], plen: usize) -> own ?Iter with db
  error ret == null

# the entry is borrowed from the iterator and invalidated by the next advance
fn kv_iter_next(it: Iter) -> ?kv_entry until next kv_iter_next(it), release(it)

fn kv_errstr(s: c_int) -> *cstr static
```

The derived high-level API:

```python
with kv.open("db.kv") as db:          # Db handle → context manager
    db.put(b"k", b"v")                # (ptr,len) pairs collapsed
    v = db.get(b"k")                  # out buffer copied, kv_free called
    for e in db.iter(prefix=b"k"):    # Iter keeps db alive; each entry copied before advancing
        print(e.key, e.val)
```

Note `-> own ?Iter with db`. The iterator is owned by the caller, **and** it
must not outlive `db`. Ownership and span are independent facts, and the DSL
records both.

---

## 15. Design decisions

The load-bearing design choices, each with its rationale and the section
that defines it.

| # | Question | Decision | Where |
|---|---|---|---|
| 1 | Count expressions that call functions | Allowed only for functions marked `pure`. Parameter counts are evaluated before the call. Returned extents are evaluated immediately after it. | §6.1 |
| 2 | Ownership of graphs and arenas | No new construct. One handle owns the whole structure, and every interior pointer is `with` that owner. Ownership that moves is stated on the functions that move it. | §5.7 |
| 3 | Variadic and keyword arguments | Treated as logical packs (`pack rest`, `pack keywords`) carried by one of several lowerings: arrays, tagged arrays, C `...`, `va_list`, options records with a presence encoding, or name/value pairs. Generators lower and lift packs in both directions (import and export). C `...`/`va_list` are bridged by direct or dynamic variadic calls plus generated C shims. A bare `...` is opaque. | §6.6 |
| 4 | Thread-local lifetimes | `static per thread` and `until next f() on thread`. Lifetime is kept separate from which threads may call a function. | §5.3 |
| 5 | Library versions | `version query ... format ...`, plus `since` / `removed` / `when version`. Symbols are bound weakly, and calling one that's missing gives an "unavailable" error at call time. | §4.7 |
| 6 | The meaning of `?` | `?` only ever means nullable. "Not yet determined" is spelled `unknown`. | §5.1 |
| 7 | ABI classification of structs passed by value | Never written in the DSL. Derived per target into the lowered form (`abi_class`) for generators that make raw calls. | §11 |
| 8 | Implementing a C++ interface from Rust | `implementable` classes get a generated C++ shim subclass that forwards to callbacks. Generating vtables directly is out of scope. | §8.6 |
| 9 | Order for enabling direct C++ linking | Stages 0–4, with a fixed list of cases that always use shims. | §8.1 |
| 10 | `moved` vs. `consume` | Kept as separate modes. The caller's obligations differ (still destroy the object vs. never touch it again), and confusing them causes leaks or double destruction. | §5.2, §8.5 |
| 11 | Weak references | No construct. A weak reference is its own handle type, and upgrading it is a function returning `own ?T`. | §4.3 |
| 12 | Importers and trust | v1 imports only clang attributes, counted as confirmed with provenance `import:clang`. GIR, SAL and win32metadata importers are out of scope. Those systems are used only as a corpus for validating the design by hand. | §12.2 |

| 13 | Several headers per library | `header` can be repeated. Headers are parsed together with `define`, `include_path` or `compile_commands`. `use` splits a contract across files. A library with no headers needs explicit layouts. | §3.2 |
| 14 | Scope of `platforms` | Applies to the whole file. Facts hold on every listed platform unless they are inside `when` or carry a per-platform suffix. Any declaration, `refine` included, can be restated per platform. "Platform" (triple) and "target language" are separate terms. | §3.2, §4.7 |
| 15 | Nullable pointer syntax | `?T` replaces `?*T`. `*` and `?` are the two pointer constructors. | §4.2 |
| 16 | Layout rules and bit-level layout | `layout c` (default; assertions optional) or `layout explicit` (all offsets required). No "language X's rules" mode. C bitfields use `bits N` + `@bit`. Manual packing inside an integer uses `as packed { ... @lo..hi }`. | §4.4, §4.9 |
| 17 | Distinct types like `fd` | Integer handles (`handle Fd = c_int none -1 release close`) when a value must be released. `as View` for a distinct type with no ownership. Labels for metadata only. | §4.3, §6.7 |
| 18 | Effects beyond the built-ins | An open set: `effect` declarations with sets and implication, plus `effects of cb`. Built-ins have meaning to generators. User-declared effects go to bindings and tools. Constraints (`thread`, `reentrant`, `signal_safe`) are kept separate. | §6.5 |
| 19 | Data labels | `label` declarations attached to slots. They never change type or calls. Bindings and tools give them meaning. | §6.7 |
| 20 | Global symbols | `var` with `const`/`mut`, `live`, `per thread`, `weak`, `valid after`, ownership, `provided by`, and access discipline. `symbol fn` / `symbol var` for link-only symbols. | §7.4 |
| 21 | Language string ABIs and other idioms | Never in the contract. A small declarative **language profile** per language records strings, slices, packs, memory model, errors, concurrency and naming. | §9.2 |
| 22 | Releasing GC pins | The contract's span decides *when*, and the profile's pin/unpin operations decide *how*. Copying wins where §5.9 allows it and pinning is expensive. | §9.2 |
| 23 | Import vs. export mappings | Separate binding files per language and direction. The contract has no direction, only providers (`interface ... provided by`). Target-language names moved out of the contract into bindings. | §9.1, §9.3 |
| 24 | ABIs other than C | `abi` families. Lifetime, shape, effect and binding layers are family-neutral. Type, symbol and calling layers are per family. `jvm` and `cpython` are designed. Only `c` is built in v1. | §9.4 |
| 25 | Build integration | One `writ` CLI, integrated per build system (Cargo, CMake, Meson, Bazel, Python, JVM, SwiftPM, Go, MSBuild), with no compiler-driver hook. Toolchain facts come from the build or `compile_commands.json`. | §9.5 |

| 26 | Bit-slices and bit-level addressing | Records can be bit-addressed (`layout explicit bits in U lsb0/msb0`, `@bit(N)`, `uN`/`iN`, straddling allowed, inline sub-byte arrays). `access` sets access width and the unit of synchronization. Bit-slices are a shape (`*[bit in U order; n from s]`, plus packed `*[uW ...]`), so they are hidden behind one value in bindings. | §4.9 |
| 27 | Repeated per-function rules | `conventions for fn pattern { ... }` states a rule once. Explicit facts override it, and provenance records it. | §10.4 |
| 28 | Error detail outside the return value | `error_channel` declares a thread- or handle-attached channel once; `error ... via channel` uses it. | §6.4 |
| 29 | Empty arrays and null | `*[T; n]` is non-null even when empty, because slice-building callees require it. `null if empty` and `?[T; n]` relax it. | §6.1 |
| 30 | "Absent" values | One keyword, `none`, names the absent value for handles, structs, scalars and keyword fields. Pointers use null implicitly. | §4.3, §4.6, §6.6 |
| 31 | Rust libraries | Described in Rust's own types (`lang rust`), imported from rustdoc JSON. A generated `extern "C"` transport is the default, and direct linking uses facts derived from the exact build. | §8.9, §12.2 |
| 32 | Implementation scope | A core tier is implemented, and an extended tier is specified but rejected until a library needs it. | §1 |
| 33 | Calls with more than two results | Ordered `outcome`s, each `success` or `failure` with its own predicate, detail and output rules. `error P via X` is shorthand for one failure outcome. | §6.4 |
| 34 | Stale error channels | `reset before call` on a channel; generators clear it before the call | §6.4 |


---

## 16. Prior art

No existing system has all of these together:

- neutral about both the source language and the target language;
- describes an **existing** ABI rather than defining a new one;
- lifetimes that extend past the call;
- layout and symbols verified against the real header and binary;
- C++ called by direct linking.

Each system below covers part of that. This DSL borrows from all of them:

| System | What we take from it | Why we don't adopt it as is |
|---|---|---|
| GObject Introspection | Transfer modes, array annotations, callback scopes, closure/destroy pairing | Tied to GObject; no layout verification, C++, or event spans |
| win32metadata | One machine-readable contract feeding many target languages; per-DLL symbols | Windows only; binary format; thin on lifetime |
| SAL | The array-shape vocabulary (`_Out_writes_to_`), `_Post_invalid_` | MSVC-only; lifetimes end with the call |
| COM IDL / MIDL | `size_is`/`length_is`; the `ref`/`unique`/`ptr` aliasing split; copy-in/copy-out semantics | RPC marshaling model; defines its own world |
| Clang attributes | `counted_by`, nullability, `lifetimebound`, `lifetime_capture_by` | Scattered and specific to one compiler; no release functions |
| Swift C++ interop | Refcounted handles, +1/+0 returns | Only targets Swift |
| SWIG | The idea of mapping idioms (`(char *, size_t)` → string) | Procedural typemaps written per language; always wraps |
| cxx / autocxx / Crubit / Zngur | Rust↔C++ mechanics; shim generation; lifetime import | Rust-only; Crubit depends on Bazel |
| UniFFI / Diplomat / WIT | One contract, many target languages; `own`/`borrow` handles | They define a new ABI; can't describe an existing C library |
| bindgen, cffi, jextract, ffigen | Header → physical layout | No contract layer, which is what this DSL adds |
